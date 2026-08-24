# RP2040をMAX3421代替のUSB Hostブリッジにする計画

## 背景・動機

MAX3421E経由のHost実装(`mds/usb_hid/2026-08-23_filter_conv_router_with_max3421.md`)は、実機で以下の問題を抱えたまま保留中だった:
- SPI通信自体が時々乱れる(mount/unmount不安定、0バイートレポート混入)
- type-c直結Device出力を追加した際、`Unhandled interrupt 4 on cpu 0!`のクラッシュループが発生
- 上記の切り分けのため電源を別供給しようとした結果、**MAX3421E自体が壊れた可能性が高い(初期化(REVISIONレジスタ確認)が通らなくなった)**

一方、`rp2040_host_check/`(`mds/usb_hid/2026-08-22_rp2040_host_check.md`)では、**全く同じワイヤレスドングルをRP2040 + TinyUSB Hostスタックに挿すだけで、何の工夫もなく7バイート丸ごと正しく届く**ことを既に実機確認済み。ESP32-S3のネイティブUSBホスト固有の問題であって、ドングル側にもTinyUSBというソフトウェアスタック自体にも問題がないことは確定している。

MAX3421Eが物理的に故障した(かもしれない)今、**MAX3421Eチップの代わりにRP2040自体をUSB Hostコントローラとして使い、ESP32とはUART等の単純なリンクで繋ぐ**方針を検討する。

## MAX3421方式との本質的な違い

MAX3421Eは「USBプロトコル処理は自分でやるが、記述子パース・HIDレポート解釈は一切しない、レジスタの塊」で、ESP32側がTinyUSB Hostスタックそのものを動かし、SPI経由でMAX3421のレジスタを直接叩く(`hcd_max3421.c`)必要があった。このSPI通信は「複数バイトのトランザクションをCSアサート中に連続で行う」低レベルな作りで、配線品質・クロック速度にシビアだった(今回のmount/unmount不安定さの根本原因も恐らくここ)。

RP2040案は逆転の発想: **RP2040自身が(rp2040_host_check.inoで既に実証済みの)TinyUSB Hostスタックをまるごと動かし、HIDレポートの記述子パース・デコードまで全部RP2040側で完結させる**。ESP32側に必要なのは、RP2040から来る「もう解釈済みのHIDイベント」を受け取るだけの単純なメッセージ受信ロジックであり、**TinyUSB Hostスタックそのものも、SPIレジスタレベルの通信も一切不要になる**。

UART(単純な調歩同期、バイト単位で自己クロック)はSPI(複数バイトを跨ぐ厳密なタイミングでクロック同期する必要がある)と比べて配線品質にずっと寛容なので、MAX3421方式で悩まされた信号品質問題そのものが原理的に起きにくいと期待できる。

## アーキテクチャ案

```
[ワイヤレスドングル] --USB--> [RP2040 (Host, TinyUSB)] --UART--> [ESP32-S3 (Host role)] --WiFi/UDP--> [Device role基板]
```

- RP2040: `rp2040_host_check.ino`を拡張。`tuh_hid_mount_cb`/`tuh_hid_report_received_cb`は既存のまま(実証済み)、ダンプ先をSerial1のテキストではなく、後述のバイナリフレームプロトコルに変更(または追加)。
- ESP32側: 新規`main/usb_host_rp2040_bridge.c`(仮)。UARTで受けたフレームをデコードし、`usb_host_max3421.c`が既に持っている`hid_report_parser.c`ベースのパース処理・`handle_keyboard_report`/`handle_mouse_report_generic`/`handle_consumer_report`・`hid_forwarder_*`への受け渡しロジックをそのまま再利用する(MAX3421固有なのはSPI/GPIO/`tuh_*` APIまわりだけで、パース以降は完全にトランスポート非依存に書けるはず)。

## UART間プロトコル案(叩き台)

固定长フレーム+チェックサムで、UARTの「バイト列に境界がない」問題に対処:

```
[0xAA (sync)] [msg_type: 1B] [dev_addr: 1B] [idx: 1B] [len: 2B LE] [payload: len bytes] [checksum: 1B (XOR)]
```

