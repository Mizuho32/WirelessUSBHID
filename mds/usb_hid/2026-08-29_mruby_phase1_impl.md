# mruby Phase1実装(VM組み込み・ビルド配管)

設計/プランは[[2026-08-28_mruby_filter_route]]参照。このドキュメントは実際にesp32-kvm-ipへ組み込んだ内容と、その過程で踏んだビルド上の落とし穴の記録。

## 実装済み(host/device両ロールでビルド確認済み)

- `esp32-kvm-ip/components/mruby/mruby`: mruby本体をgit submoduleでベンダリング(upstream非改変)
- `esp32-kvm-ip/components/mruby/esp32s3_build_config.rb`: xtensa-esp32s3-elf向けクロスビルド設定(自前ファイル、submodule外)。`stdlib`/`stdlib-ext`/`math`/`metaprog`のみ(bigint/complex/rational/regexp/set/time/random/pack/mruby-socket等は除外)
- `esp32-kvm-ip/components/mruby/CMakeLists.txt`: `rake`をCMakeのカスタムターゲットとして呼び出し`libmruby.a`を生成、`${COMPONENT_LIB}`にリンク
- `main/mruby_filter.c/.h`: VM初期化・スクリプトロード(`main/mruby_scripts/default.rb`をEMBED_TXTFILESで埋め込み)、`filter_keyboard`/`filter_mouse`/`route_*_udp`への`mrb_funcall_argv`往復、`hostname`フック。スクリプトエラー時は`mrb_print_error`でログしてfail-open(filter系)/fail-closed(route系)
- `Kconfig.projbuild`に`CONFIG_MRUBY_FILTER_ROUTE_ENABLE`(Cフォールバックスイッチ、デフォルトy)
- `hid_forwarder.c`: 全hookで`mruby_filter_active()`により mruby / 既存C(`filter_rules.h`/`route_rules.h`)を分岐。Cフォールバック側は無傷のまま残置
- `wifi_manager_init()`のhostname引数はNULL許容に変更(NULLなら`esp_netif_set_hostname()`を呼ばない=noset)。`main_host.c`はコンパイル時定数`WIFI_HOSTNAME "-host"`をやめ、`mruby_filter_init()`をwifi初期化前に呼んで`mruby_filter_hostname()`を渡す
- `partitions.csv`新設(factory 3MB)、`sdkconfig`/`sdkconfig.defaults`を8MB flash + custom partition tableに更新。mruby込みのHost role image(約2MB)に対し3MB中33%空き、Device role(mrubyなし)は73%空き

## ビルドで踏んだハマりどころ(次に似た統合をする際のメモ)

