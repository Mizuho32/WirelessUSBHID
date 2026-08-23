# MAX3421を使ったHID変換

## 概要

昨日までの試行錯誤で、現状のESP32 USBでは一部デバイスからの信号を正確に取得できないことがわかった。  
ただ、そもそもこのプロジェクトをやり始めた動機として、複数HIDを接続し、filter/conv/route、をしたい、というのがあり、これができればなんでもいい。  
ここでは、MAX3421を使い、USB Hostとして機器を読み取り、PCへと信号を送る。

### 要件
- 基本的には `2026-08-21_usb_host.md' にある要件と同じ
- 複数HID(Hubにぶら下がっている)を解釈可能
- 9ボタンマウスの件もケアされている
- HID機器 → MAX3421 → filter/conv/route → ESP32 type-c (device) or UDP
- filter/convは実装してもらったが、ここで、routeのルールによりどこに送るか定義(filterと一部被っている気もするが)
- Hostのコードに追加実装(MAX3421がつながっていたらこのモードにする?)がいいと考えているが、要検討ポイント

### 調査結果

#### 朗報: TinyUSBに既にMAX3421 Hostドライバが存在し、しかもこのプロジェクトが既に依存している

`esp32-kvm-ip/managed_components/espressif__tinyusb`(現状 `espressif/tinyusb` v0.21.0、`idf_component.yml`にDevice role用として既に依存済み)のソースツリーを確認したところ、`src/portable/analog/max3421/hcd_max3421.c`が**そのまま入っていた**。TinyUSB本家(hathach/tinyusb)はMAX3421EをSPI経由のHost Controller Driver(HCD)としてサポートしており、Espressifが取り込んでいるツリーにもそのファイル自体は含まれている。

さらに重要なのは、これが**RP2040クロステストで実際に「7バイート丸ごと正しく届く」ことを実機確認済みの、あの`tuh_hid_*` APIと全く同じTinyUSB Hostスタック**だということ(`mds/2026-08-22_rp2040_host_check.md`)。つまりMAX3421採用は単なる「別チップで試してみる」ではなく、**既に正しいと分かっているソフトウェアスタックを、ESP32上でもそのまま使う**という話になる。ESP32-S3のネイティブUSB-OTG(DWC_OTG)+`espressif/usb_host_hid`という組み合わせ固有の問題(4つの仮説を潰してなお原因不明、`mds/2026-08-22_esp_idf_usb_host_known_issues.md`)を、ハードごとバイパスする形になるので、**3バイート切り詰め問題そのものも副次的に解決する可能性が高い**。

必要なボードAPI(アプリ側で実装する関数)はたった3つ:
```c
extern void tuh_max3421_spi_cs_api(uint8_t rhport, bool active);
extern bool tuh_max3421_spi_xfer_api(uint8_t rhport, uint8_t const* tx_buf, uint8_t* rx_buf, size_t xfer_bytes);
extern void tuh_max3421_int_api(uint8_t rhport, bool enabled);
```
ESP-IDFの`driver/spi_master.h`(SPIマスター)+ GPIO割り込み(INTピン、MAX3421はアクティブLowレベル割り込み)で素直に実装できる規模。

#### 落とし穴: EspressifのCMakeLists.txtはDevice modeしかビルドしない

`managed_components/espressif__tinyusb/CMakeLists.txt`を見ると、`srcs`リストには`*_device.c`系と`dcd_dwc2.c`(Device Controller Driver)、`usbd.c`(Device stack本体)しか入っておらず、**Host側(`src/host/usbh.c`, `src/host/hub.c`, `src/class/hid/hid_host.c`, `src/portable/analog/max3421/hcd_max3421.c`等)は一切コンパイル対象に入っていない**。ソースは丸ごと含まれているのに、EspressifパッケージはDevice専用ラッパーとして固定されている、ということ。

つまり`CFG_TUH_MAX3421`のようなKconfig/マクロを立てるだけでは動かず、**Host側ソースをビルドに含める別経路**が要る。選択肢:
1. `managed_components/`配下のCMakeLists.txtを直接書き換える — 手軽だが、`managed_components`はコンポーネントマネージャが再解決時に上書きしうる領域なので、恒久対応としては脆い(gitignore対象でもある)。
2. 本家TinyUSB(`hathach/tinyusb`)を`third_party/`等にgit submoduleで追加し、Host mode(+MAX3421)用に**独自のESP-IDFコンポーネント**(自前CMakeLists.txtで`srcs`にHost系ファイル一式を明示的に追加)としてラップする。EspressifのDevice専用パッケージとは完全に別物として共存させる。
3. (参考)Arduino的な`USB_Host_Shield_2.0`ライブラリを使う手もあるが、これは独自のHIDクラスドライバ・記述子パーサーを持つ全く別のスタックで、`hid_report_parser.c`/`filter_rules.h`等の今の資産をほぼ再利用できなくなるので不採用が妥当そう(TinyUSB Host経由なら`tuh_hid_report_received_cb`から先は今の資産をほぼそのまま使い回せる)。

→ **2番(本家TinyUSBをsubmoduleで独自コンポーネント化)が本命**。EspressifパッケージをHost用に無理やり改造するより、上流ソースをそのまま素直に使う形が壊れにくい。

#### ハードウェア面の注意点
- **VBUS給電**: MAX3421E自体はSIE(プロトコル処理)のみで、VBUS(5V)のソーシングは別回路(昇圧+ロードスイッチ)が要る。ESP32/RP2040の内蔵USBポートと全く同じ制約。既製のMAX3421ブレイクアウト(例: Circuits@Home系のUSB Host Shield 2.0互換ボード)は大抵オンボードでVBUSスイッチを持っているので、素のMAX3421E単体チップより「USB Host Shield」的な完成品ボードを選んだ方が配線が楽(なければ今回のRP2040と同じジャンパー直結でも検証は可能)。
- **配線**: SPI(MOSI/MISO/SCK/CS)+ 別途INT ピン(必須、Lowアクティブの割り込み)。RESETピンも配線推奨。SPIクロックはMAX3421Eのデータシート上限(26MHz程度)を超えないこと、モードはSPI Mode 0。
- **Hub/複数デバイス**: TinyUSBの`CFG_TUH_HUB`は当然使えるが、ESP32-S3ネイティブOTGの「8Hostチャンネル固定」という制約とは性質が異なる — MAX3421は物理的に1つのSIEをソフトウェアで時分割している(`max3421_ep_t ep[CFG_TUH_MAX3421_ENDPOINT_TOTAL]`という配列で管理、サイズは設定可能)ので、チャンネル数の上限は緩められる代わりに、**全転送がSPI経由で直列化される**(並列ハードウェアDMAではない)。キーボード+マウス程度の低帯域なInterrupt転送であれば実用上まず問題にならないはず。

#### 既存コードとの関係(実装への影響)
- `hid_report_parser.c`(HID Report Descriptorのフィールド抽出)・`filter_rules.h`(filter/conv)・`protocol.h`+UDP送信ロジック(`send_keyboard_report`/`send_mouse_report`/`send_consumer_report`)は**ドライバに依存しない共通資産**として、ほぼそのまま再利用できる。
- 書き直しが要るのは`usb_host_task.c`の「デバイス接続時の種別判定」「生レポート受信→各handle_*_report呼び出し」の部分だけ。今の実装は`espressif/usb_host_hid`のAPI形状(`hid_host_device_handle_t`、`hid_host_device_get_raw_input_report_data`等)に依存しているので、TinyUSBの`tuh_hid_mount_cb`/`tuh_hid_report_received_cb`(`dev_addr`, `instance`, `report`, `len`)という形状に合わせて書き直すことになる。ちょうど`rp2040_host_check/rp2040_host_check.ino`でやったのと同じAPIなので、あちらでの実装がほぼそのまま雛形として使える。
- 9ボタンマウスの「3バイート切り詰め+Report ID抜けConsumer usage」quirk対応コード(`last_boot_consumer_usage`まわり)は、TinyUSB Host経由なら本来のReport ID付き7バイートがそのまま届く見込みが高いので、**恐らく丸ごと不要になる**(要実機確認)。

#### 決定: 別roleではなく、既存`KVM_ROLE=HOST`の中でランタイム自動検出+フォールバック

要件にあった「MAX3421がつながっていたらこのモードにする?」に対する回答: **別ビルド(別`KVM_ROLE`値)にせず、同一Hostビルド内でランタイム自動検出+フォールバックにする**方針でOK。

- MAX3421Eの有無は起動時にSPIで安全にプローブできる(既知レジスタへの書き込み→読み返し等)。挿さっていなければ応答が返らないだけなので、native OTG側と衝突するリスクなく判定できる。
- native OTGの`usb_host`/`espressif/usb_host_hid`スタックと、MAX3421用に独自ラップするTinyUSB Hostスタックは、**物理的に別バス(native OTGピン vs SPI+GPIO)を使う独立した状態機械**なので、同じバイナリに両方リンクしても機能的な衝突はない。フラッシュ/RAM増加のみのコストで、現状のHostビルド(1MBパーティションの20%程度使用)には十分余裕がある。
- 起動シーケージは概ね: SPIプローブ → MAX3421応答あり → TinyUSB Host(MAX3421 HCD)を起動、無し → 従来通りnative OTG(`usb_host_hid`)を起動、という単純な二択にできる。両方を同時稼働させる必要はない(将来「native OTGポート+MAX3421ポートの両方に別デバイスを挿して同時に使う」という拡張も設計上は可能だが、それは要件外)。
- 唯一の実装上の注意: `tusb_config.h`は現状Device role専用の単一ファイル(`CFG_TUSB_RHPORT0_MODE=OPT_MODE_DEVICE`)。MAX3421用には`CFG_TUH_ENABLED`/`CFG_TUH_MAX3421`を有効にした別設定が要るので、TinyUSBの`CFG_TUSB_CONFIG_FILE`という「コンパイル定義でconfigヘッダのパスを差し替える」機構を使い、Host role専用の設定ヘッダ(例: `tusb_config_host.h`)を用意する。1ファイル共有はできないが、詰まるような話ではない。

## Phase1 実行可能性検証
まず、要件のMAX3421で正常に読み取ってtype-cに流す、という動作を確認できるとこまでやる。

### 提案する実装ステップ
1. **ハード調達**: MAX3421Eブレイクアウト(できればVBUSスイッチ内蔵のもの)。SPI(MOSI/MISO/SCK/CS)+INT(+RESET)をESP32-S3のGPIOに配線。
2. **TinyUSB Host用コンポーネントの用意**: 本家`hathach/tinyusb`を`third_party/tinyusb`辺りにgit submodule追加し、Host系ソース(`src/tusb.c`, `src/host/usbh.c`, `src/host/hub.c`, `src/class/hid/hid_host.c`, `src/portable/analog/max3421/hcd_max3421.c`, `src/common/tusb_fifo.c`等)を明示的にビルドする独自CMakeLists.txtのESP-IDFコンポーネントとしてラップする(Espressifパッケージとは別物として共存させ、既存Device roleには一切触れない)。`KVM_ROLE=HOST`のビルドにのみリンクする(Device roleは無関係)。
3. **最小疎通確認**: まずは既存`usb_host_task.c`とは独立に、`rp2040_host_check.ino`と同じ発想の最小コード(`tuh_hid_mount_cb`でReport Descriptorをダンプ、`tuh_hid_report_received_cb`で生レポートをそのままログ出力するだけ)で、ESP32上でもMaxxterドングルから**7バイート届くか**を確認する(ESP32上でTinyUSB Host + MAX3421が動くこと自体の一次検証。SPIプローブによる自動検出はまだ後回しでよい)。
4. **確認できたら本実装へ**: SPIプローブによるMAX3421検出→バックエンド選択のロジックを`usb_host_task.c`(またはその周辺)に実装し、`hid_report_parser.c`/`filter_rules.h`/`protocol.h`送信ロジックを両バックエンド共通で繋ぎ込む。9ボタンマウスquirkコードが本当に不要になったかもここで確認。
5. **Hub経由の複数デバイス**確認(`mds/2026-08-22_multi_device.md`の要件)。MAX3421フォールバック時・native OTG時それぞれで。

## Phase1 実装結果(ビルド確認済み、実機未検証)

上記の提案通りソフト側を実装し、`bin/build_host.sh build`(Host role)・`bin/build_device.sh build`(Device role、退行確認)ともに警告0件でビルドが通るところまで確認した。実装過程で当初案から2点訂正が要った。

### 訂正1: 本家TinyUSBのsubmodule追加ではなく、既存vendorコピーをそのままローカルcomponent化

「本家`hathach/tinyusb`をgit submoduleで追加」ではなく、**既に`managed_components/espressif__tinyusb`にvendorされている(Espressifが取り込み済みの)全く同じソースツリーをそのまま`esp32-kvm-ip/components/tinyusb/`にコピーし、`main/idf_component.yml`の`override_path`でそちらを使うよう指定**する方式にした。理由: 新規にsubmoduleを足すと「Device roleが使うEspressifパッケージ」と「Host roleが使う本家素コピー」の2系統のtinyusbバージョンが並存し将来ズレるリスクがあるが、今回の方式なら**プロジェクト全体で常に単一のtinyusbソースツリー**(Host roleはこのローカルcomponentを、Device roleは元々のEspressifパッケージ相当を、同じソースから使い分けるだけ)で済む。`override_path`はESP Component Managerの正規機能で、`idf.py reconfigure`時に`managed_components/espressif__tinyusb`を自動的に使わなくなることも確認済み(`dependencies.lock`の該当エントリが`source: {type: local, path: components/tinyusb}`に変化)。

### 訂正2: `main/tusb_config.h`は実は完全に無効(Device roleも含めて)だった

「`tusb_config.h`はDevice role専用の単一ファイルなので`CFG_TUSB_CONFIG_FILE`でHost role用に差し替える」という当初案は、調査の結果**前提から誤りだった**。実際には`espressif/esp_tinyusb`のCMakeLists.txtが

```cmake
idf_component_get_property(tusb_lib ${tinyusb_name} COMPONENT_LIB)
target_include_directories(${tusb_lib} PRIVATE "include")
```

という形で、tinyusbライブラリターゲットに**自分自身の(esp_tinyusbにバンドルされた)`include/tusb_config.h`を直接インクルードパスとして注入**しており、これがDevice roleの実際の設定として使われている。`main/tusb_config.h`はどのビルドコマンドの`-I`にも一度も出てこないことを`compile_commands.json`で確認した — つまり存在するだけで**Device roleも含めて一度も実際に読まれていなかった**(元々の`CFG_TUD_HID=3`等の値は生きているように見えて実は無意味で、本当のDevice用設定はesp_tinyusb側のKconfig駆動の設定ファイルの方)。

このため、Host role用のTinyUSB設定は`main/tusb_config.h`ではなく**`components/tinyusb/host_config/tusb_config.h`(新設、component内)に置き**、`components/tinyusb/CMakeLists.txt`側で

```cmake
target_include_directories(${COMPONENT_LIB} BEFORE PRIVATE "host_config")
```

として`BEFORE`で強制的に先頭に挿入することで、esp_tinyusbの注入(処理タイミングに関係なく)より確実に勝たせている。あわせて、Host role分の`srcs`は「追加」ではなく「Device用ファイル一式を丸ごと差し替え」(排他)にした — 同じ`tusb_config.h`をDevice用ファイル(`dcd_dwc2.c`/`usbd.c`等)にも使わせるとCFG_TUD_*が欠落して壊れるため、Host roleではそもそもDevice用ファイルをコンパイル対象に含めない方が単純かつ安全と判断。

### 実装した追加API

`hcd_max3421.c`が要求する3つのボードAPI(`tuh_max3421_spi_cs_api`/`spi_xfer_api`/`int_api`)に加えて、TinyUSBコア(`tusb_common.h`)が要求するミリ秒タイマーAPI(`tusb_time_millis_api`/`tusb_time_delay_ms_api`、FreeRTOSの`xTaskGetTickCount()`ベースで実装)も必要だった(リンクエラーで発覚)。

### 新規ファイル
- `esp32-kvm-ip/components/tinyusb/`: 上記のローカルoverride component(`managed_components/espressif__tinyusb`のコピー + Host role用`srcs`分岐 + `host_config/tusb_config.h`)
- `esp32-kvm-ip/main/usb_host_max3421.c`/`.h`: SPI+GPIOグルー実装 + Phase1スモークテスト(`tuh_hid_mount_cb`/`report_received_cb`で記述子・生レポートをそのままログ出力するだけ、`rp2040_host_check.ino`と同じ発想)。`main_host.c`から既存のnative OTGパス(`usb_host_task_start()`)と**並行して無条件に**呼ぶようにした(失敗しても非致命的 - ログを出して続行するだけ)。ピン配置(`MAX3421_PIN_*`)はプレースホルダなので実配線に合わせて要調整。

### ピン配置(`main/usb_host_max3421.c`) - 実配線に合わせて確定
ESP32S3-Plus標準SPIピン(MOSI=GPIO9 / MISO=GPIO8 / SCLK=GPIO7)+ CS=GPIO4 / RST=GPIO5 / INT=GPIO6。デバッグ出力は引き続きUART0(GPIO43/44, D6/D7)で衝突なし。
- **INT**(GPIO6): MAX3421EのINTはオープンドレイン・Lowアクティブ - 直結でOK(コード側で内部プルアップ+立ち下がりエッジ割り込みを設定済み)。
- **RST**(GPIO5): RESETもLowアクティブ。コード側で起動時に「Low 10ms → High 10ms待ち」のリセットパルスを打つように実装済み(発振器安定待ち)。フローティングにしないこと。

### 実機確認: Report Protocolの取り忘れ、SPIクロック起因の不安定さ

実配線後、最初は`tuh_hid_report_received_cb`が3バイート(`00 00 01`)を返してきて一瞬「ESP32ネイティブ側と同じバグがMAX3421でも再現したか」と焦ったが、原因は単純: TinyUSBはデフォルトでBoot Protocol(`hid_host.c`の`_hidh_default_protocol = HID_PROTOCOL_BOOT`)なので、RP2040クロステストの時と同様`tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT)`の呼び出しが要る。これを`max3421_host_task()`(`tuh_rhport_init()`より前)に追加したら7バイート(Report ID込み)が届くようになった — ESP32ネイティブ側の謎バグの再来ではなく、単なる実装漏れだった。

その後「MAX3421 unmountがすぐ起きて不安定」という報告あり。VBUS(5V)・MAX3421 VCC(3.3V)は共に配線済みと確認済みなので、電源不足ではなく**SPIクロック(当初10MHz)がブレッドボード配線には速すぎた**線が濃厚と判断し、`MAX3421_SPI_CLOCK_HZ`を1MHzまで下げた。HIDは元々低帯域なので速度を犠牲にする価値は十分にある。この不安定さの根本解決は保留にして先に進む方針(最悪RP2040をUSB Hostとして使う代替案あり)。

### 訂正: 出力先は「UDP → 既存Device role基板」のみ実装、type-c直結は未着手
要件の「HID機器 → MAX3421 → filter/conv/route → ESP32 type-c (device) or UDP」のうち、ここまで実装したのは**UDP側のみ**。「MAX3421を積んだこのESP32自身のtype-cをDevice化してPCに直結する」方は**まだ何もしていない**(以前の記述で実装済みであるかのように読める部分があったが誤り、訂正)。

この2つは実装として結構違う: type-c直結には、MAX3421がHostを担う分空いているネイティブUSB-OTGペリフェラルをDeviceモードで使う必要があり、`components/tinyusb`をDevice+Host両対応(dual rhport)でビルドし直す必要がある(現状のKVM_ROLE=HOST分岐はHost専用ファイルしか含めていない)。

**優先順位を確認: 両方将来的に欲しいが、まずは動作確認のためUDP → 既存Device role基板の疎通を優先。type-c直結は後回し。**

## Phase1 完了: `hid_forwarder.c`への集約 + `usb_host_max3421.c`の本実装

Phase1の最後のステップ(`hid_report_parser.c`/`filter_rules.h`/`protocol.h`への接続)を実施。

### リファクタ: UDP送信+filter/mergeロジックを`hid_forwarder.c`に切り出し
これまで`usb_host_task.c`(native OTGバックエンド)に直書きだった、UDPソケット管理・`filter_rules.h`適用・キーボードのマージ状態(物理キーボード+マウスボタンからの合成キーの合成、`mds/2026-08-21_filter_conv_route.md`)を、新設の`main/hid_forwarder.c`/`.h`に切り出した。両バックエンドが呼ぶ公開APIは3つだけ:
- `hid_forwarder_keyboard_report(modifiers, keycodes[6])`
- `hid_forwarder_mouse_sample(buttons, dx, dy, wheel, pan)`
- `hid_forwarder_consumer(usage_id)`

`main_host.c`が起動時に`hid_forwarder_init()`を1回呼び(UDPソケット作成)、その後`usb_host_task_start()`(native OTG)と`usb_host_max3421_task_start()`(MAX3421)を両方起動する形。`usb_host_task.c`側もこの`hid_forwarder_*`呼び出しに置き換え、重複コードを解消(退行なし、ビルド確認済み)。

### `usb_host_max3421.c`: デバイス種別判定・記述子パース・ディスパッチを本実装
`tuh_hid_mount_cb`/`tuh_hid_report_received_cb`で、`usb_host_task.c`の`handle_driver_connected`/`hid_host_interface_callback`とほぼ同じロジック(マウス/キーボード/Consumer Controlの判定、`hid_report_parser.c`でのReport Descriptorパース、Report/Boot Protocolの切り替え)を`tuh_hid_*` API向けに実装。デバイス状態は`hid_host_device_handle_t`の代わりに`(dev_addr, idx)`をキーに管理。

**9ボタンマウスの3バイート切り詰めquirk対応コード(`last_boot_consumer_usage`)は移植していない** — TinyUSB経由では実機で7バイート丸ごと正しく届くことを確認済みなので不要と判断(ただし実際の9ボタンマウス実機での再確認はまだ)。

### ハマった沼: 手動SET_PROTOCOLの二重リクエストが3バイート化の真因だった

実機テストで断続的に3バイートレポートが再発。何段階か仮説を試したが遠回りだった(結果的に無駄だった経緯として残す):
1. 「TinyUSBのデフォルトプロトコル取り忘れ」仮説 → 実は取得済みで無関係
2. 「`tuh_hid_set_protocol()`が非同期なのでreceive_report開始が早すぎる」仮説 → completion callback(`tuh_hid_set_protocol_complete_cb`)で同期化したが直らず
3. **最終的にユーザーの指摘で気づいた根本原因**: dump-onlyの動作実績があるバージョン(コミット`a6a5301`)を基準に「そこから何が変わったか」で差分を見直すべきだった。差分は「`tuh_hid_mount_cb`内でマウス用に手動`tuh_hid_set_protocol(Report)`を追加で呼んでいたこと」——列挙時の自動SET_PROTOCOL(`tuh_hid_set_default_protocol()`+`CFG_TUH_HID_SET_PROTOCOL_ON_ENUM`、デフォルト有効)と合わせて**同じ内容のリクエストを2回**送っていたことになる。このドングルはSET_PROTOCOL周りが元々怪しい(`mds/2026-08-22_wireless_dongle_short_reports.md`)ので、重複リクエストで一時的にBoot形状のレポートを吐いていた可能性が高い。

対処: 手動の`tuh_hid_set_protocol()`呼び出しを完全に削除し、dump-onlyバージョンが実際にやっていた「自動の1回だけ」に戻した。dispatch/parsingロジックはそのまま追加。実機で7バイート確認できたので、これで確定とする。

**教訓**: 動作実績のあるコードがある時は、そこからの差分(diff)で原因を絞るべきで、仮説を積み重ねて後付けで修正していくのは遠回りになりやすい。

### 現状: 依然不安定(保留中)、Device roleに届いても操作できないことがある

7バイートのレポート自体は安定して来るようになったが、**MAX3421側の通信自体が時々乱れる**(unmount、`[1:1] raw report (0 bytes):`という長さ0のレポートが混ざる、等)。この状態のとき、Device role側にUDPが届いてもPC操作ができないことがある一方、Device role基板をリセットすると復活する、との報告あり。

0バイートのレポート自体は`handle_keyboard_report`/`handle_mouse_report_boot`が長さチェックで弾く(`length < sizeof(...)`で早期return)ので、それ単体がDevice側を壊す直接原因ではないはず。**より疑わしい仮説**: 同じ通信の乱れの最中に、長さは正常だが中身が化けたレポート(garbage but non-zero length)が紛れ込み、たまたま「何らかのキーが押された」ように解釈されて`hid_forwarder_keyboard_report()`経由で送信され、UDPプロトコルは差分ではなく毎回フルステート送信なので、その後に正しい(キー解放)レポートが届かない限りDevice側はそのキーを押しっぱなしと認識し続ける——Device roleのリセットで直る、という報告と整合する。

根本原因はSPI/配線の信号品質(mount/unmount不安定さと同根)である可能性が高く、これ自体は引き続き保留。git履歴は`a6a5301`から今回のコミットまでを1つに整理(squash)した。

### 未実装・保留(次ステップ)
- **MAX3421の通信不安定さの根本解決(保留中)** - 電源・SPIクロック(現状5MHz)は試したが未解決。最悪RP2040をUSB Hostとして使う代替案あり
- type-c直結Device出力(将来対応、優先度は下げた)
- SPIプローブによるMAX3421自動検出→バックエンド選択(現状は無条件で両方起動)
- 9ボタンマウス実機での再確認(quirk無しで本当に問題ないか)
- Hub経由の複数デバイス確認(`mds/2026-08-22_multi_device.md`の要件)

## Phase2 route/filterの詳細を詰める