- `msg_type`: `MOUNT`(payload=Report Descriptor全体)/ `UNMOUNT`(payload無し)/ `REPORT`(payload=生レポートのバイト列)
- ESP32側は`0xAA`を見つけるまで読み飛ばし、`len`が異常(記述子なら数百バイト程度が上限、レポートなら数十バイト以内)ならその場でフレームを破棄して再同期 - 1バイート化けても連続クラッシュしない設計にする(MAX3421の反省を活かす)。
- ボーレート: 記述子ダンプ(最大でも数百バイト、mountの瞬間だけ)+ 通常レポート(数バイト、高々数百Hz)なので115200でも十分だが、余裕を見て460800や921600でも問題ないはず(短距離の有線UART、SPIほどクロック同期がシビアでない)。

## 必要な変更

### RP2040側(`rp2040_host_check/`を拡張)
- `tuh_hid_mount_cb`/`report_received_cb`は変更不要(記述子・生レポートをそのまま右から左に流すだけ)
- 出力を上記バイナリフレームに変更する薄い送信関数を追加するだけ - 新規ロジックはごく少量
- Serial1(UART、GP0=TX/GP1=RX)をそのままESP32とのリンクに転用(既にデバッグ用に配線済み・実証済みの経路)。人間向けテキストログが要る場合は別途USB-シリアル変換器で分岐して見るか、諦めるか(要検討)。

### ESP32側(新規`usb_host_rp2040_bridge.c` + `.h`)
- UART初期化(`driver/uart.h`、MAX3421用に使っていたGPIO4/5/6/7/8/9は不要になるので、その中から2本(TX/RX)を転用できる)
- フレーム受信タスク(UARTから読み、同期・チェックサム確認・メッセージ組み立て)
- `MOUNT`受信時: `hid_report_parser.c`でのパース + デバイス種別判定(`usb_host_max3421.c`の`tuh_hid_mount_cb`相当のロジックをほぼそのまま移植)
- `REPORT`受信時: `handle_keyboard_report`/`handle_mouse_report_generic`/`handle_consumer_report`相当を呼び、`hid_forwarder_*`に渡す
- **TinyUSB Hostスタック(`tuh_*` API)・SPI・MAX3421関連コードは一切不要** - `components/tinyusb`のHost側ビルド(`hcd_max3421.c`等)ごと外せる可能性がある(ただしtype-c Device出力(Phase2)がrhport0を使う設計はそのまま維持できる - むしろrhport1(Host)を丸ごと使わなくなる分、tusb_config.hがシンプルになる)

### 電源・配線
- RP2040側のVBUS問題(基板のVBUSピンがデバイス専用でダイオードブロックされている)は`mds/usb_hid/2026-08-22_rp2040_host_check.md`で既にジャンパー直結で解決済み、そのまま流用可能。
- ESP32-RP2040間はUART(TX/RX/GND)のみで、MAX3421のような高精度クロック配線は不要。

## 移行方針

- 当初はMAX3421バックエンドを丸ごと置き換える形でよい(MAX3421が壊れている以上、まずは「動くものを1つ作る」を優先)。
- 将来的に余裕があれば、`usb_host_max3421_probe()`と同様の自動検出("RP2040から起動時にハンドシェイクバイトが来るか")でnative OTGとの切り替えに組み込むこともできるが、これは必須ではない。

## 未検討・要確認事項
- RP2040の`Serial1`とESP32側UARTの電圧レベル(共に3.3V系のはずだが要確認)
- UARTフレームの再同期ロジックの具体的な閾値(`len`の妥当性チェック範囲など)
- Hub経由の複数デバイス(`dev_addr`/`idx`をまたぐ複数ストリーム)がRP2040側のTinyUSB Hostスタックで問題なく扱えるか(MAX3421側では実機確認済みなので、RP2040側でも恐らく問題ないはずだが未確認)
- ビルド/書き込みは既存の`bin/build_flash_rp2040.sh rp2040_host_check tinyusb_host build/flash`がそのまま使える見込み