1. **PRIV_REQUIRESでのinclude伝播漏れ**: `main`の`idf_component_register`に`PRIV_REQUIRES mruby`を書いても、`main`のコンパイルコマンドに`-I .../mruby/include`が現れなかった(`esp_driver_uart`/`esp_timer`で既に既知だった問題と同一)。`target_link_libraries(${COMPONENT_LIB} PRIVATE idf::mruby)`で明示リンクして解決
2. **presymヘッダ**: mrubyはgemboxの内容に応じて`mruby/presym/id.h`等をビルド時に生成する(`mruby/build/esp32s3/include/`配下)。`idf_component_register`の`INCLUDE_DIRS`に`mruby/include`だけでなくこの生成ディレクトリも足す必要があった
3. **`-Wl,--wrap=longjmp`とstatic archiveのリンク順**: ESP-IDFはXtensa向けに`longjmp`を全体ラップしている(FreeRTOSコンテキストスイッチとレジスタウィンドウの問題対策、`esp_rom/CMakeLists.txt`)が、ラッパー実体(`esp_rom.a`内の`.o`)は「その時点までに誰かが参照して初めて」static archiveから引っ張られる。mrubyの例外処理(`setjmp`/`longjmp`、`src/error.c`ほか)がこのプロジェクトで初めて`longjmp`を実際に呼ぶコードだったため、リンク順(`libmruby.a`が`esp_rom.a`より後)のせいで未解決参照になった。`-u __wrap_longjmp`で強制解決(このプロジェクトの他の類似ワークアラウンドと同じ手法)
4. **実機で発覚: task "main"のスタックオーバーフロー**(ビルドは通っても実機で落ちた): `app_main()`が動く"main"タスクは`CONFIG_ESP_MAIN_TASK_STACK_SIZE`(デフォルト3584byte)しか無く、`mruby_filter_init()`のVM初期化+Prismパーサーがこれを食い潰した。`sdkconfig`/`sdkconfig.defaults`で8192に変更。同じ理由で、実際にHID入力を処理する側(`hid_forwarder_*()`→`mrb_funcall_argv()`)のタスク、具体的には`usb_host_rp2040_bridge.c`の`dispatch_task`、`usb_host_max3421.c`の`max3421_host_task`、`usb_host_task.c`のHIDドライバbackground task(いずれも元4096byte)も同じクラスの問題を踏む可能性が高いため、先回りして8192に統一した。
5. **実機で発覚: GCアリーナの解放漏れによるヒープリーク**: スタックオーバーフローを直した後、マウスを動かすと確実に`(unknown):0: Out of memory (NoMemoryError)`と、UART側の`checksum mismatch`/`frame len ... exceeds max`(resyncing)が頻発した。原因は`mruby_filter.c`の各hook(`mruby_filter_keyboard_report()`/`mruby_filter_mouse_report()`/`mruby_route_*_also_udp()`)が`mrb_gc_arena_save()`/`mrb_gc_arena_restore()`を呼んでいなかったこと。`mrb_funcall_argv()`が返す一時オブジェクト(スクリプト側の配列リテラル等)がGCアリーナに積まれたまま解放されず、マウスの高頻度呼び出し(`dispatch_task`から`hid_forwarder_mouse_sample()`経由で呼ばれる)でヒープを食い潰し続けていた。ヒープ枯渇がシステム全体に波及し、UARTフレーム処理側のバッファ確保等まで巻き込んで壊れていたとみられる(checksum mismatchはmruby本体のバグではなく、この副作用)。各hookの入口で`ai = mrb_gc_arena_save(s_mrb)`、全returnパスの直前で`mrb_gc_arena_restore(s_mrb, ai)`を呼ぶよう修正。**mrubyをCコードに組み込む際、`mrb_funcall`系を高頻度に呼ぶループがあるなら、アリーナの保存/復元は必須**という一般的な教訓。

## 新規に増えたビルド前提

- **git submodule**: `esp32-kvm-ip/components/mruby/mruby`は`Wireless_USBHID`(親)→`esp32-kvm-ip`(submodule)→`mruby`(その中のsubmodule)という2階層目のsubmoduleになった。フレッシュチェックアウトでは`git submodule update --init --recursive`が要る(`--recursive`を付け忘れると空ディレクトリのままビルドが`mruby.h: No such file`等で失敗する)。
- **Ruby + rake**: mruby自身のビルドシステムが必要。ESP-IDFのxtensaツールチェインとは別に、ビルドを実行するホスト機にRuby(rake同梱)が要る。`components/mruby/CMakeLists.txt`の`find_program(RAKE_EXECUTABLE rake)`が見つからない場合は`FATAL_ERROR`で明示的に落ちるので、専用のセットアップスクリプトは用意していない(READMEに一言添えれば十分と判断)。

## DSL実装(source/sink/pipeline)

Phase1の配管確認が取れたので、[[2026-08-28_mruby_filter_route]]の3節で設計したDSLを実装した。旧来の`filter_keyboard`/`filter_mouse`/`route_*_udp`という6関数API(`main/mruby_scripts/default.rb`・`examples/wheel_to_udp_only.rb`が使っていたもの)は完全に置き換え、両スクリプトともDSL版に書き直し済み。

### 実装した構成

