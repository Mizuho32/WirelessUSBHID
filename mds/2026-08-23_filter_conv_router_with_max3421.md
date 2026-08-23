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

## Phase2 route/filterの詳細を詰める
