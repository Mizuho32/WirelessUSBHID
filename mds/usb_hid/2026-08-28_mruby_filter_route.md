# filter/conv/routeをmrubyでスクリプト化したい

## 概要

現状の`filter_rules.h`/`route_rules.h`(gitignoreされた個人カスタマイズ、[[2026-08-21_filter_conv_route]]・[[2026-08-23_filter_conv_router_with_max3421]]参照)は、C関数を直接編集して`idf.py build && idf.py flash`し直す方式。困っているのは:

- 挙動を変えるたびにビルド+reflashが要る(iterationが重い)
- 凝った条件分岐を書こうとするとCで書くのが面倒(状態機械、閾値、複数条件の組み合わせなど)

これをmrubyスクリプトに置き換えられないか、というのが本題。動機は「reflash無しで挙動を変えたい」であって、「非プログラマにも設定させたい」ではない(自分専用の道具のまま)。

さらに検討を進めた結果、単なるCのポート先ではなく、**src/sinkを抽象化したDSL**にして`KVM_ROLE=HOST/DEVICE`のコンパイル時分岐やターゲット固定(`KVM_TARGET_HOST`)自体もスクリプト側に持ち上げる方向で設計する。

## 想定ハードウェア(2026-08-28更新)

- ESP32-S3、フラッシュ8MB・オンチップ(パッケージ内蔵)PSRAM 8MB想定。この容量ならmruby VM+スクリプトを載せてもフラッシュ/RAM双方に十分な余裕がある。**懸念の中心は容量ではなく実行速度**に移る。
- 現行`sdkconfig.defaults`は`CONFIG_ESPTOOLPY_FLASHSIZE=2MB`のままなので、実機がこの想定通りなら設定を合わせて更新する必要がある(パーティションテーブルも合わせて拡張)。
- 「オンチップPSRAM」は同一パッケージに封入されているだけで、CPUコアと同じダイ上のレジスタ/内蔵SRAM(実質512KB程度)とは別物。Octal SPI経由でアクセスする外付けメモリダイであることに変わりはなく、内蔵SRAMより物理的に低速。ただしFlashキャッシュと共有の32〜64KBキャッシュがあり、小さく頻繁に触るワーキングセット(mrubyのGCヒープのような用途)はキャッシュに乗ればSRAMとほぼ同速、キャッシュに収まらない/スラッシングする場合は素のPSRAM速度(80〜120MHz駆動)まで落ちる。実機での実測が必要な点は変わらない。`heap_caps_malloc(MALLOC_CAP_INTERNAL)`でVMヒープを内蔵SRAM側に固定できるかは確認しておきたい。

## フィージビリティ調査結果(2026-08-28、mrubyクロスビルド実測)

実際に`mruby`(upstream head)をxtensa-esp32s3-elf-gcc(ESP-IDFのtoolchain)でクロスビルドして検証した。

- **移植性の問題は無かった**: `stdlib`/`stdlib-ext`/`math`/`metaprog`のgemboxで、コア含め全てエラー無くコンパイル・アーカイブまで成功。唯一失敗したのは`mruby-socket`(`sys/socket.h`依存)だが、そもそもスクリプト側にネットワークI/Oを持たせる設計ではない(ネットワーク送受信はCのhid_forwarder.c側が担当、mrubyは加工ロジックのみ)ので単純に対象から外せば良い。
- **サイズ内訳**(`.text`、リンク前のオブジェクト単位、`--gc-sections`適用前の粗い上限値):
  - VMコア(`src/`): 約300KB(デバッグ用ダンプ機能等を削れば減らせる)
  - `mruby-compiler`(Prismパーサー、オンデバイスでスクリプトをコンパイルする機能): **423KB**、単体で最大のブロック
  - 不要にできそうなgem(bigint/complex/rational/regexp/set/time/random/pack等、フィルタ/ルーティングDSLには基本不要): 合計150KB超