## 参考
- `mds/usb_hid/2026-08-22_rp2040_host_check.md`: RP2040でこのドングルが7バイート届くことを実証済みの記録(VBUSジャンパーの詳細等も含む)
- `mds/usb_hid/2026-08-23_filter_conv_router_with_max3421.md`: MAX3421方式の実装記録・既知の不安定さ・type-c Device出力(Phase2)の実装内容
- `rp2040_host_check/rp2040_host_check.ino`: 拡張のベースになる既存スケッチ

## 実装結果(ビルド確認済み、実機未検証)

上記の計画通りソフト側を実装した。

- `rp2040_host_bridge/rp2040_host_bridge.ino`(新規スケッチ): `rp2040_host_check.ino`のmount/report_receivedコールバックはほぼそのまま(`tuh_hid_interface_protocol()`で判定したitf_protocolを添えて)、ダンプ先をテキストではなく上記のバイナリフレームに変更。`loop()`内で500ms間隔のHEARTBEATも送信(プローブ用)。
- `esp32-kvm-ip/main/usb_host_rp2040_bridge.c`/`.h`(新規): UART受信のバイト単位ステートマシン(`feed_byte`)+ `usb_host_max3421.c`とほぼ同じデバイス種別判定・記述子パース・ディスパッチロジック(ただし`tuh_hid_itf_get_info()`の代わりにフレームの`itf_protocol`フィールドを使う)。TinyUSB Host/SPI関連のコードは一切なし。`usb_host_max3421_probe()`と対になる`usb_host_rp2040_bridge_probe()`(HEARTBEATフレームを最大800ms待つ)・`usb_host_rp2040_bridge_task_start()`を実装。
- `main_host.c`: バックエンド選択をRP2040ブリッジ→MAX3421→native OTGフォールバックの3択に変更。RP2040かMAX3421のどちらかが使われる場合のみtype-c Device出力(`usb_device_typec_start()`)を起動。

### ハマった沼: `esp_driver_uart`だけCMakeの`PRIV_REQUIRES`が効かない

`main/CMakeLists.txt`のHost role分`PRIV_REQUIRES`に`esp_driver_uart`を追加しても、`usb_host_rp2040_bridge.c`の`#include "driver/uart.h"`が`fatal error: driver/uart.h: No such file or directory`で失敗し続けた。同じリストに並んでいる`esp_driver_gpio`/`esp_driver_spi`は問題なく解決されるのに、`esp_driver_uart`だけ解決されない。

切り分けた内容:
- `rm -rf build.host`からの完全リビルド・`idf.py reconfigure`でも再現(キャッシュの問題ではない)
- リストの順番を変えても無関係
- `esp_driver_uart`の代わりに古い`driver`コンポーネントを指定しても無関係(そもそも`driver`は今のESP-IDFではI2C/touch/TWAIの残骸で、UARTとは無関係と判明)
- `build.host/project_description.json`の`main`の`priv_reqs`を直接見ると、実際に`esp_driver_spi`すら載っていない(なのに実際のコンパイルコマンドには`-I .../esp_driver_spi/include`が存在する)——つまりこのjsonの`priv_reqs`表示自体がCMakeLists.txtの内容を正しく反映していない(idf.pyの依存関係ヒント機能`tools/idf_py_actions/hint_modules/component_requirements.py`が使っているのと同じデータ)。しかし実際のコンパイルコマンド(`ninja -t commands`で確認)では`esp_driver_spi`は`-I`に載るのに`esp_driver_uart`だけ載らない、という非対称な現象だった。
- `esp_driver_uart`自体のビルド(`libesp_driver_uart.a`)は問題なく成功しており、コンポーネント自体は正常。

原因はESP-IDF 6.0のコンポーネント要件解決まわりの何らかの非対称な挙動(esp_driver_uartのCMakeLists.txtが持つ`CONFIG_VFS_SUPPORT_IO`時の追加`target_link_libraries(idf::vfs)`が怪しいが未確定)と思われるが、深追いを止めて対処療法に切り替えた。

**対処**: `idf_component_register()`の`PRIV_REQUIRES`に頼らず、登録直後に`target_link_libraries(${COMPONENT_LIB} PRIVATE idf::esp_driver_uart)`を明示的に追加することで解決。ビルド確認済み(Host role・Device role とも警告0件)。
