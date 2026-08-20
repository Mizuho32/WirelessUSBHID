#  初期実装

## 概要
やりたいこと：USBセレクタを無線化したい。
- 対象は HID（マウス・キーボード）限定。複数マウス等、複数HIDデバイスがありうる。
- PC側には専用ドライバを入れたくない。特にBIOSからも普通のUSBキーボード/マウスとして認識させたい。
- そのため、USB/IPのような「PC側ドライバ必須」方式ではなく、PC側MCUがUSB Deviceとして物理的にHIDをエミュレーションする方式を考えている。

構成イメージ：
```
[Mouse / Keyboard]
        │ USB
        ▼
[MCU: USB Host + Wi-Fi]
        │
       Wi-Fi
        │
        ▼
[MCU: USB Device + Wi-Fi]
        │ USB
        ▼
       PC
```

- 機器側MCUは USB Host として実物のHIDを読み取る。
- PC側MCUは USB Device としてPCにHIDを提供する。
- PCからは専用ドライバなしで、普通のUSB HIDに見せる。
- 複数デバイスの場合、PC側でUSB Hub + 複数HID Deviceをエミュレーションする構成が自然。
- BluetoothよりWi-Fi推奨。USBプロトコルをトンネルする用途ではレイテンシ/帯域的にWi-Fiの方が扱いやすい。
- MCU候補として ESP32-S3を両端に1個ずつが有力。USB OTG（Host/Device）＋Wi-Fiを1チップで持つため構成が簡単。
- 重要な検討事項：BIOS対応のためUSB HID Boot ProtocolをPC側ESP32-S3で実装できるか、USB Hub + 複数HIDのエミュレーションをどう実装するか。`

## 要件
- USB HID機器を完全に透過で無線化(マウスとキーボードを解釈して、MCUのHID機能で、ではなく)

## 疑問
- 複数HIDのエミュレーションって書いてあるけどできるのか?
- HID Boot Protocolって書いてあるけど、本当に必要なの? (HIDとして認識できればOSがどうとか関係ないと思ってたけど違う…?)

## 調査結果

### Q1. 複数HIDのエミュレーションはできるのか?

**結論: できる。ただし「USB Hub」としてではなく「複数のHIDインターフェースを持つコンポジットデバイス」として実装するのが正しいアプローチ。**

- USB的に「複数のHIDデバイスに見せる」ことと「USB Hubを実装する」ことは別物。Windows/BIOSから複数のHIDデバイスとして認識させたいだけなら、1つのUSBデバイスの中に複数のHIDインターフェース(キーボード用インターフェース、マウス用インターフェースなど)を持たせる「コンポジットデバイス」構成で十分であり、これが標準的な実装方法。
- 実際、TinyUSB(ESP-IDFのUSBデバイススタックの実体)には**デバイス側のUSB Hubクラス実装は存在しない**(Hubクラスはホスト側機能としてのみサポートされている)。よって概要にある「USB Hub + 複数HID Deviceをエミュレーション」という表現は誤りで、正しくは「コンポジットHIDデバイス(Hubは介在しない)」。
- TinyUSBは`CFG_TUD_HID`を1より大きくすることで複数HIDインスタンス(=複数インターフェース)を持たせられる。実装は`usb_descriptors.c`側でインターフェース番号・エンドポイント番号を個別に定義し、`GET_HID_DESCRIPTOR`等のクラスリクエストをインターフェース番号で分岐させる形になる。
- **ハードウェア制約(エンドポイント予算)に注意が必要**: ESP32-S3のUSB-OTG(DWC2 Full-Speedコア)はデバイスモードでエンドポイントが計6本(EP0含め最大5 INが同時アクティブ)しかない。つまり無制限に複数HIDインターフェースを増やせるわけではなく、キーボード+マウス+α程度が現実的な上限。
  - キーボードのLED状態通知(Caps Lock等)は専用のINTERRUPT OUTエンドポイントを使わずControlエンドポイント(EP0)経由のSET_REPORTで代替でき、エンドポイントを節約できる。
  - さらにエンドポイントを節約する手段として、1つのHIDインターフェース内に複数のTop Level Collection(Report IDで区別)を持たせ、複数の論理HIDデバイスを1エンドポイントに相乗りさせるテクニックもある。ただし後述のBoot Protocolは「インターフェース単位」でしか機能しないため、BIOS対応が必要なキーボード/マウスは専用インターフェースを割り当てる必要がある。
- **推奨構成**: Boot Protocol対応の専用キーボード・インターフェース1つ + 専用マウス・インターフェース1つ(BIOS用)、それ以外の追加デバイスはReport Protocolのみのインターフェース(またはTLC相乗り)として実装。初期スコープは「キーボード1台+マウス1台」に絞るのが現実的。

### Q2. HID Boot Protocolは本当に必要なのか?

**結論: 必要。BIOSからの認識を要件にしている以上、事実上必須。**

- BIOS/UEFIやブートローダー(GRUB, iPXEなど)が内蔵するUSBスタックは簡易実装であり、フルのHID Report Descriptorをパースせず、固定フォーマット(キーボード8バイト、マウス3〜4バイト)の**Boot Protocol**のみに対応しているケースが大半。README/概要に明記されている「BIOSからも普通のUSBキーボード/マウスとして認識させたい」を満たすには、Boot Protocol対応(インターフェースディスクリプタで`bInterfaceSubClass=1`, `bInterfaceProtocol=1`(キーボード)/`2`(マウス))が事実上必須で、これがないとOS起動前(BIOSセットアップ画面やブートローダーでの選択画面等)で操作できない。
- 実現性は高い: TinyUSBにはBoot Keyboard/Mouse実装のサンプル(PR #1025)が存在し、実機検証でLenovo/HP機のBIOS、GRUB、iPXEで正常動作したと報告されている。ESP32-S3 + TinyUSBの組み合わせでの実現性は高いと判断できる。
- 実装上の留意点:
  - Boot/Reportプロトコルの切替はホストからの`SET_PROTOCOL`リクエストで行われ、TinyUSB側は`tud_hid_set_protocol_cb()`で捕捉する。過去バージョンでこのコールバックが呼ばれないバグが報告されている(Issue #1129)ため、実装時は最新のTinyUSB/ESP-IDFで動作検証すること。
  - Boot Protocolのキーボードは同時押しキー数が最大6キー+モディファイアという制約がある(Report Protocolならより柔軟に拡張可能)。
  - Boot対応が必須なのは基本的に「キーボード1つ・マウス1つ」まで。それ以外の追加デバイスはBIOS上で認識されなくても実用上問題ない(OS起動後はドライバがReport Protocolで通信するため、OS側では全デバイスを通常通り認識できる)。

### 機器側(USB Host)での複数デバイス対応について

- README冒頭に「複数、HUB経由」とある通り、機器側MCUが物理HUB経由で複数のUSB HID機器(複数マウス等)を読み取る前提だが、ここにも同様にエンドポイント予算の制約がある。
- ESP32-S3のUSB Host機能(DWC2ベース)はチャンネル数が8(EP0含む)。最近のESP-IDFではKconfigオプション `CONFIG_USB_HOST_HUBS_SUPPORTED`(1段HUB)/`CONFIG_USB_HOST_HUB_MULTI_LEVEL`(多段HUB)で外付けHUB経由の複数デバイス接続をサポートしている。ただしダウンストリームデバイス1台ごとにチャンネルを消費するため、8チャンネルという上限の中でキーボード+マウス程度なら問題ないが、大量のデバイスを同時接続するのは非現実的。
- 古いESP-IDF/TinyUSBの時期は外付けHUBサポート自体が無かったため、実装時は最新のESP-IDF(HUB対応が入ったバージョン以降)を使う必要がある。

### 類似の先行プロジェクト(参考)

- [KMChris/esp32-kvm-ip](https://github.com/KMChris/esp32-kvm-ip): ESP32-S3を使い、Windowsホストの入力をWiFi/UDP経由で別PCにUSB HID(キーボード・マウス)としてエミュレーションするKVM実装。以下の設計が参考になる:
  - 固定16バイトのUDPパケット、マジックナンバー+シーケンスカウンタで重複/古いパケットを破棄。
  - 差分ではなく「フルステート」を毎回送ることで、パケットロス時の影響を最小化。
  - WiFi Modem Sleepを無効化し、初回パケットで発生しがちな約200msの遅延を排除。
  - ポーリングレートは125Hz(60〜1000Hzで可変)、Core0=ネットワーク処理/Core1=HID処理に分離。
  - ただし**Boot Protocol非対応・単一デバイスのみ対応**であり、本プロジェクトが必要とする「BIOS対応」「複数デバイス対応」は満たしていない。レイテンシ対策・パケット設計は流用できるが、そのまま使える実装ではない。

### 設計方針への反映

- 概要の「USB Hub + 複数HID Deviceをエミュレーションする構成が自然」は「コンポジットHIDデバイス(Hubは使わない)」に修正する。
- Boot Protocol対応を要件に明記する(BIOS対応の実現手段として必須)。
- デバイス側6エンドポイント/ホスト側8チャンネルというハードウェア制約を踏まえ、初期スコープは「キーボード1台+マウス1台、両方Boot Protocol対応」に絞り、複数マウス等の追加デバイス対応は拡張フェーズとする。
- Wi-Fi区間の実装(UDP、フルステート送信、Modem Sleep無効化等)はesp32-kvm-ipの設計を参考にできる。

### 参考リンク

- [USB Device Stack - ESP32-S3 (ESP-IDF Programming Guide)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/usb_device.html)
- [USB Host - ESP32-S3 (ESP-IDF Programming Guide)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/usb_host.html)
- [USB Device Driver / Host Driver - ESP-USB Programming Guide](https://docs.espressif.com/projects/esp-usb/en/latest/esp32s3/usb_device.html)
- [HID Device (Keyboard) Boot Protocol for use in PC BIOS/GRUB etc. - tinyusb Discussion #1003](https://github.com/hathach/tinyusb/discussions/1003)
- [tud_hid_set_protocol_cb() is not called when SET_PROTOCOL is sent - tinyusb Issue #1129](https://github.com/hathach/tinyusb/issues/1129)
- [Esp32 S3 USB Hub - tinyusb Discussion #2467](https://github.com/hathach/tinyusb/discussions/2467)
- [Support for 1-level USB hub in USB host implementation - esp-idf Issue #12554](https://github.com/espressif/esp-idf/issues/12554)
- [KMChris/esp32-kvm-ip](https://github.com/KMChris/esp32-kvm-ip)

## 実装方針(初期決定事項)

### スコープ

- 単一デバイス版を経由せず、**マルチHID対応 + BIOS対応(Boot Protocol)を最初のマイルストーンとする**。

### KMChris/esp32-kvm-ipの詳細確認結果

PC側(デバイス側)の現状実装を確認したところ:

- `CFG_TUD_HID = 1`で**HIDインターフェースは1つのみ**。
- そのインターフェース内でマウス(Report ID 1)・キーボード(Report ID 2)・コンシューマーコントロール(Report ID 3)を**Report IDで多重化**し、単一エンドポイント(`0x81`, 16バイトバッファ)に集約している。
- `bInterfaceProtocol = HID_ITF_PROTOCOL_NONE`であり、**Boot Protocol非対応**(想定通り)。
- 送信側(Host側)はESP32ではなく**Windows PC上のPythonソフト**(`WH_KEYBOARD_LL`/`WH_MOUSE_LL`/Raw Inputフックで入力をキャプチャ)。実物のマウス/キーボードをMCUがUSB Hostとして直接読む構成ではない。

この設計(Report ID多重化)はエンドポイントを消費せずデバイス種別を増やせるため、**Boot Protocol非対応でよい追加デバイスの受け皿としてはそのまま拡張できる**。一方、Boot ProtocolはReport IDなしの固定フォーマットが前提のため、**既存インターフェースの延長では実現できず、専用のBoot Keyboard/Bootマウス・インターフェースを別途新設する必要がある**(`CFG_TUD_HID`を1→3程度に増やし、TinyUSBのboot_interfaceパターンを追加)。エンドポイント予算は Boot Keyboard + Boot Mouse + 既存の多重化Reportインターフェースで計3 IN + EP0 = 4本となり、デバイス側の上限6本に収まる。

### KMChrisの取り込み方法: フォーク(コード取り込み) vs サブモジュール

**結論: サブモジュールではなく、フォークしてコードをモノレポに取り込み直接改造する。**

技術的コストの観点での判断理由:

- `usb_descriptors.c` / `tusb_config.h` / `hid_task.c` はTinyUSB/ESP-IDFの流儀上、アプリ側にコピーして直接編集する前提のファイル群であり、安定APIの向こうに隠して依存する設計になっていない。サブモジュール化する自然な境界が存在しない。
- 最初から行う変更(HIDインターフェース数の変更、Bootインターフェース新設、レポート整形ロジックの二系統化)は、まさにサブモジュールで「触らずに済ませたい」中核ファイルそのものへの変更。サブモジュールにしても本家の更新を素直に取り込める見込みはほぼなく、二重リポジトリ管理のコスト(pin管理、`git submodule update --init`忘れ、CI設定の複雑化)だけが残る。
- 機器側MCU(USB Host)はKMChris側に対応コードが存在せず完全新規実装になるため、どのみちプロジェクト全体を単一リポジトリで一体管理する方が開発効率が良い。
- ライセンスはMITのため、取り込みに手続き的な障害はない。手順としては、traceability目的でGitHub上に一度forkしてから、コードをモノレポに取り込んで自プロジェクトのファイルとして改造する。

### 各コンポーネントの実装方針

- **PC側MCU(USB Device)**: KMChris/esp32-kvm-ipのDevice側コード(`main/`一式)をフォークして取り込み、直接改造のベースとする。
  - 既存の多重化Reportインターフェースは維持し、BIOS非対応でよい追加デバイスの拡張先として使う。
  - 新規にBoot Keyboard / Boot Mouse用インターフェースを追加し、BIOS対応を実現する。
  - キーボード/マウスの実データは共通の内部状態として持たせ、Boot用インターフェースとReport用インターフェースの両方へ整形して配信する。
- **機器側MCU(USB Host)**: KMChris側に相当コードが無いため新規実装。ESP-IDF公式の`usb/host/hid`サンプル(Boot Protocolのみパースする実装で今回の要件と親和性が高い)をベースにする。
- **デバッグ用PC側ソフト**: KMChris側のWindows Pythonソフトをベースに継続利用する(既に必要性を見込んでいたため好都合)。
- **Host-Device間の通信**: KMChris側はPC⇔MCU間がWiFi/UDPだが、今回はMCU⇔MCU間の通信として新規に設計・実装する必要がある。