- **オンデバイスコンパイラを積むか外すか**という選択肢がある: 外して「スクリプトはPC上で`mrbc`によりバイトコード化してからアップロード」にすればサイズは半分近くに減らせるが、WebUI(Phase2)での「生の`.rb`テキストをブラウザで編集してすぐ反映」という一番やりたいワークフローには**オンデバイスコンパイラが必要**。8MB/8MB想定なら423KBは十分許容範囲なので、**オンデバイスコンパイラは積んだままにする**方針とする。
- ビルドにはRuby(mrubyの`rake`ベースビルドシステム用)が必要。開発機には既にRuby 3.4.4 + rakeが入っているため問題ないが、ESP-IDFのCMakeビルドから見るとmrubyは「別ビルドシステムを内包した外部依存」になる点は留意(後述の組み込み方針を参照)。

## 設計案

### 1. スクリプトの保存場所

LittleFS/SPIFFSパーティションにスクリプト(.rb)を置く。人間が読み書きする単位がテキストファイルなので自然。パーティションテーブルに`storage, data, littlefs, ...`を追加する形。

### 2. スクリプト更新経路(フェーズ分け)

reflash無しで挙動を変える、という動機を満たす順に育てる。**HIDイベント自体は既存通りUDP(best-effort、低遅延優先)のままでよいが、スクリプト転送は信頼性が要る(欠落・順序保証が必要なテキスト転送)ので、同じUDPプロトコルを流用せず別経路にする**。

- **Phase 1: シリアル書き換え** — スクリプトパーティションだけをUART経由で書き込む(`esptool.py write_flash`相当)。app本体の再ビルドは不要になるので、これだけでも「Cを直して再ビルド」より大幅に軽い。最初の一歩として一番実装コストが低い。
- **Phase 2: WebUI編集** — ESP-IDFの`esp_http_server`でHTTPサーバを立て、ブラウザからスクリプトを編集・保存できるページを提供する。POSTでスクリプト本文を受け取り、ストレージパーティションに書き込んでVMをリロード。HID用のUDPと違い、これはTCP(HTTP)ベースなので転送の信頼性はスタック任せにできる。無線かつビルド不要という点で、実質的にここが「本当の意味でreflash無し」の到達点になる。
- **Phase 3(任意)**: WebUIにシンタックスチェック(mrubyパーサーだけ先に走らせて構文エラーを保存前に弾く)やスクリプトのプレビュー/バージョン履歴(直前のスクリプトを1世代だけ保持してロールバックできる、程度で十分)を足す。ここは余力があれば。

### 3. Src/Sinkの抽象化: DSL設計

今の実装は「Host役はUSBを読んでUDP/type-cへ出す」「Device役はUDPを受けてtype-cへ出す」という**役割がコンパイル時(`KVM_ROLE`)に固定**されている上に、送信先も`KVM_TARGET_HOST`という単一の固定値。これをsource(信号の出どころ)とsink(信号の行き先)を対等な部品として名前で定義し、その間をパイプラインでつなぐ形にすると、Host/Device roleという区別自体が「たまたまそういうパイプラインを書いた」だけになり不要になる。gstreamerの`src ! filter ! sink`やPulseAudioのソース/シンク間ルーティングと確かに同じ発想。

擬似コード案:

