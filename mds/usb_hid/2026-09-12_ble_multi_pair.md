# BLEマルチデバイスペアリング「スロット」- 市販キーボードのFn+1/2/3方式

## 経緯

`mds/usb_hid/2026-09-11_ble_reconnect_holdoff.md`で「切断した相手だけを一定時間rejectする」(接続受理→即切断)方式を実装したが、実機で以下が発覚:

- PCが切断後に高頻度(0.25秒間隔)で再接続を試み続け、そのたびに「accept→reject判定→即切断」の全サイクルが回る
- しかもreject判定(`esp_hid_gap.c`)とHID接続処理(`nimble_hidd_fork.c`、CONNECT/DISCONNECTイベント経由で`ble_wifi_off_while_connected`のWiFi制御をする側)は**独立した別々のGAPイベントリスナー**なので、rejectが決まっても接続処理側は"普通に繋がった"扱いで一通りフル処理(WiFi停止等)を済ませてしまう
- この頻度でWiFi停止/再開・BLEイベントループが回ってるところに`ble_toggle false`(全停止)が割り込むと、`esp_event_loop_delete()`がイベントループ用タスクを消そうとした瞬間にクラッシュ(`addr2line`で完全解決、`ble_hid_free_config → esp_event_loop_delete → FreeRTOSのqueue内部`)

対症療法(排他制御等)も検討したが、そもそも「切断のたびにaccept→reject」という設計自体が「相手を選べない」広告方式(誰でも繋げる undirected advertising)を前提にしているのが根本原因。ユーザーから「市販キーボードのマルチペア機能(Fn+1/2/3的な)をやりたかった」という話が出て、NimBLEの**ダイレクト広告(directed advertising)**と組み合わせれば根本解決できると判明したので、これを実装した。

## 設計

**マルチポイント(同時複数接続)ではない**。esp_hid/このプロジェクトの`nimble_hidd_fork.c`はBLE HID出力層が単一接続前提(`esp_ble_hidd_dev_s`の`conn_id`/`connected`が単一フィールド)で、そこを複数接続対応にするのは大改造になる。今回やったのは「複数のボンド先を覚えておいて、明示的に切り替える」方式(市販キーボードのFn+1/2/3と同じ)。同時に繋がるのは常に1台だけ。

### ダイレクト広告(directed advertising)

NimBLEの`ble_gap_adv_start()`は`direct_addr`引数に特定の相手のBLEアドレスを渡すと、**その相手の端末からしか繋げられない広告**になる(`conn_mode = BLE_GAP_CONN_MODE_DIR`)。これを使うと:

- 「スロットnに切り替え」→ そのスロットにボンド済みの相手宛にダイレクト広告 → **その相手以外は繋がりようがない**(accept→reject後処理が不要)
- 「スロットnに新規デバイスをペアリング」→ 従来通りの誰でも可広告(undirected) → 繋がってきた相手のアドレスをそのスロットに記録

### スロットの状態遷移

- **意図的切断**(理由0x13、相手側が明示的に切断): 今アクティブなスロットを「アイドル」にし、**広告を一切しない**。同じ相手が何度再接続を試みても、そもそも広告してないので繋がりようがない(accept→reject自体が発生しない)。次に`ble_pair_switch`/`ble_pair_new`を明示的に呼ぶまで放置。
- **非意図的切断**(電波切れ等): 今のスロット/モードのまま自動で広告再開(通常のBLE機器の自動再接続と同じ)。

これにより、旧`should_reject_reconnect()`(接続受理→即reject)の仕組みが完全に不要になった。丸ごと削除。

### スロット数・永続化

- 3スロット(`BLE_PAIR_SLOT_COUNT`)。NVS namespace `blepair`に、各スロットのボンド済みピアアドレス(`ble_addr_t`、7バイトblob、キー"slot1"〜"slot3")+ 最後にアクティブだったスロット番号(キー"active")を保存。再起動/`ble_toggle`サイクルをまたいで同じ相手に自動で繋がりに行く。
- スロットが一度も使われてない(値が無い)場合、起動時は広告せずアイドルのまま待機。

## DSL

