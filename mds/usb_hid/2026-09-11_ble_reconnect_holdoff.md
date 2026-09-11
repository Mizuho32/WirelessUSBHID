# BLE切断後30秒の再アドバタイズ保留を`ble_wifi_off_while_connected`限定に

## 症状

「PC/スマホから接続を切ると、30秒待たないと別の機器に繋げない」。市販のイヤホン等はすぐ別機器に繋がるのに、という指摘。

## 原因: 保留(holdoff)が無条件にかかっていた

`ble_hid_device.c`の`ESP_HIDD_DISCONNECT_EVENT`ハンドラに、切断理由が`BLE_ERR_REM_USER_CONN_TERM`(0x13、「相手側が明示的に切断した」)の場合、`BLE_REDISCONNECT_HOLDOFF_US`(30秒)だけ再アドバタイズを保留するロジックがある(`2026-09-07_ble_hid_sink_impl.md`で導入)。

このholdoffの**存在理由**はコメントに明記されている通り「`ble_wifi_off_while_connected`と組み合わせた時、切断直後にOSが即座に再接続してしまうと、WiFi/WebUIを使う窓が実質無くなる」という一点だけだった。しかし実装は`ble_wifi_off_while_connected`が有効かどうかを見ずに、**切断理由(0x13)だけで無条件に**holdoffをかけていた。結果、この機能を使ってない(=デフォルト設定の)スクリプトでも、単に「別の機器に繋ぎ替えたい」だけの場面で無駄に30秒待たされていた。

## 対応

条件に`mruby_filter_ble_wifi_off_while_connected()`を追加(`&&`)。この機能を使っていなければholdoffは一切かからず、切断直後に即座に再アドバタイズを再開する - 市販機器と同じ挙動になる。使っている場合は従来通り30秒保留(WiFi窓の確保という元の目的通り)。

involuntary drop(電波切れ等、理由コードが0x13以外)は元々この条件に入っておらず、影響なし。

## 未検証

実機でのA→B機器切り替えの実際の体感速度改善は未確認(コードレベルでの対応)。

## 参考

- `esp32-kvm-ip/main/ble_hid_device.c`の`hidd_event_callback()`の`ESP_HIDD_DISCONNECT_EVENT`分岐
- `mds/usb_hid/2026-09-07_ble_hid_sink_impl.md`(holdoffの元々の導入経緯)
- `mds/usb_hid/2026-09-11_ble_dynamic_enable.md`(`ble_wifi_off_while_connected`/`ble_toggle`)
