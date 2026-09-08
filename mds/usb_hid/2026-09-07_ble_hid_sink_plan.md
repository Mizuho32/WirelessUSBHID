# Host role: BLE HID出力プラン

**未実装、設計のみ**。Host role(`KVM_ROLE=HOST`)に、既存のType-C有線HID出力(`sink :name, :typec`)と並行する新しい出力先として、BLE HID(HOGP)を追加するプラン。物理USB Hostで読んだマウス/キーボードを、ターゲットPCへ**BLE経由で直接**送る(Device role・WiFi中継は不要、この経路には無関係)。

## 動機・スコープ

- ESP32-S3はBLEのみ対応(Classic BT/BR-EDR非搭載)。今どきのBLE HIDキーボード/マウスはWindows/macOS/Linuxほぼ全てネイティブ対応なので実用上問題なし。
- 帯域は非懸念(HIDレポートは数バイト〜十数バイト、BLEの接続間隔で十分足りる)。
- 一番の懸念はリソースではなく**WiFi/BLEの無線共存によるレイテンシ**(同じRFを時分割で共有 - `esp_coex`)。過去の`2026-08-24_rp2040_bridge_fps_investigation.md`同様、実装後に実機でHIDレイテンシの再測定が必須。

## リソース実測(esp_hid_deviceサンプル、esp32s3+NimBLE構成)

`libbt.a`(NimBLE本体)+`libbtdm_app.a`(BLEコントローラ)+`libesp_hid.a`(HIDプロファイル)+`libbtbb.a`(ベースバンド)+`libcoexist.a`(無線共存)の合計:

- Flash: 約185KB
- DIRAM(RAM): 約22KB

現在のHost role: Flash空き約980KB(31%)、DRAM空き約194KB(43%使用)。上記を足しても Flash約780KB空き・DRAM約170KB空き程度残る見込みで、**十分収まる**(パーティション拡張不要)。ただしNimBLEの接続バッファ等は実行時heapを別途食うので、実装後は実機で`esp_get_free_heap_size()`を確認する。

## 設計案

### 1. mruby DSLの新しいsink種別として追加

既存の`sink_kind_t { SINK_TYPEC, SINK_UDP }`(`mruby_filter.c`)に`SINK_BLE`を追加し、`dsl_sink()`で`sink :name, :ble`を受け付ける。`:typec`/`:udp`と同じ枠組みに乗るので、パイプライン記述(`to :ble_out`等)や既存のfrom/to/branch構文はそのまま使い回せる。Device role側は変更不要。

有効/無効の制御は`:udp`と同様、**専用のboolean toggleは新設しない** - スクリプトが`sink :name, :ble`を宣言するかどうかだけで決まる(`usb_suspend_wifi_sleep`のような別枠の`true/false`トグルとは違う仕組み)。スクリプトが呼ばなければBLE/NimBLEスタックの初期化自体が走らない想定。

### 2. BLE HID出力の実装本体

`ble_hid_device.c`(新規)を追加し、NimBLE + `esp_hid`(`esp_hidd`、HOGP)でBLEキーボード/マウスとしてadvertise。既存の`hid_report_parser.c`が生成するレポート形式をそのまま流用できるか要確認(TinyUSB向けとBLE HID Reportディスクリプタの差異を吸収する層が必要になる可能性)。

### 3. ペアリング

- **ESP32側UIは不要**。Just Works(PINなし、MITM保護なし)でadvertise・bonding。ペアリング操作はターゲットPC側のOS標準Bluetooth設定画面のみで完結。
- ボンディング情報はNimBLEの標準ボンドストア(NVS)に任せる。
- 「別PCと再ペアリングしたい」用に、既存のWebUI(`mruby_webui.c`)へ「Unpair」ボタンを追加(Device roleにはUIが無いのでHost roleに置ける利点)。
- ステータスLED(`status_led.c`)は**WiFi用の点滅パターンとは別のパターン**にする(同じ点滅だと状態を見分けられないため)。具体的な点滅速度/回数は実装時に決めるが、例えば「WiFiは250ms間隔の単純点滅」に対し「BLEは短い二度点滅+休止」のような区別を付ける。
  - 優先度は**WiFi側を優先**: LEDは1個しか無いので、WiFi接続中(初回接続試行中の点滅)とBLE advertise中が重なった場合はWiFi側のパターンを表示し、BLE側の状態表示はWiFiが安定(接続済み=点灯)してから出す。