- `source(name, :usb_host, kind: :keyboard/:mouse/:consumer)` / `sink(name, :typec/:udp, ...)` / `pipeline(name) { from(...); to(...) { |ev| ... }; branch(sink) { |ev| ... } }` を`mrb_define_method`でKernelメソッドとして登録
- これらは**スクリプトロード時に一度だけ評価**され、固定長Cの配列(`sink_def_t[8]`、kind別`pipeline_t`×3、各`to`/`branch`最大6段)に解決される。イベントの度にDSLをたどる実装ではない(設計docの「実行時の性能設計」を満たす)
- `to`/`branch`が保持するブロック(Proc)は`mrb_gc_register()`でGCから保護し、VM生存中ずっと使えるようにしている
- ディスパッチ関数(`mruby_dispatch_keyboard/mouse/consumer`)が`hid_forwarder.c`から直接呼ばれ、typec/UDP問わずそのkindの全sinkへの送信を一手に引き受ける(旧来の「filter一発→routeの可否だけ別判定」という2段構えは無くなった)

### 設計docの原案から意図的に簡略化した点

1. **イベント表現はHash、dot記法オブジェクトではない**: `ev.wheel = 0`ではなく`ev[:wheel] = 0`。専用クラス+アクセサをkind毎に自作するコストに見合わないと判断し、mrubyの既存Hash APIをそのまま使った
2. **マウス発火の合成キーボードキー(back/forward→Alt+矢印)は`to`ブロックの出力ではなく、独立したトップレベルフック`mouse_synth_keys(buttons, dx, dy, wheel, pan) -> [modifiers, keycode]`**にした。sink/pipelineは「どこに送るか」に専念させ、「キーボード側の合成状態に何を足すか」は別の関心事として分離
3. **`source`に`:udp`(Device role相当をこのエンジンに統合)は未実装**。`:usb_host`のみ。Host/Device役割の統合は設計docに書いた将来像ではあるが、今回の実装対象からは明示的に除外(スコープ拡大を避けた)
4. **`to`はブロックを共有する複数sink指定**が可能(`to :typec_kbd, :main_pc do |ev| ... end`のように1つの変換結果を複数sinkへ流す)。sink毎に個別クローンして別々に変換、という形にはしていない(設計doc擬似コードの用法とも一致)
5. **`to`にブロックが無ければ無変換の素通しファンアウト**(`to :typec_kbd`だけ)。`branch`は常にブロック必須で、常に生値(他のtoステージの変換の影響を受けない)を見る

### 実装上の注意点(次に触る時のため)

- **ブロック呼び出しは`mrb_yield_argv()`ではなく`mrb_funcall_argv(block, :call, ...)`を使っている**。`mruby_dispatch_*()`はmrubyの呼び出しチェーンの外(素のFreeRTOSタスクコンテキスト)から呼ばれるため、既存の保護フレーム(jmp_buf)が無い。`mrb_funcall_argv()`はそれ自体が保護済みのトップレベル呼び出しであることがPhase1で実証済み(スクリプト内エラーがクラッシュせず`mrb->exc`に載るだけで済んでいた)なのに対し、`mrb_yield_argv()`が同じ保証を持つかは未確認だったため、安全側に倒して`#call`経由で統一した
- 各ディスパッチ関数の中でHashを組み立てる際も、Phase1で踏んだGCアリーナリーク(前述5番)と同じ罠があるため、引き続き`mrb_gc_arena_save`/`restore`を関数全体に掛けている
- スクリプトロードのリトライ(アップロード済みスクリプト失敗→埋め込みdefault.rbへフォールバック)の間で、sink/source/pipelineの登録状態が残ったままだと2回目のロードが壊れるため、各ロード試行の直前に`reset_dsl_state()`で全部クリアしている

### ビルド確認

Host role: `esp32-kvm-ip.bin`は3MBパーティション中33%空き(mruby込み)。Device role(mrubyなし)は73%空き。両ロールともビルド成功。