```ruby
# ---- Source定義: 信号の出どころ ----
source :local_kbd,   :usb_host, kind: :keyboard
source :local_mouse, :usb_host, kind: :mouse
source :local_cc,    :usb_host, kind: :consumer

source :net_in, :udp, listen: 9000        # 他基板からのHIDイベントを受ける側(今のDevice role相当)

# ---- Sink定義: 信号の行き先 ----
sink :typec_kbd,   :typec, kind: :keyboard
sink :typec_mouse, :typec, kind: :mouse
sink :typec_cc,    :typec, kind: :consumer

sink :main_pc,   :udp, host: "192.168.1.50", port: 9000   # 今のKVM_TARGET_HOST相当、これがN個に増やせる
sink :second_pc, :udp, host: "192.168.1.51", port: 9000

# ---- Pipeline: source -> [stage...] -> sink(複数可、fan-out) ----
pipeline :mouse do
  from :local_mouse

  # type-c向けだけを加工するstage(今のfilter_rules.hのmouse相当)
  to :typec_mouse do |ev|
    ev.wheel = 0 if ev.wheel != 0   # type-cからはwheelを消す
    ev                              # nilを返せばこのsinkへは送らない(drop)
  end

  # 生値を見て条件付きで別sinkへ(今のroute_rules.hのmouse相当、filter後ではなく元イベントを見る)
  branch(:main_pc) { |ev| ev.wheel != 0 || ev.pan != 0 }
end

pipeline :keyboard do
  from :local_kbd
  to :typec_kbd, :main_pc do |ev|
    ev.remap(CAPS_LOCK => :left_ctrl)
  end
end

# ---- 今のDevice role相当は、単に source が :udp で sink が :typec のpipeline ----
pipeline :from_network do
  from :net_in
  to :typec_kbd, :typec_mouse, :typec_cc   # kindでkeyboard/mouse/consumerに自動振り分け
end
```

ポイント:

- `source`/`sink`は名前空間だけの宣言で、実体(USB Hostから読むのか、type-cへ出すのか、UDPで送受信するのか)はC側のドライバに紐づく。スクリプトはどれとどれをどう繋ぐかだけを書く。
- `to`ブロックが今の`filter_mouse_report()`相当(そのsink向けにだけ効く加工)、`branch`が今の`route_*_also_udp()`相当(生イベントを見て追加のsinkへ流すかどうか)。この非対称性([[2026-08-21_filter_conv_route]]で決めたマウスだけの雑仕様)が、DSLでは「to = 加工付き接続」「branch = 条件付き無加工接続」という2種類の接続として自然に一般化される。keyboard/consumerも同じ書き方で書けるので、今Cコード側にある「マウスだけ非対称、keyboard/consumerは未対応」というギャップも解消できる。
- `sink :main_pc`を増やすだけで複数Target PCへのfan-outになる(今の「`KVM_TARGET_HOST`は1つだけ」という制約の解消)。`source :net_in`を足せるので、1枚の基板が同時に「ローカルUSBを読む」と「他基板からのネットワーク入力を受けてtype-c出力する」を両方できる(今はHOST/DEVICEどちらか一方の役割にコンパイル時固定)。

#### 実行時の性能設計(最重要)

DSLの`pipeline`/`source`/`sink`定義はスクリプトロード時に一度だけ評価し、「このsourceのイベントが来たら呼ぶべきstage/sinkの一覧」を**事前に平坦なテーブルへ解決**しておく。イベントが来るたびにDSLのグラフをたどるような実装は禁止(まさに現状ドキュメントの「一番のリスク」節で挙げた懸念に直結する)。C側から見ると、ロード時に「mouseイベント用のコールバックリスト」を1回だけ構築し、実行時は単純にそのリストを順に`mrb_funcall()`するだけ、という形にする。DSLの表現力と実行時コストを分離するのがこの設計の肝。

### 4. mruby処理のON/OFFスイッチ(Cフォールバック)

万一スクリプトが壊れている/VM初期化に失敗する/性能が出ない場合に備え、mruby経由の処理を丸ごと無効化してCの`filter_rules_default.h`/`route_rules_default.h`相当の素通し(またはgitignoreされた`filter_rules.h`/`route_rules.h`があればそちら)にフォールバックするスイッチを設ける。

- **自動フォールバック**: VM初期化失敗・スクリプトのパース/構文エラー時は、ログを出した上で自動的にC版へフォールバック(起動時に毎回落ちるより安全側に倒す)
- **手動スイッチ**: `sdkconfig`のKconfigオプション(ビルド時、例`CONFIG_MRUBY_FILTER_ROUTE_ENABLE`)に加えて、NVSに1bitのランタイムフラグを持たせ、WebUI(Phase2)から「mruby処理を切る」をトグルできるようにする。実機で挙動がおかしい時に、reflash無しでC版に戻せることが重要(この機能自体がreflash運用をなくすためのものなので、フォールバック手段だけreflashが要る、では本末転倒)
- 呼び出し側(`hid_forwarder.c`)からは今の`__has_include`分岐と同じ形に見せる: 「mruby有効か」を1箇所でチェックし、無効ならこれまで通りCの`filter_rules.h`/`route_rules.h`パスへ、有効ならmruby VM経由、という単純なif分岐にする

