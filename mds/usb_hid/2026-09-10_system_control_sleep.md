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

## フォローアップ: 生の0xXX指定 + 他のSystem Control usage対応

「他にもusageあるんじゃない? 0xXX直接指定でも送信できる?」という質問がきっかけで、初版の実装を見直した。

### 初版の制約: TinyUSBテンプレートは3値限定

初版は`TUD_HID_REPORT_DESC_SYSTEM_CONTROL()`(TinyUSBの既製テンプレート)をそのまま使っていたが、これは**Power Down/Sleep/Wake Upの3つのUsageを列挙し、2bitのArray fieldで「どれが押されたか(1/2/3、0=なし)」という圧縮インデックスを送る**方式。実際のHID Usage ID(0x81/0x82/0x83)はワイヤに乗らず、`usb_device_typec.c`/`ble_hid_device.c`側で「Usage ID → インデックス」への変換テーブル(`system_control_array_value()`)を挟んでいた。この方式のままだと4つ目以降のUsageを増やすには、レポート記述子のArray fieldを作り直す(Usageを列挙し直し、Logical Max/Report Sizeを広げる)必要があり、結局「テンプレートを捨てて自前で書く」のと変わらない。

### 再設計: Consumer Controlと同じ「Usage Min/Max直接一致」方式

Consumer Controlの`usage_id`フィールド(`TUD_HID_REPORT_DESC_CONSUMER()`)は最初から「Logical Minimum == Usage Minimum」というトリックを使っていて、**レポートに乗る値がそのまま実際のUsage ID**になっている(0 = Logical Minimum未満 = 「何も押されてない」という、HID Array fieldの標準的な解釈)。System Controlも同じ形に描き直せば、任意の連続したUsage範囲をそのまま生の値で送れる。

- `usb_descriptors.c`: `TUD_HID_REPORT_DESC_SYSTEM_CONTROL()`をやめて、`HID_USAGE_MIN_N`/`HID_USAGE_MAX_N`/`HID_LOGICAL_MIN_N`/`HID_LOGICAL_MAX_N`で手書きの記述子に置き換え(Consumer/mouseのX/Yフィールドと同じ2バイトエンコーディング - 0x81は符号付き1バイトの範囲(+127まで)を超えるため2バイト表記が必須、Usage Min/Maxは符号無し扱いなので1バイトのままでよい)。
- `ble_hid_device.c`のレポートマップも同じ範囲・同じ形に合わせて手書き修正。
- `usb_device_typec_system_control_report()`/`ble_hid_device_system_control_report()`/Device role `hid_task.c`から変換テーブル(`system_control_array_value()`)を完全に削除 - `usage_id`をそのまま1バイトにキャストして送るだけになった。
- `mruby_filter.c`: `system_control_usage_from_symbol()` → `system_control_usage_from_value()`に改名・拡張。**Integer(生のUsage ID)も受け付ける**ようになり、範囲外なら`system_control: usage 0x.. out of supported range (0x81-0x8F)`というエラーで弾く。シンボルも3つ→15個に拡充。

### 対応範囲: 0x81-0x8F(連続した15個)

`class/hid/hid.h`の`HID_USAGE_DESKTOP_SYSTEM_*`のうち、この範囲に収まる連続ブロックを丸ごとサポート:

| Usage ID | シンボル | 内容 |
|---|---|---|
| 0x81 | `:power_down` | Power Down |
| 0x82 | `:sleep` | Sleep |
| 0x83 | `:wake_up` | Wake Up |
| 0x84 | `:context_menu` | Context Menu |
| 0x85 | `:main_menu` | Main Menu |
| 0x86 | `:app_menu` | App Menu |
| 0x87 | `:menu_help` | Menu Help |
| 0x88 | `:menu_exit` | Menu Exit |
| 0x89 | `:menu_select` | Menu Select |
| 0x8A | `:menu_right` | Menu Right |
| 0x8B | `:menu_left` | Menu Left |
| 0x8C | `:menu_up` | Menu Up |
| 0x8D | `:menu_down` | Menu Down |
| 0x8E | `:cold_restart` | Cold Restart |
| 0x8F | `:warm_restart` | Warm Restart |

`system_control :context_menu, :sysctl_typec`のようにシンボルで呼んでも、`system_control 0x84, :sysctl_typec`と生のInteger(この範囲内)で呼んでも同じ結果になる。

### 対応してないもの: 0xA0番台/0xB0番台(飛び地)

この先にも`HID_USAGE_DESKTOP_SYSTEM_DOCK`(0xA0)/`UNDOCK`(0xA1)/`SETUP`(0xA2)/`BREAK`(0xA3)/`DEBUGGER_BREAK`(0xA4)/`SPEAKER_MUTE`(0xA7)/`HIBERNATE`(0xA8)、さらに`DISPLAY_INVERT`(0xB0)〜`DISPLAY_LCD_AUTOSCALE`(0xB7)といった値がUSB HID Usage Tables上に存在するが、0x8Fから飛んでいて連続していない(単一のUsage Min/Max宣言では表現できない)上、ラップトップのドック検知/デバッガ/ディスプレイ固有の用途が多く、このKVMプロジェクトで一般的に使う場面が思いつかなかったため今回は対象外にした。必要になったら2つ目のUsage Min/Maxブロックをレポート記述子に追加する形で対応可能(既存の0x81-0x8Fブロックには影響しない)。

