# BLE切断後の再接続保留: 「相手を選ばず全ブロック」から「切断した相手だけ拒否」へ

## 症状

「PC/スマホから接続を切ると、30秒待たないと別の機器に繋げない」。市販のイヤホン等はすぐ別機器に繋がるのに、という指摘。

## 最初の対応(不十分だった)

`ble_hid_device.c`の`ESP_HIDD_DISCONNECT_EVENT`ハンドラに、切断理由が`BLE_ERR_REM_USER_CONN_TERM`(0x13、「相手側が明示的に切断した」)の場合、30秒間**再アドバタイズを丸ごと止める**ロジックがあった(`2026-09-07_ble_hid_sink_impl.md`で導入、目的は`ble_wifi_off_while_connected`用のWiFi窓の確保)。

最初は「`ble_wifi_off_while_connected`が有効な時だけ保留する」よう条件を絞る対応をしたが、指摘を受けて考え直した: **そもそも「意図的な切断には再接続してほしくない」という要望自体は`ble_wifi_off_while_connected`と無関係に成立する一般的な話**で、「相手を選ばず全部のアドバタイズを止める」という実装が過剰だった。別の機器を繋ぎたいだけなのに、たまたま同じ理由コード(0x13)で発火する保留のせいで巻き添えを食っていた。

## 対応: 「アドバタイズは止めない、切断した相手だけ拒否する」

**発想を変えた**: 全体のアドバタイズを止める代わりに、**アドバタイズは即座に再開したまま、直前に意図的切断した相手(の識別アドレス)だけを一定時間拒否**する。これなら:

- 別の機器はすぐ繋がる(アドバタイズ自体は止まってないので)
- 直前に切断した機器がすぐ勝手に繋ぎ直してくる、という元々の懸念にもちゃんと対応できる

### 実装(`esp_hid_gap.c`)

NimBLEの生のGAPイベントハンドラ`nimble_hid_gap_event()`(ESP-IDFのexampleからvendorしたファイル、`esp_hidd`より下のレイヤで、`ble_gap_conn_desc`経由でピアの識別アドレスに直接アクセスできる)に手を入れた:

- **`BLE_GAP_EVENT_DISCONNECT`**: 切断理由が0x13(`BLE_ERR_REM_USER_CONN_TERM`)なら、`event->disconnect.conn.peer_id_addr`(切断された接続の識別アドレス)と「いつまで拒否するか」のタイムスタンプ(`esp_timer_get_time() + 30秒`)を記録するだけ。アドバタイズの制御はここでは一切しない(`ble_hid_device.c`側が今まで通りその後に再開する)。
- **`BLE_GAP_EVENT_CONNECT`**(接続成立時): `ble_gap_conn_find()`で今繋がってきた相手のアドレスを取得し、直前に記録した「拒否対象」と一致 **かつ** まだ猶予時間内なら、`ble_gap_terminate()`で即座にこの接続を切る(`should_reject_reconnect()`)。別アドレスの相手、または猶予時間切れなら何もせず通常通り接続を受け入れる。

`ble_hid_device.c`側は逆にシンプルになった: `ESP_HIDD_DISCONNECT_EVENT`は理由コードに関わらず**常に即座に**`esp_hid_ble_gap_adv_start()`でアドバタイズ再開するだけになった(旧`s_readvertise_timer`/`readvertise_timer_cb()`/`BLE_REDISCONNECT_HOLDOFF_US`は丸ごと削除)。「誰を拒否するか」の判断は`esp_hid_gap.c`側に完全に移った。

involuntary drop(電波切れ等、理由コードが0x13以外)は元々この対象外で、影響なし。

## 未検証

- 実機でのA→B機器切り替えの実際の体感速度改善は未確認。
- 拒否した相手が短い間隔で再接続を繰り返し試行してきた場合の挙動(その都度「接続成立→即切断」を繰り返すだけで、大きな問題は無いはずだが実機未確認)。
- プライバシー機能(Resolvable Private Address)を使うOS側で、`peer_id_addr`が本当に安定した識別子として機能するか(ボンディング済みなら本来IRKで解決された同一アドレスになるはずだが、実機未確認)。

## 参考

- `esp32-kvm-ip/main/esp_hid_gap.c`の`nimble_hid_gap_event()`/`should_reject_reconnect()`
- `esp32-kvm-ip/main/ble_hid_device.c`の`hidd_event_callback()`の`ESP_HIDD_DISCONNECT_EVENT`分岐(シンプル化された側)
- `mds/usb_hid/2026-09-07_ble_hid_sink_impl.md`(holdoffの元々の導入経緯)
- `mds/usb_hid/2026-09-11_ble_dynamic_enable.md`(`ble_wifi_off_while_connected`/`ble_toggle`)