### 実機で発覚: `sink :udp`のgetaddrinfo()が早すぎてlwIPクラッシュ

`wheel_to_udp_only.rb`(`sink :main_pc, :udp, host:, port:`を含む)をアップロードして実機で試したところ、`NVS initialized`の直後に`assert failed: tcpip_send_msg_wait_sem ... Invalid mbox`でクラッシュした。

原因: `dsl_sink()`が`:udp`のhost/portを即座に`getaddrinfo()`で解決していたが、`mruby_filter_init()`(hostname機能のため`wifi_manager_init()`より前に呼ぶよう変更済み、[[2026-08-28_mruby_filter_route]]参照)の中でスクリプトをロードする時点ではまだ`esp_netif_init()`(lwIPのTCP/IPスレッド起動)が実行されておらず、`getaddrinfo()`がmboxの無いスレッドにメッセージを送ろうとして落ちていた。

対処: `sink_def_t`にhost文字列/portをそのまま保持するだけにして、実際の`getaddrinfo()`は新設の`mruby_filter_resolve_udp_sinks()`に分離、`main_host.c`で`wifi_manager_init()`成功後(`hid_forwarder_init()`の直前)に呼ぶよう変更。未解決の間(`udp_resolved == false`)は該当sinkへの送信は黙ってスキップする(`send_*_to_sink()`のガード)。`hid_forwarder.c`の`resolve_target()`も元々同じ理由でWiFi初期化後に置かれており、今回はそのパターンに揃えた形。

## `:udp` source実装(Host/Device統合の第一歩)

`:udp`をsourceとして実装した。design docで「今回は対象外」としていたピースだが、実際にやってみると既存のkindごと固定パイプライン配列(`s_pipelines[PIPE_KEYBOARD/MOUSE/CONSUMER]`)をそのまま活かせる形に収まった。

### 設計上のポイント

- `:usb_host`ソースは元々kindが`source()`宣言時に固定(物理マウスは常にマウスイベントしか出さない)。一方`:udp`ソースは**1つのlisten portに種類の異なるパケットが混在して届く**(`protocol.h`の`EVENT_TYPE_*`で判別)ため、kindをsource宣言時ではなく`from`呼び出し時に指定させることにした: `from :net_in, kind: :mouse`。design docの擬似コード(1つの`pipeline :from_network`が3種類を`to`で自動振り分け)とは違う形で、**kind毎に別々の`pipeline`ブロックを書く**(`pipeline :net_keyboard do from :net_in, kind: :keyboard; ... end`のように)必要がある — 実装の単純さを優先した簡略化
- ローカル入力用の`s_pipelines[]`とは完全に別の`s_net_pipelines[]`を新設(kindごとに3つ、ローカル用と対で計6つ)。`mruby_dispatch_keyboard/mouse/consumer()`(ローカル、`hid_forwarder.c`から)と`mruby_dispatch_net_keyboard/mouse/consumer()`(ネットワーク、後述の受信タスクから)は共通の`dispatch_*_via(pipeline_t*, ...)`ヘルパーに実体を持たせ、どちらの配列を渡すかだけが違う
- `mouse_synth_keys`フックは**ローカル入力側だけで呼ぶ**(ネットワーク受信したマウスサンプルは送信元ボード側で既に処理済みのはずなので、受信側で再度back/forward判定をするのは二重適用になり不適切と判断)
- 受信タスク(`net_source_task`, 4096byteスタック)は`sink :udp`と同じ理由でWiFi初期化後に`mruby_filter_start_net_source()`から起動(スクリプトが`:udp` sourceを宣言していなければ何もしない)。1ボードにつきlisten portは1つまで(`source`で2個目の`:udp`宣言はエラー)

### サンプル

`main/mruby_scripts/examples/device_role_bridge.rb`を新規作成: ローカルUSB入力(従来通りtype-cへ)に加え、UDPポート9000で受けたHIDイベントもtype-cへ再送する、Host/Device両方を1枚でこなす構成のデモ。