## フォローアップ: `CONFIG_BT_NIMBLE_SVC_HID_MAX_RPTS`更新漏れ(BLE Consumer sinkが動かなくなる報告から発覚)

`sink :ble_cc, :ble, kind: :consumer`を有効にすると動かなくなる、という実機での報告を受けて調査。

`sdkconfig.defaults`に元々こういうコメントがあった:

> esp_hid's NimBLE HOGP backend (nimble_hidd.c) is entirely compiled out without this ... Its default CONFIG_BT_NIMBLE_SVC_HID_MAX_RPTS=3 already matches ble_hid_device.c's 3 report IDs (keyboard/mouse/consumer), so left unset.

System Controlを追加してBLEのreport-mode報告が4種類(keyboard/mouse/consumer/system_control)になった時、**このKconfig値を3のまま放置していた**。`esp_hid`(`components/bt/host/nimble/.../services/hid/src/nimble_hidd.c`の`create_hid_db()`)を実際に読むと:

```c
for (...) {
    if (report->protocol_mode == ESP_HID_PROTOCOL_MODE_REPORT) {
        if (report_mode_rpts >= MAX_REPORTS) {   // MAX_REPORTS = CONFIG_BT_NIMBLE_SVC_HID_MAX_RPTS
            ESP_LOGE(TAG, "Too many report-mode reports (%d >= MAX_REPORTS); truncating", report_mode_rpts);
            break;   // 以降のレポートは一切登録されない、エラーは戻り値に伝播しない
        }
        ...
        report_mode_rpts++;
    } else {
        // Boot mode複製(keyboard/mouseのみ)は別フラグ(kbd_inp_present等)で無条件登録 - このカウントに含まれない
    }
}
```

Keyboard/MouseのBoot mode複製はこのカウントに含まれない(別経路で無条件登録)。カウントされる"report-mode"エントリは、レポートマップのバイト列順(= `s_ble_hid_report_map`内の宣言順)通りに: Keyboard(1)→Mouse(2)→Consumer(3)→System Control(4)。`MAX_REPORTS=3`のまま追跡すると:

- Keyboard: `0>=3`? no → 登録、count=1
- Mouse: `1>=3`? no → 登録、count=2
- Consumer: `2>=3`? no → 登録、count=3
- System Control: `3>=3`? **yes** → **ここで無言でループごと打ち切り**(エラーはログに出るだけで、呼び出し元には伝播しない)

**この経路を厳密に追う限り、切り捨てられるのはConsumerではなくSystem Controlのはず**(Consumerはちょうど3番目で枠内に収まる計算になる)。なので、報告された「Consumerが動かなくなる」症状をこれだけで完全に説明できるかは不明 - ただし`CONFIG_BT_NIMBLE_SVC_HID_MAX_RPTS=3`が古いままなのは確実な不整合で、System Controlを使う場合は確実に踏む問題のため修正した(3→4、`sdkconfig.defaults`のコメントも更新)。

Consumer自体の不具合は、コードを読む限りでは特定できず、実機ログ(`ESP_LOGE`/`ESP_LOGW`、特に"NIMBLE_HIDD"/"BLE_HID"/"NimBLE"タグ、"Too many report-mode reports"の有無)の確認が必要。

## 実機ビルド確認

Host role: 初版・再設計版・`MAX_RPTS`修正版いずれもビルド成功、Flash使用率24%空き(既存の機能追加同様、ほぼ変化なし)。実機での動作確認(実際にターゲットPCがSleepすること、およびConsumer/System ControlのBLE経路そのもの)は未実施。Device roleのビルド確認は今回省略(Host roleが主眼のため)。

## 参考

- `esp32-kvm-ip/main/mruby_filter.c`の`dsl_system_control()`/`system_control_usage_from_value()`/`send_system_control_to_sink()`
- `esp32-kvm-ip/main/usb_descriptors.c`の手書きSystem Controlレポート記述子(`SYSTEM_CONTROL_USAGE_MIN`/`MAX`)
- `esp32-kvm-ip/main/usb_device_typec.c`/`ble_hid_device.c`の`*_system_control_report()`(もう変換テーブルは無く、usage_idをそのまま1バイトにキャストするだけ)
- `esp32-kvm-ip/components/tinyusb/src/class/hid/hid_device.h`の`TUD_HID_REPORT_DESC_CONSUMER()`(参考にした「Usage Min/Max == Logical Min/Max」トリックの元ネタ)
- `esp32-kvm-ip/components/tinyusb/src/class/hid/hid.h`の`HID_USAGE_DESKTOP_SYSTEM_*`
- `esp32-kvm-ip/main/mruby_scripts/default.rb`(使用例)
