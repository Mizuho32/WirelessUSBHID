# System Control (Sleep等)をmrubyショートカットから発行する

## 動機

ターゲットPCはUSBキーボードの物理Sleepボタン押下に反応するようになっているが、実際に繋いでいるキーボードにはそのキーが無い。「特定のキー組み合わせ(ショートカット)を押したら、そのSleep相当のHIDイベントを代わりに発行したい」という要望。

## 0x82は実はConsumer Controlじゃない

最初「Consumer Control(cc)経由で0x82を送ればいいのでは」という話が出たが、これは誤り。USB HID Usage Tablesでは:

- Consumer Control = Usage Page **0x0C**。このプロジェクトの`:consumer`/cc周りは完全にこのページ専用(`hid_report_parser.c`の`USAGE_PAGE_CONSUMER`、`usb_descriptors.c`/`ble_hid_device.c`のレポート記述子、`hid_forwarder_consumer()`等、全部Consumer Page前提)。
- Sleep(0x82)/Power Down(0x81)/Wake Up(0x83) = **Generic Desktopページ(0x01)の"System Control"コレクション**という、Consumer Controlとは別のトップレベルコレクション/別レポート。

つまり「ccとして0x82を送る」は的外れで、既存の3インターフェース(keyboard/mouse/consumer)のどれにも属さない**4つ目の出力(System Control)を新設する**必要があった。

## 実装方針: Sourceを持たない一発アクション

keyboard/mouse/consumerは全部「物理デバイスからのレポート」→「mrubyのsource/sink/pipeline(`from`/`to`/`branch`)で経路制御」という設計だが、System Controlには対応する物理デバイスが存在しない(そもそもこのキーボードにSleepキーが無いから困っている、というのが動機)。よって`source`/`pipeline`は作らず、**`system_control(:sleep, *sink_names)`という一発呼び出しのDSLメソッド**として実装した。

- `sink :name, :typec/:ble/:udp, kind: :system_control`で宛先を宣言(既存の`sink`の型/バリデーションをそのまま流用 - `kind:`の許容値に`:system_control`を追加しただけ)。
- `system_control :sleep, :sink1, :sink2, ...`が呼ばれた瞬間、指定した全sinkへ即座に送信(Ctrl+Alt+Sのようなショートカットを`:keyboard`パイプラインの`to(:typec_kbd) { |ev| ... }`ブロック内で検知して呼ぶのが典型的な使い方 - `main/mruby_scripts/default.rb`にサンプル)。
- **自動リリース付き**: 送信後`SYSTEM_CONTROL_PULSE_MS`(20ms)待って、同じsink群へ`usage_id = 0`(idle)を送る。呼び出し側が押下/離すの状態管理をする必要が無い、「1回呼べば1回分のボタン押下が飛ぶ」という単純なAPIにした(物理的な瞬間押しボタンの模倣)。

`PIPE_SYSTEM_CONTROL`という`enum`値自体は追加した(`kind_from_symbol_value()`/`dsl_sink()`の既存バリデーションに相乗りするため)が、対応する`s_pipelines[PIPE_SYSTEM_CONTROL]`/`s_net_pipelines[PIPE_SYSTEM_CONTROL]`は誰も埋めない(`from`/`source`が無いので)、永久に空のまま - 数百バイト無駄になるが、既存コードの改修量を最小化する方をとった。

## HIDレポートの中身: TinyUSBのテンプレートがそのまま使えた

`components/tinyusb/src/class/hid/hid_device.h`に`TUD_HID_REPORT_DESC_SYSTEM_CONTROL()`という既製マクロがあり、まさに欲しい形(Power Down/Sleep/Wake Upの3値 array field、1バイト)そのものだった:

```c
/* System Control Report Descriptor Template
 * 0x00 - do nothing
 * 0x01 - Power Off
 * 0x02 - Standby
 * 0x03 - Wake Host
 */
#define TUD_HID_REPORT_DESC_SYSTEM_CONTROL(...) ...
```

