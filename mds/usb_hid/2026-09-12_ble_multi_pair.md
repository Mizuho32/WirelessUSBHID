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

## 追記: `ble_toggle true`直後の`ble_pair_new`が失敗する(実機ログで発見)

実機で`ble_toggle true`の直後(同じスクリプトのtick内、5ms差)に`ble_pair_new(1)`を呼んだところ失敗:

```
E (77958) NimBLE: ble_hs_hci_cmd_send_buf rc=22
E (77959) NimBLE: error setting advertisement data; rc=22
W (77961) MRBFILT: ble_pair_new(1) failed: ERROR
I (77987) BLE_HID: started   ← ble_pair_newの呼び出しより30ms *後*に来てる
```

**原因**: `ble_hid_device_start()`は`s_started = true`を設定してすぐ返るが、これはNimBLEホストがコントローラとの同期ハンドシェイクを終える**前**("BLE_HID: started"ログ = `ESP_HIDD_START_EVENT`、実際の同期完了)。同期前に広告開始のHCIコマンドを送るとエラーになる。`ble_pair_switch`/`ble_pair_new`は`ble_hid_device_started()`(スタックが存在するか)だけをチェックしてたので、この競合を素通りしてしまってた。

**対処**: `s_host_synced`フラグを追加(`ESP_HIDD_START_EVENT`で true、start()の頭とstop()でfalse)。`ble_hid_device_ready()`(`s_started && s_host_synced`)を新設。`switch_or_new()`は`ble_hid_device_ready()`が false ならエラーにせず、`s_pending_slot`/`s_pending_pairing_mode`にキューして`ESP_OK`を返す。`ble_pair_slots_resume_on_start()`(`ESP_HIDD_START_EVENT`から呼ばれる、同期完了後)がこのpendingを最優先でチェックするようにした(NVSに永続化された「最後にアクティブだったスロット」より優先)。

「接続中に別スロットへ切り替え」用に既にあった`s_pending_slot`の仕組みをそのまま再利用しただけで、新しい状態変数は増やしていない。

ビルド・フラッシュ済み。この特定の競合(`ble_toggle true`直後の`ble_pair_new`)が直ったかは実機再検証が必要。

## 追記2: ペアリングはできるがBLE入力が全滅する(実機ログで発見)

`ble_pair_new`でペアリング自体は成功したが、キーボード/マウス/consumer全部BLE側で反応しない(type-c出力は無事 - 別経路なので無関係)。ログ:

```
I ESP_HID_GAP: subscribe event; conn_handle=1 attr_handle=49 reason=1 prevn=0 curn=1 previ=0 curi=0
E NimBLE: ble_store_config_write_cccd rc=27
```

`rc=27`は`BLE_HS_ESTORE_CAP`。`ble_gatts_clt_cfg_access()`(CCCD書き込みのATTハンドラ、実装は`ble_gatts.c`)を読むと、`ble_store_write_cccd()`の戻り値を**そのままピアへのATT応答として返してる**のを確認 - つまりストア容量オーバーは「保存し損ねるだけ」ではなく、**購読リクエスト自体がその場で失敗する**。

`CONFIG_BT_NIMBLE_MAX_CCCDS`(全ボンド合計のグローバル上限、デフォルト8)に対し、このHIDは1台につき6個のCCCD(キーボード boot+report、マウス boot+report、Consumer Control、System Control)を必要とする。1台分だけで既にほぼ上限で、3スロット分のボンドを同時に保持するようになった結果、実機で本当に溢れた。

**対処**: `sdkconfig.defaults`で`CONFIG_BT_NIMBLE_MAX_CCCDS=24`(3スロット×6 + 予備)に変更。ビルド・フラッシュ済み。

**注意**: 既存のボンド/CCCDストア(NVS)は今回の変更前に溜まった分がそのまま残ってる可能性がある。WebUIの「Unpair」ボタン(`ble_hid_device_unpair()`、`ble_store_clear()` + `ble_pair_slots_forget_all()`)で一度全部クリアしてから試すのを推奨。

## 追記3: 「意図的切断で待機」を撤回、再ペアリング関連の2バグ修正

**「意図的切断でアイドルに入る」動作を撤回**。ユーザー確認: KDE Plasma(PC側)が切断後に自動で再接続してくること自体は市販品でも同じ挙動("市販品も試したらそうだった…PC側のせいか")、Androidは再接続してこないとのこと。スロット方式ができた今、「別機器に切り替えたい」という要求自体は`ble_pair_switch`/`ble_pair_new`という明示操作で完結する(呼ぶと現在の接続を切って新しい相手へ切り替える)ので、「切断のたびに自動でアイドルへ」という動作はもう不要どころか邪魔("PCから切断してもBLE無効化自体はしないで、PCがまた接続したら接続できるようにしてほしい")。`ble_pair_slots_on_disconnect()`から`deliberate`引数を削除、切断理由に関わらず常に同じスロットへの広告を再開するだけにした。

**バグ1: `ble_toggle false`しても広告中LEDが点滅したまま。** `ble_hid_device_stop()`がLED状態を一切触ってなかった(START/DISCONNECTイベント側でしか`status_led_set_ble_advertising()`を呼んでなかった)。`stop()`の最後に`status_led_set_ble_advertising(false)`を追加。

**バグ2疑い: `ble_pair_new()`後、相手のOS側に新規ペアリング候補として出てこない。** 原因調査: `ble_pair_new()`はスロットの記録(このプロジェクト独自のNVS "blepair"マッピング)は消してたが、**NimBLE自体のボンドストア(`ble_store_config`、LTK/IRK/CCCD等)は消してなかった** - 別々のストレージ。修正として`ble_store_util_delete_peer()`を追加、スロットの記録を消す前に古いアドレスに対応するNimBLEボンドも明示的に削除するようにした。

ただし: これはESP32側を綺麗にするだけで、**相手(PC/スマホ)側が古いペアリング情報をまだ覚えてる場合、相手側の「デバイスを削除/このデバイスを削除」も別途必要**(一般的なBLEの制約で、ペリフェラル側だけ忘れてもセントラル側が覚えてれば「新規ペアリング候補」としては出てこない可能性が高い)。これはESP32側のバグというより双方の状態不一致の話なので、完全には直せない。

ビルド・フラッシュ済み。両方とも実機再検証はまだ。

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
