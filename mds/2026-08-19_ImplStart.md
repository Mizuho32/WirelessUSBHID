## 概要
- よく考えるとマルチ対応は無くても困らないな(もともとリモートマシンのBIOS操作用を考えてたので)
- まず、BIOS対応だけ実装したい。submoduleに追加したので、適当なブランチ切って、実装、ビルドまで回してくれない? 
- XIAO ESP32S3を考えてて、なんか標準type-CをUSB OTGにできそうな雰囲気ある。
  - もしソフト側でそれに関する対応必要ならそれも実装して。
  - デバッグ出力は、2つめのUART(あるよね?)を使って、USB Serialで見ればいい。
- IDFは/opt/esp-idf5.5.1が使える(バージョン足りてるよね?)

## メモ

### Installs
cmake, ninja

### 実施内容

**ブランチ**: `esp32-kvm-ip`サブモジュール内に`feature/bios-boot-protocol`を作成し、`main` (e7f8a95)から分岐。実装をコミット済み(4dbc9e7)。

**実装内容**:
- 単一の Report ID 多重化 HID インターフェースを廃止し、Boot Keyboard(itf 0)と Boot Mouse(itf 1)の2つの専用インターフェースに分割。`bInterfaceSubClass`は`TUD_HID_DESCRIPTOR`マクロが自動でBoot(=1)に設定。
- レポート構造体はTinyUSB本体が「Standard HID Boot Protocol Report」として定義済みの`hid_keyboard_report_t`/`hid_mouse_report_t`をそのまま使用(自前定義を削除)。Report IDを使わないため、Boot/Reportどちらのプロトコルでも同じバイト列で通る設計にでき、プロトコル切替時の特別なロジックは不要。
- マウスはBoot Protocol制約で8bit相対X/Yになるため、UDPから来る16bit dx/dyをclamp。
- Consumer Control(メディアキー)はスコープ外として削除(UDPプロトコル自体は未変更なので、Pythonサーバ側が送ってきても単に無視される)。
- `CFG_TUD_HID`を1→2に変更。
- **XIAO ESP32S3対応**: USB-Cポートが1つしかなくネイティブUSB(GPIO19/20)をBoot HIDデバイスが専有するため、コンソール出力をUSB-Serial-JTAGからUART0(XIAOのD6/D7 = GPIO43/44)に変更。フラッシュ書き込みはROMブートローダー側の機能なので影響なし。実機ログを見るには外付けUSB-UARTをD6/D7に接続する必要がある点は留意。

**ビルド環境構築**: `/opt/esp-idf5.5.1`はPython venv・cmake・ninjaが未インストールだったため、`~/.espressif`を書き込み可能にしてもらった上でインストール・`idf.py set-target esp32s3`・`idf.py build`まで実行し、警告・エラーなしでビルド成功を確認済み(`esp32-kvm-ip.bin`生成、使用率24%程度)。

### 保留にしたこと

- 親リポジトリ(`Wireless_USBHID`)側のコミットはまだしていない。READMEやサブモジュール追加が未コミットのまま。サブモジュールのポインタ更新も含めてコミットするかは要確認。
- 実機(XIAO ESP32S3)への書き込み・実BIOSでの動作確認は未実施(このセッションではビルド確認まで)。
- ~~WiFi SSID/パスワードは`menuconfig`で設定が必要~~ → 後述の理由でKconfigから`main/wifi_credentials.h`に変更済み。

### 追加実装: Boot Protocol + 既存機能(Report Protocol)の両立

「Report ID多重化を廃止した=既存機能を失う」わけではないか?という指摘を受けて調査・実装。

**結論**: 両立できる。Report Descriptorは**Report Protocolモード時にしか参照されない**(Bootモード中、BIOSはディスクリプタを一切読まず固定フォーマットを決め打ちで期待する)。OSが起動すると自動的にReport Protocolへ切り替わる(`SET_PROTOCOL`、デフォルトもReport)ため、同じインターフェースが`tud_hid_n_get_protocol(instance)`を見て「Bootモードのときだけ縮小フォーマット、Reportモード(=OS起動後)は元の拡張フォーマット」を出し分けることが仕様上正当にできる。

**実装内容**(コミット 85285b3):
- キーボード: 元々Boot互換の8バイト固定フォーマットだったため分岐不要、変更なし。
- マウス: `tud_hid_n_get_protocol(ITF_NUM_MOUSE)`で判定し、Bootモード時はTinyUSB標準の5バイト圧縮フォーマット(8bit相対XY)、Reportモード(通常のOS操作時)は元の7バイト拡張フォーマット(5ボタン, 16bit相対XY, wheel, pan)を送信。→ OS起動後は元の精度(16bit)がフルに復活。
- Consumer Control(メディアキー): BIOSは無関係なので、Boot非対応の3つ目のインターフェース(itf 2)として復活。`CFG_TUD_HID`を2→3に変更。
- エンドポイント: Keyboard(0x81) + Mouse(0x82) + Consumer(0x83) の3本の専用IN割り込みエンドポイント + 全インターフェース共有のEP0(コントロール、`SET_PROTOCOL`/`SET_IDLE`/`SET_REPORT`等がここを通る)。ESP32-S3の上限(計6本、同時アクティブIN最大5)に収まる。
- ビルドは警告・エラーなしで成功。

### 追加実装: WiFi認証情報をKconfigから外す

「menuconfigでWiFi設定するとフルビルド要る?」という指摘を受けて調査・実装(コミット bb23ee7)。

**実測結果**: sdkconfigの値を1つ書き換えただけで、1069ステップ中953ファイルが再ビルド対象になった(ほぼフルビルド相当)。ESP-IDFは`sdkconfig.h`をほぼ全ファイルから間接的に参照する構造のため。

**対応**:
- `main/Kconfig.projbuild`から「WiFi Configuration」メニュー(WIFI_SSID/WIFI_PASSWORD)を削除。
- 代わりに`main/wifi_credentials.h`(gitignore対象)に`WIFI_SSID`/`WIFI_PASSWORD`をプレーンな`#define`として定義。テンプレートとして`main/wifi_credentials.h.example`をリポジトリに追加。
- `main.c`は`CONFIG_WIFI_SSID`/`CONFIG_WIFI_PASSWORD`ではなく`wifi_credentials.h`の`WIFI_SSID`/`WIFI_PASSWORD`を参照するよう変更。
- README(esp32-kvm-ip側)のセットアップ手順も更新。
- **実測で確認**: `wifi_credentials.h`のパスワードだけ書き換えて再ビルドしたところ、再コンパイルされたのは`main.c`1ファイルのみ。狙い通り。