ポイント: 実際のワイヤ上の値は**生のHID Usage ID(0x81/0x82/0x83)ではなく、宣言順の配列インデックス(1/2/3、0=なにも押されてない)**。Consumer Controlの`usage_id`(そのまま生のUsage IDをレポートに乗せる10bitのselector field)と違って、System Controlは2bitのArray fieldで「3つのうちどれか」を1/2/3で表現する形式。

このプロジェクト内では、Consumer同様「`usage_id`(0x81/0x82/0x83、0=release)を上位レイヤ全部で使い回し、実際にワイヤに乗せる直前(`usb_device_typec_system_control_report()`/`ble_hid_device_system_control_report()`/Device role側`hid_task.c`)でだけ1/2/3への変換をする」という設計にして、`protocol.h`のUDPパケット/`mruby_filter.c`のDSL層は他のkindと同じ感覚で触れるようにした。

## 変更箇所(フルパリティ - typec/BLE/UDPの3経路とも対応)

Consumer Controlと同じ3つの出力経路すべてに対応させた(片方だけ対応、という中途半端な状態はDSLとして分かりにくくなるため):

- **`usb_descriptors.h`/`.c`**(Host/Device両ロール共有): 4つ目のHIDインターフェース`ITF_NUM_SYSCTL`追加。`TUD_HID_REPORT_DESC_SYSTEM_CONTROL()`をそのまま使用。`system_control_report_t`(1バイト)。
  - `CFG_TUD_HID`(`main/tusb_config.h`、`components/tinyusb/host_config/tusb_config.h`)と`CONFIG_TINYUSB_HID_COUNT`(`sdkconfig.defaults`)を3→4に。前者はDevice role、後者はesp_tinyusbのKconfig経由でHost roleのtype-c出力に効く(`usb_device_typec.c`の`tinyusb_driver_install()`)。
- **`usb_device_typec.c`/`.h`**(Host role、type-c出力): `usb_device_typec_system_control_report(usage_id)`追加。
- **`ble_hid_device.c`/`.h`**(Host role、BLE出力): Report ID 4としてSystem Controlコレクションをレポートマップに追加、`ble_hid_device_system_control_report(usage_id)`追加。
- **`protocol.h`**: `EVENT_TYPE_SYSTEM_CONTROL`追加、`udp_packet_t`/`hid_event_t`に`system_control.usage_id`メンバ追加(consumerと同じ8バイト枠に収まる)。
- **`hid_forwarder.c`/`.h`**: `hid_forwarder_send_system_control_to()`追加(UDP送信のみ - `hid_forwarder_consumer()`のような「物理デバイスから来たかのように処理する」入口は無い、そもそも物理ソースが無いので)。
- **`network_task.c`**(Device role、UDP受信): `EVENT_TYPE_SYSTEM_CONTROL`のcase追加。
- **`hid_task.c`**(Device role、type-c出力): 受信したイベントをusage_id→1/2/3変換して`ITF_NUM_SYSCTL`へ送信。
- **`mruby_filter.c`**: `PIPE_SYSTEM_CONTROL`、`system_control_usage_from_symbol()`、`send_system_control_to_sink()`、`dsl_system_control()`、DSL登録。

これで、Host roleがtype-c直結でもBLE経由でも、あるいはDevice role側の別ボードに中継する構成でも、`system_control`は同じ書き方で動く。

## 実機ビルド確認

Host role: ビルド成功、Flash使用率24%空き(既存の機能追加同様、ほぼ変化なし)。実機での動作確認(実際にターゲットPCがSleepすること自体)は未実施。

## 参考

- `esp32-kvm-ip/main/mruby_filter.c`の`dsl_system_control()`/`system_control_usage_from_symbol()`/`send_system_control_to_sink()`
- `esp32-kvm-ip/main/usb_device_typec.c`/`ble_hid_device.c`の`system_control_array_value()`(usage_id→wire値の変換、両ファイルに同内容を意図的に複製)
- `esp32-kvm-ip/components/tinyusb/src/class/hid/hid_device.h`の`TUD_HID_REPORT_DESC_SYSTEM_CONTROL()`
- `esp32-kvm-ip/components/tinyusb/src/class/hid/hid.h`の`HID_USAGE_DESKTOP_SYSTEM_*`
- `esp32-kvm-ip/main/mruby_scripts/default.rb`(使用例)