### 5. hostnameのmruby移行

現状`wifi_credentials.h`の`WIFI_HOSTNAME`(コンパイル時定数、`main.c`ではそのまま、`main_host.c`では`"-host"`suffix付き)を`wifi_manager_init()`に渡し`esp_netif_set_hostname()`している。これは**1ビルド=1ホスト名固定**であり、同じfirmwareイメージを複数台にそのまま載せる(今回のmruby化の一因でもある「複数デバイス搭載」)という方向性と相性が悪い。

- `WIFI_HOSTNAME`の`#define`によるコンパイル時固定は廃止する
- 代わりにmrubyスクリプト側にhostname設定用のDSL関数を設ける(例: `hostname "my-device-host"`)。スクリプト側で値をセットしていればそれを`esp_netif_set_hostname()`に渡し、**セットされていなければ何もしない(デフォルトnoset)** — 素の(mDNS/DHCP的に)チップ既定のホスト名のまま、という扱いにする
- これにより同一firmwareイメージを複数台へ配布し、各台固有の設定(hostname含む)はmrubyスクリプト側(WebUI経由で個体ごとに編集)に寄せられる。他の個体差(role相当の振る舞い含む)も将来的に同じ場所に集約していく方向性と一致する

## 一番のリスク: ホットパスの実行速度

容量(フラッシュ/RAM)は想定ハードウェア(8MB/8MB)なら十分。残る最大の懸念は**マウスの高頻度経路での`mrb_funcall()`往復コスト**。`hid_forwarder_mouse_sample()`は数百Hz級で呼ばれ、[[2026-08-24_rp2040_bridge_fps_investigation]]はまさにこの経路のレイテンシ/FPSを詰めた調査だった。そこで潰した遅延を、mruby VM経由のディスパッチやGC停止で再び持ち込むリスクがある。実装前に潰すべき未知数:

- 1回の`mrb_funcall()`往復コストの実測(前述の通り事前にコールバックテーブルへ解決した上での、1呼び出しあたりのコスト)
- GCタイミングがレポート処理と衝突して単発の遅延スパイクを起こさないか(インクリメンタルGCの設定、あるいは`mrb_full_gc`を明示的にこちらで制御できるか)
- mruby VMのヒープをSRAM/PSRAMどちらに置くかで速度がどれだけ変わるか(前述「想定ハードウェア」節参照)
- 最悪、マウスだけはCのまま(現状の`filter_rules.h`/`route_rules.h`路線を維持)にして、頻度の低いkeyboard/consumer/pipeline定義そのものだけmruby化する、という切り分けも選択肢としてありうる

## 既存パターンとの関係

- `filter_rules.h`/`route_rules.h`の「gitignoreされた個人コピー、無ければ`_default.h`にフォールバック」という`__has_include`パターン([[2026-08-21_filter_conv_route]])とmrubyスクリプトは並存できる: スクリプトパーティションが存在しない/読み込み失敗したらCの`_default.h`(今の素通し/type-c onlyのデフォルト)にフォールバックする形で、今の安全側デフォルトの思想を引き継げる。
- README.mdの「filter/conv/routeの編集場所」セクションは、mruby化した場合ここに「スクリプトの置き場所・WebUIのURL・DSLのリファレンス」を追記する形になる想定。

## 未着手の次のアクション