### 4. 電源管理との統合

`power_manager.c`のPC suspend/resume検知(既存の`usb_suspend_wifi_sleep`/`usb_suspend_rp2040_sleep`と同様のパターン)に、BLE広告の停止/再開も乗せられる余地あり - ただしこれはBLE出力自体が「ターゲットPCへの接続」を担うので、WiFi/RP2040のような「PC側USBサスペンドに連動」ではなく「BLE接続自体の切断/再接続」が主なイベントになりそう。詳細は実装時に検討。

## 決定事項

- **Type-C有線出力とBLE出力は同時使用可能**にする(排他にしない)。同じHIDイベントをType-C sinkとBLE sinkの両方へ配信する形 - スクリプトで両方の`sink`を宣言すれば両方同時に生きる。
- **複数PCとのボンディング切り替えは保留**(市販BTマウスにあるプロファイル切替のようなもの)。最初は1台とだけペアリングするMVP。

## 未決定・要相談

- ~~`hid_report_parser.c`のレポート形式とBLE HID Reportディスクリプタの整合をどう取るか(共通化 or 変換層)~~ → 実装・実機確認済み(下記参照)
- ~~LEDの具体的な点滅パターン(間隔・回数)の詳細~~ → 実装済み: WiFi接続中は既存の250ms均等トグル、BLE advertising中はON100ms/OFF100ms/ON100ms/OFF700msの「二度点滅+休止」(1秒サイクル)。優先度もプラン通り(WiFi優先、BLE状態はWiFi接続済み・非suspend時のみ表示) - [2026-09-07_ble_hid_sink_impl.md](2026-09-07_ble_hid_sink_impl.md)の「Unpair UI・LED点滅」参照

## 未検証

- ~~WiFi+BLE同時使用時のHIDレイテンシ実測(無線共存の影響)~~ → 実機検証済み: BLE経由のマウスは明らかにFPSが低い(Type-C/UDPと比べて動きが粗い)。NimBLE自身のログ垂れ流しは止められたので([2026-09-07_ble_hid_sink_impl.md](2026-09-07_ble_hid_sink_impl.md)参照)、それとは別要因 - BLE接続インターバル自体の本質的な制約の可能性が高いが未検証のまま持ち越し。
- NimBLE+esp_hidの実行時heap使用量(静的footprintのみ実測済み)
- `esp_hid`のHIDディスクリプタ層が本プロジェクトの既存レポート形式とどこまで噛み合うか → 実装・実機確認済み、噛み合った([2026-09-07_ble_hid_sink_impl.md](2026-09-07_ble_hid_sink_impl.md)参照)

## 参考

- `esp32-kvm-ip/main/mruby_filter.c`の`sink_kind_t`/`dsl_sink()` - 既存sink拡張ポイント
- `esp32-kvm-ip/main/mruby_webui.c` - 既存WebUI(Unpairボタンの置き場所候補)
- `esp32-kvm-ip/main/status_led.c` - 既存LED点滅パターン(WiFi接続用、転用候補)
- `esp32-kvm-ip/main/power_manager.c` - PC suspend/resume検知の既存実装
- ESP-IDF `examples/bluetooth/esp_hid_device`(NimBLE構成は`sdkconfig.ci.nimble`) - BLE HIDキーボード/マウスの参考実装、今回のリソース実測の元
- ESP-IDF標準コンポーネント`esp_hid`(`$IDF_PATH/components/esp_hid`) - HOGPプロファイル層。今のところこのプロジェクトの`main/idf_component.yml`には無いので、導入時は`main/CMakeLists.txt`のHOST role `PRIV_REQUIRES`に`bt`/`esp_hid`を追加する形になる