- `ble_pair_switch(n)` (n=1..3): スロットnの既存ボンド先へダイレクト広告。ボンドが無ければ`ESP_ERR_NOT_FOUND`警告。BLEスタック未起動なら`ESP_ERR_INVALID_STATE`警告。
- `ble_pair_new(n)`: スロットnの既存ボンドを破棄し、誰でも可広告(新規ペアリング)。次に繋がってきた相手をこのスロットに記録。
- `ble_pair_slot`: 現在アクティブなスロット番号(Integer)、アイドルなら`nil`。
- `ble_pair_slot_bonded?(n)`: スロットnにボンド済みデバイスがあるか。

いずれも接続中に呼ばれた場合、まず現在の接続を切断(`ble_hid_device_disconnect_current()`、非同期)し、その切断が実際に完了してから実際の切り替えが発動する(`ble_pair_slots_on_disconnect()`の"pending"分岐)。

`mruby_scripts/default.rb`にキーボードコンボの実装例(Ctrl+Alt+1/2/3)を追加した。

## 実装ファイル

- `main/ble_pair_slots.c`/`.h`(新規) - スロットの状態機械・NVS永続化・DSLから呼ばれる本体
- `main/esp_hid_gap.c`: 
  - `esp_hid_ble_gap_adv_start()`が`direct_addr`引数を取るように変更(`NULL`=従来通り誰でも可、非NULL=ダイレクト広告)
  - 旧`should_reject_reconnect()`/`s_reject_peer_addr`等を全削除
  - `BLE_GAP_EVENT_CONNECT`/`BLE_GAP_EVENT_DISCONNECT`から`ble_pair_slots_on_connect()`/`_on_disconnect()`を呼ぶ(このファイルは元々ピアアドレス・切断理由への生アクセスがあるので、higher-levelな`ble_hid_device.c`側のイベントデータ(アドレス無し)を経由する必要がない)
- `main/ble_hid_device.c`: 
  - `ESP_HIDD_START_EVENT`/`ESP_HIDD_DISCONNECT_EVENT`の広告制御を`ble_pair_slots_resume_on_start()`/(esp_hid_gap.c側の処理に一任)に置き換え。旧`s_readvertise_timer`(3秒blackout)の仕組みは丸ごと削除(ダイレクト広告でそもそも要らなくなった)
  - `ble_hid_device_disconnect_current()`を新規追加(`kvm_ble_hidd_dev_disconnect()`経由)
  - `ble_hid_device_unpair()`(WebUIの"Unpair"ボタン)が`ble_pair_slots_forget_all()`も呼ぶよう更新(全スロット+全ボンドを一括削除)
- `main/nimble_hidd_fork.c`/`.h`: `dev_p->disconnect`がupstreamでは一度も配線されてなかった(esp_hidd.c/nimble_hidd.c双方の既存ギャップ、このforkが元々持ってた問題ではない)ので、`nimble_hidd_dev_disconnect()`を追加して配線。公開ラッパー`kvm_ble_hidd_dev_disconnect()`も追加。
- `main/mruby_filter.c`: `ble_pair_switch`/`ble_pair_new`/`ble_pair_slot`/`ble_pair_slot_bonded?`のDSL登録

## 未検証

- 実機でのダイレクト広告の動作確認(意図した相手だけ繋がるか)は未確認。
- ダイレクト広告はADV_DIRECT_INDパケットを使うため広告データ(name/UUID等)を含められない - `esp_hid_ble_gap_adv_start()`はdirect_addr指定時`ble_gap_adv_set_fields()`をスキップするようにしたが、これがNimBLE側で本当に必要な回避か(呼んでもエラーにならないだけなのか)は未検証。
- OS側のプライバシー機能(Resolvable Private Address)がある場合、`peer_id_addr`(ボンディング後にIRKで解決される識別アドレス)が本当に安定してダイレクト広告のターゲットとして機能するかは未検証。
- スロット切り替え(`ble_pair_switch`/`_new`呼び出し→現在接続を切断→pending切り替え発動)の実際の往復動作は未検証。
- `ble_pair_switch`/`_new`をBLEスタック未起動時に呼んだ場合のエラーメッセージ通り、`ble_toggle true`を先に呼ぶ必要がある - スクリプト側での組み合わせ方は`default.rb`の例次第。

## 参考

- `mds/usb_hid/2026-09-11_ble_reconnect_holdoff.md`(これが置き換えた旧設計の全経緯)
- `mds/usb_hid/2026-09-11_ble_dynamic_enable.md`(`ble_toggle`/`ble_dynamic`)
- NimBLEの`ble_gap_adv_start()`/`struct ble_gap_adv_params`(`BLE_GAP_CONN_MODE_DIR`/`high_duty_cycle`)