1. ~~mrubyがxtensa-esp32s3-elf向けにクロスコンパイルできるか~~ → 確認済み(前述フィージビリティ調査結果)。以後は`esp32-kvm-ip`本体への組み込み作業
2. 実機で`esp_get_free_heap_size()`(SRAM/PSRAM別)と`esptool.py flash_id`を確認し、8MB/8MB想定を検証する(ユーザー側で実機テスト予定)
3. 上記DSLの「ロード時にコールバックテーブルへ解決」という設計方針のもとで、`mrb_funcall()`往復コストを実機で計測し、マウス高頻度経路に耐えるか判断する(ユーザー側で実機テスト予定)
4. 耐えるなら全イベント種別をmruby化、耐えないならマウスだけCに残す設計に倒す

## Phase1実装メモ(方針)

最初のPhase1は「DSLのフル実装」ではなく、**mrubyの配管(VM組み込み・ビルド・スクリプトロード・Cフォールバック)を実機で通す**ことを目標にする。スクリプト自体は今の`filter_rules_default.h`/`route_rules_default.h`相当の素通し(UDP/type-cへダダ流し)を書くだけに留め、DSL(source/sink/pipeline)そのものの作り込みはPhase1が通ってから。

- mrubyは`esp32-kvm-ip`配下に別コンポーネントとしてベンダリング(サブモジュール等)し、mruby自身の`rake`ビルドをESP-IDFのCMakeから外部ステップとして呼び出し、生成された`libmruby.a`を本体にリンクする方針(ビルドにRubyが要る点はビルド手順書に明記する)
- 前述の4節(Cフォールバックスイッチ)・5節(hostnameのmruby移行)は、DSL本体の作り込みを待たずにPhase1の時点で骨組みだけ入れておく

実際の実装内容・ビルドで踏んだハマりどころは[[2026-08-29_mruby_phase1_impl]]参照。

### スクリプトのアップロード手順

Phase 1のシリアル書き換え(前述)は`bin/upload_mruby_script.py`として実装済み。使い方:

1. ファームウェアを一度flash(いつも通り): `bash bin/build_host.sh build flash`。この時点で`mrb_script`パーティション(`partitions.csv`)も書き込まれるが中身は空(消去状態)なので、起動時は`main/mruby_scripts/default.rb`が使われる。
2. スクリプトをアップロード(reflash不要、`mrb_script`パーティションだけ書き換え):
   ```
   export ESP_IDF=/opt/esp-idf && source "$ESP_IDF/export.sh"
   python3 bin/upload_mruby_script.py --port /dev/ttyACM0 \
       esp32-kvm-ip/main/mruby_scripts/examples/wheel_to_udp_only.rb
   ```
   内部で`parttool.py write_partition --partition-name mrb_script`を呼んでいるだけなので、esptoolと同じ自動リセットでブートローダに入る(ボタン操作は通常不要)。`idf.py monitor`等がポートを掴んだままだと衝突するので、アップロード前に閉じておく。

   **ポート指定を間違えると`Connecting......`で無限に固まる**: XIAO ESP32S3は書き込み(flash/parttool)がネイティブUSB-Cポート(`/dev/ttyACM0`)、コンソール/monitorは`sdkconfig.defaults`で別途UART0に逃がした外付けUSB-UARTアダプタ(`/dev/ttyUSB0`)、と**別ポート**になっている(`sdkconfig.defaults`のコメント参照)。`--port`にコンソール側の`/dev/ttyUSB0`を渡すとEN/IO0に繋がっていないためリセットが効かず延々とシンク待ちで固まる。`bin/build_host.sh ... flash`で実際に使われたのと同じポート(`/dev/ttyACM0`)を指定すること。
3. `mruby_filter_init()`は起動時に一度だけ`mrb_script`を読むので、書き込み後に一度リセット/電源再投入が要る。
4. `idf.py -p /dev/ttyACM0 monitor`のログで反映を確認:
   - `Loaded uploaded script from mrb_script partition (no reflash)` — アップロード成功
   - `Loaded embedded default.rb (no mrb_script uploaded)` — 未アップロード/空
   - `Uploaded mrb_script failed to load, falling back to embedded default.rb` — パース失敗
