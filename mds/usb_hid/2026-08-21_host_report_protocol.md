# Host側のBoot Protocol / Report Protocol整理

## 経緯
`mds/usb_hid/2026-08-21_usb_host.md`で実装したHost role(USB Hostとして実機キーボード/マウスを読む)はBoot Protocolのみ対応。wheel握りつぶし・マウス戻る/進む→Alt矢印をfilter/convで書こうとしたら、Boot Protocolにはそもそもwheel/pan、追加ボタン(4番目以降)のデータが存在しないことが判明(`hid_mouse_input_report_boot_t`はbutton1-3 + X/Yの3バイト固定)。

当初「OS起動済みなら拡張HID(Report Protocol)、起動前ならBoot Protocolにfallback」という、Device側(`hid_task.c`)と同じ自動切り替えをHost側にも期待していたが、これは勘違いだった。整理する。

## Device側とHost側でBoot/Reportの決まり方が違う

**Device側(esp32-kvm-ip本体、無改造)**: `hid_task.c`が`tud_hid_n_get_protocol()`でモードを見て分岐している。
```c
if (tud_hid_n_get_protocol(ITF_NUM_MOUSE) == HID_PROTOCOL_BOOT) {
    // BIOS/ブートローダ
} else {
    // OS起動済み
}
```
これはTarget PC側のUSBコントローラ(BIOS or OS)が`SET_PROTOCOL`要求で「今どちらのモードで喋ってほしいか」を指定してきて、TinyUSBがそれに従って自動的に両対応する。つまり「OS起動済みなら拡張、起動前ならBoot Protocol」は**Device側では既に自動的に実現済み**(今回のHost role実装より前から)。

**Host側(今回新規実装、実機キーボード/マウスを読む方)**: 根本的に違う。キーボード/マウス自体は「OSが起動してるか」を知らないし気にしない — 相手にはOSという概念自体が存在しない、ただのUSB HIDデバイス。Boot/Reportどちらで喋るかは、**そのUSBリンクの"ホスト"(=このESP32自身)がどちらを要求するかだけで決まる**。今の実装(`usb_host_task.c`の`handle_driver_connected()`)は接続時に`hid_class_request_set_protocol(handle, HID_REPORT_PROTOCOL_BOOT)`で**こちらから恒久的にBoot Protocolを要求している**。何かにフォールバックしたわけではなく、実装を簡単にするための固定選択。

結論: 「OS起動状態に応じた自動切り替え」という形の"fallback"はHost側には成立しない(判定材料となる"OS起動状態"がそもそも存在しない)。意味があるとすれば「Report Protocolを要求したが機種が対応してない場合だけBoot Protocolにフォールバックする」という互換性目的のfallackで、これは別物。

## 方針: Report Protocol対応を進める
filter/conv(wheel握りつぶし、戻る/進む→Alt矢印など)をHost側で実現するには、Report Protocolへの対応が要る。理由と作業内容:

- 常時Report Protocolを要求する(`HID_REPORT_PROTOCOL_REPORT`)
- Report Protocolのレポートフォーマットは機種依存(Boot Protocolと違い標準化された固定レイアウトがない)なので、`hid_host_get_report_descriptor()`で取得したHID Report Descriptorを自前でパースし、wheel/pan/追加ボタンのビット位置を特定する必要がある
- `usb_host_task.c`の現状「genericレポートは無視」としている経路(`HID_SUBCLASS_BOOT_INTERFACE`以外を素通ししてるだけの部分)を実装する
- filter_rules.hのフック関数シグネチャも、wheel/pan/追加ボタンを扱えるよう拡張が必要

これから実装開始。

## 実装完了・ビルド確認済み

- `main/hid_report_parser.h/.c`(新規): HID Report Descriptorの汎用パーサー。Generic Desktop(X/Y/Wheel)、Buttonページ(最大8ボタン)、Consumer AC Pan(横スクロール)のフィールドをディスクリプタから見つけてbit offset/lengthを算出する。Collection階層は追わなくてよい(Inputアイテムの出現順だけでbitオフセットが決まるため)。Report ID対応(複数レポートIDを持つ複合デバイスにも対応)。
  - **ネイティブgccでユニットテスト済み**(ESP32実機なしで検証): 標準的な5ボタン+ホイール+AC Panマウスのディスクリプタ(Report IDなし版・あり版の2パターン)を手でエンコードし、X/Y/wheel/pan/button1-5すべてのbit offset/length/signedが期待通りに算出されること、実際のレポートバイト列からの値抽出(符号付き解釈含む)が正しいこと、Report ID不一致時に0を返すことを確認。
- `main/usb_host_task.c`: マウス接続時にまずReport Protocolを要求し、`hid_host_get_report_descriptor()`で取得したディスクリプタをパース。X/Yが見つかればReport Protocol採用、見つからず尚且つBoot Interface対応機種ならBoot Protocolにフォールバック(機種互換性のためのフォールバックであり、「OS起動状態」とは無関係)。キーボードは従来通りBoot Protocol固定。
  - マウスボタンとキーボードで別々に管理していた状態を統合: `s_kbd_modifiers/keycodes`(実キーボード由来) + `s_synth_modifiers/keycode`(マウスボタン由来の合成キー)をマージして1つのキーボードUDPパケットとして送る(`send_merged_keyboard_report()`)。UDPプロトコルは差分でなく全状態を送る設計なので、これをやらないと片方が他方を上書きしてしまう。
- `main/filter_rules.h`: `filter_mouse_report()`のシグネチャをwheel/pan/synth_modifiers/synth_keycode込みに拡張し、**wheel握りつぶし・戻る(button4)/進む(button5)→Alt+左右矢印を実際に有効化**(コメント例ではなく本番コード)。button4/5→Alt+矢印はレベル(ボタンが押されてる間キー押しっぱなし)であって、エッジ検出は不要(現在のbuttons状態から毎回導出するだけで済むため)。
- `main/CMakeLists.txt`: HOST role SRCSに`hid_report_parser.c`を追加。

**ビルド確認結果**: Host role・Device role(退行チェック)とも警告/エラー0件。Host roleバイナリサイズ 0xcd0d0 bytes(20%空き)。

**未検証(実機が必要)**: 実際のマウスのReport Protocolディスクリプタがこのパーサーで正しくパースできるか、Report Protocol⇔Boot Protocolフォールバックの実際の切り替わり、button4/5→Alt矢印の実際のTarget PC側での挙動。