### USB Hostバックエンド選択の完全なmruby化

`device_role_bridge.rb`(RP2040 bridge未接続、UDP受信→type-c転送が主目的の使い方)で気づいた問題: `main_host.c`のバックエンド選択は元々「RP2040/MAX3421E不在ならnative OTGをHost入力として使う」という完全ハードコードのフォールバックで、この場合`typec_capable`が`false`のままになり**`usb_device_typec_start()`が一切呼ばれない**。RP2040/MAX3421EはUART/SPI経由でネイティブUSB-Cポートを消費しないから両立できるが、native OTGフォールバックは同じポートをHost入力に使ってしまうため、type-c出力と物理的に両立できない。

最初は「スクリプトが`:udp` sourceを宣言していれば」という暗黙のヒューリスティックで対処したが、「UDP/RP2040/MAX3421/OTG Hostを完全にmruby向けに抽象化、接続はmrubyでコントロールできるように」という要望を受けて作り直した。

- 新DSL: `usb_host_backends(*syms)` — `:rp2040_bridge`/`:max3421`/`:native_otg`を試行順に列挙。列挙しなかったbackendは無効化される(`:native_otg`を外せばtype-c出力専用に空く)
- スクリプトが`usb_host_backends`を一度も呼ばなかった場合のみ、`mruby_filter_init()`内(スクリプト全体のロードが終わった後)でデフォルトへ解決する:
  - `:udp` source宣言が**ある**場合 → `[:rp2040_bridge, :max3421]`(`:native_otg`を含めない)
  - **無い**場合 → 従来通り`[:rp2040_bridge, :max3421, :native_otg]`
  - **理由**: `:udp` sourceを宣言するスクリプトは「ネットワークから受けた入力をtype-cに出したい」という意図が明確なので、RP2040/MAX3421Eが物理的に無い環境でもnative OTGはtype-c出力専用に空けておきたい(Host入力に取られると`:udp`経由の入力が出力先を失う)。一方`:udp` sourceを宣言しない普通のスクリプト(`default.rb`等)は、`usb_host_backends`という機能自体を知らなくても今まで通りの挙動(RP2040/MAX3421E不在ならnative OTGをHost入力として使う)を維持したいので、そちらはデフォルトを変えない。**要するに「察して切り替える」ヒューリスティックであり、明示的な`usb_host_backends`呼び出しの方が優先される**(ユーザーからは「趣旨とズレてる気がするが実害は無いので残す」という判断をもらった、2026-08-29)
- `main_host.c`は`mruby_filter_host_backend_count()`が0(=mruby非active、VM起動失敗かスクリプト2段階とも読み込み失敗)なら**元のハードコードなprobe順序に完全フォールバック**、そうでなければリストを順に試して最初に起動できたところで止める、というループに書き換え。これが「mruby初期化失敗でpure Cにfallback」の実体
- `device_role_bridge.rb`には`usb_host_backends :rp2040_bridge, :max3421`を明示で追加(デフォルト解決でも同じ結果になるが、この例の要点なので明示した)

### 実機で発覚: `net_source_task`とローカル入力タスクの競合(mrb_stateはスレッドセーフでない)

`device_role_bridge.rb`(ローカルusb_host由来のpipelineと`:udp` source両方を持つ)をRP2040 bridge接続状態で試したところ、`checksum mismatch (msg_type=0x04), resyncing`が大量発生。原因は、`net_source_task`(ネットワーク受信、新設)と`dispatch_task`(RP2040 bridge、ローカル入力)が**同じ`mrb_state`(`s_mrb`)へ別々のFreeRTOSタスクから同時にアクセスしうる**状態になっていたこと。mrubyのVM状態はスレッドセーフではないため、両方が同時にHIDイベントを処理しようとすると内部(GC/ヒープ/アリーナ)が壊れ、以前のGCアリーナリークと同じ経路でヒープ破損→UART側のchecksumまで巻き込まれる形で症状が出た。

対処: `s_mrb_mutex`(`xSemaphoreCreateMutex()`)を追加し、`mruby_dispatch_keyboard/mouse/consumer()`と`mruby_dispatch_net_keyboard/mouse/consumer()`の6関数全てで、本体を`xSemaphoreTake`/`xSemaphoreGive`で挟むようにした。ローカル入力とネットワーク入力の両方を使うスクリプト(`device_role_bridge.rb`等)を書く場合、この手当てが無いと確実に踏む問題だったので、DSL側に両方の口(source/pipeline)を用意する以上、C側で排他制御するのは必須の対応だった。

### 実機で発覚: `from`の引数仕様(aspec)の更新漏れ

`device_role_bridge.rb`をアップロードすると`from: wrong number of arguments (given 2, expected 1) (ArgumentError)`で読み込み失敗、Cフォールバック(埋め込み`default.rb`)に落ちていた。

原因: `dsl_from()`を`:udp` source対応(`from :net_in, kind: :mouse`のオプションHash引数を受けられるよう`mrb_get_args(mrb, "n|H", ...)`に変更)した際、`mrb_define_method()`に渡す引数仕様(aspec)を`MRB_ARGS_REQ(1)`のまま更新し忘れていた。mrubyは呼び出し時にaspecでまず引数個数をチェックするため、C関数の中身(`mrb_get_args`)が対応していても、aspec側が「引数1個ちょうど」を要求している限り2引数呼び出しはC関数に入る前に弾かれる。`MRB_ARGS_ARG(1, 1)`(必須1+任意1)に修正。

他のDSLメソッド(source/sink/pipeline/to/branch/usb_host_backends)のaspecと`mrb_get_args`の対応も一通り確認し、ズレていたのは`from`だけだった。**教訓**: `mrb_get_args`のフォーマット文字列を変更したら、対応する`mrb_define_method`のaspecも必ず同時に見直すこと。

### checksum mismatch再発の件、再現性なし(電源供給を疑う)

`from`のaspec修正後、`device_role_bridge.rb`アップロード→ローカルマウス操作のみで`frame len ... exceeds max 512`/`checksum mismatch (msg_type=0x55)`が再発。切り分けのため`default.rb`(`:udp` source無し、`net_source_task`が一切起動しない)に切り替えて同条件で試したところ発生せず。ところが`device_role_bridge.rb`に戻して再試行したら**今度は正常**——つまり`:udp` source/`net_source_task`の有無で再現性が変わったわけではなく、単発の不安定事象だった可能性が高い。MCUへの電力供給が絡んでいる可能性がユーザーから指摘されている(RP2040 bridge関連の別調査でも電源起因の不安定挙動が過去に疑われている)。今回のmruby側の変更(mutex追加、`:udp` source実装)が原因と断定できる材料は無し。実機側の電源まわりの問題である可能性を念頭に置きつつ、再発時は改めて条件(電源、USBケーブル/ハブ構成など)を記録した方が良さそう。

### ビルド確認

Host role: 33%空き。実機テストは未実施(受信側の動作確認、ローカル/ネットワーク両経路が輻輳した際の挙動などは要検証)。

## 未着手

- Phase2: LittleFS/SPIFFSスクリプトパーティション + WebUI編集
- 実機での動作確認一式(ユーザー側で実施予定):
  - DSL版スクリプト(`default.rb`・`wheel_to_udp_only.rb`)が実際に動くか(特に`wheel_to_udp_only.rb`でwheelがtype-cから消えてUDPにだけ出るか)
  - `to`/`branch`ブロックの`mrb_funcall_argv(..., :call, ...)`往復コスト(旧6関数APIより1段呼び出しが増えている分、実測が必要)
  - GCアリーナ管理が新しいHash組み立てパターンでも機能しているか(マウスを動かし続けてOOM/checksum mismatchが再発しないか)
  - `:udp` source(`device_role_bridge.rb`)の受信動作、`net_source_task`のGC/アリーナ挙動
