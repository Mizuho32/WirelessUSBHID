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

## 追記: それでも起きた「チラチラ」への対処 - 短時間の広告停止

上記のピア限定拒否だけでは実機で「PCから切ると数秒間チラチラ再接続を試みてストレス」という症状が残った。原因: 拒否は「一旦リンク層の接続を成立させてから即座に`ble_gap_terminate()`で切る」方式なので、相手のOSが送ってくる再接続の初期バーストのたびに「接続→即切断」が律儀に繰り返され、それ自体が観測可能な点滅になる。

市販のBluetoothデバイス(イヤホン等)がここでやっていそうなこと: 意図的切断の直後は**そもそもアドバタイズを出さない**。相手はアドバタイズが無ければ接続を試みる以前の段階(スキャン)で空振りするだけなので、点滅が起きようがない。多くのOSのBluetoothスタックは最初の数回だけ立て続けにリトライしてすぐ諦める(無限に叩き続けはしない)ので、その最初のバーストをやり過ごせれば十分。

### 実装(`ble_hid_device.c`)

`ESP_HIDD_DISCONNECT_EVENT`ハンドラで、切断理由が意図的切断(`BLE_ERR_REM_USER_CONN_TERM`)の場合は`esp_hid_ble_gap_adv_start()`を即座に呼ぶ代わりに、`esp_timer`のワンショットタイマー(`BLE_DELIBERATE_DISCONNECT_ADV_BLACKOUT_US`、3秒)を仕掛けて、その満了時にコールバック(`readvertise_timer_cb()`)からアドバタイズを再開する。それ以外の切断理由(電波切れ等)は従来通り即座に再開。

- 30秒の全体保留(最初にrejectされたアプローチ)と違い、**3秒だけ**の暗転なので別機器への切り替えはほぼ待たされない。
- `esp_hid_gap.c`側のピア限定拒否(30秒)はそのまま残す: 3秒の暗転を抜けた後もまだ同じ相手が食い下がってきた場合の保険。
- タイマーは`s_nimble_host_stopped_sem`と同じ「一度作ったら消さない」パターン(初回`ble_hid_device_start()`時に`esp_timer_create()`)。`ble_hid_device_stop()`の先頭で念のため`esp_timer_stop()`しておく(スタック破棄後にコールバックが発火して落ちるのを防ぐ)。

### 未検証(この追記分)

- 実機でのチラつき解消の体感確認はまだ。
- 3秒という長さの妥当性(短すぎて相手の初期バーストを抜けきれない/長すぎて別機器切り替えが微妙にもたつく、の匙加減)は未調整。

## 追記2: `ble_toggle false`でクラッシュ(実機ログで発見・別バグ)

`ble_wifi_off_while_connected true`でBLE接続後に`ble_toggle`(false方向)すると実機で`Guru Meditation Error (LoadProhibited)`。`build.host/esp32-kvm-ip.elf`が当時のフラッシュ内容と一致していたので`addr2line`でバックトレースを完全にシンボル解決でき、コードだけで原因を特定できた:

```
ble_gatts_free_mem (ble_gatts.c:1768)
 <- ble_gatts_stop (ble_gatts.c:1822)
 <- ble_hs_deinit (ble_hs.c:1132)
 <- esp_nimble_deinit (nimble_port.c:253)
 <- deinit_low_level / esp_hid_gap_deinit (esp_hid_gap.c)
 <- ble_hid_device_stop (ble_hid_device.c)
 <- ruby_ble_toggle (mruby_filter.c)
```

一つのスレッド上の同期呼び出しだけで完結していて、レースではなかった。

**根本原因**: `ble_hid_device_stop()`は`esp_hidd_dev_deinit(s_hid_dev)`(esp_hidの`nimble_hidd.c`)→その後`esp_hid_gap_deinit()`(→`esp_nimble_deinit()`→`ble_hs_deinit()`)の順で呼んでいたが、**両方とも内部で汎用の`ble_gatts_stop()`を呼んでいた**(ESP-IDFのvendorソースを実際に読んで確認。`nimble_hidd.c`の`nimble_hid_stop_gatts()`が明示的に`ble_gatts_stop()`を呼んでいて、`ble_hs_deinit()`も自身で同じ関数を呼ぶ)。NimBLEの`ble_gatts_free_mem()`はほとんどNULLガード済みだが、`BLE_DYNAMIC_SERVICE`構成下の`os_mempool_unregister(&ble_gatts_svc_entry_pool)`だけは無条件呼び出しで、2回目の呼び出しで既にunregister済みのmempoolを再度unregisterしようとしてメモリ破壊 → クラッシュ。

**対処**: `ble_hid_device_stop()`から`esp_hidd_dev_deinit()`の呼び出しを削除し、代わりに`s_hid_dev`を直接`free()`するだけにした(`esp_hidd_dev_init()`が`calloc()`した素のラッパー構造体なのでフィールドを触らず解放するだけで安全)。汎用の`ble_gatts_stop()`は`esp_hid_gap_deinit()`側の1回だけに一本化。副作用として省略される`ble_svc_hid_deinit()`/`ble_svc_hid_reset()`(dsc/chr/svcインデックスカウンタのリセット)は、`ble_svc_hid.h`のコメント通り`esp_hid_gap_deinit()`の後に明示的に`ble_svc_hid_reset()`を呼んで代替。DIS/BAS/GATT/SPSの各`_deinit()`が個別に呼んでいた`ble_gatts_free_svcs()`はNULLガード済み(`ble_gatts_stop()`側の`ble_gatts_free_svc_defs()`と同じグローバルを指すが、二重に呼んでも安全)なので、そちらを省略しても実害なしと判断。

ビルド成功、実機フラッシュ済み。

## 追記3: 追記2の修正でも別クラッシュ(実機ログで発見・第2ラウンド)

追記2の修正を試したところ`ble_hid_device_stop()`自体は`"BLE HID device stopped"`まで無事完走したが、その**直後**に別のクラッシュ。今度のバックトレース(同じく`addr2line`で完全解決):

```
ble_hs_is_enabled (ble_hs.c:456)
 <- ble_gap_adv_set_fields (ble_gap.c:4530)
 <- esp_hid_ble_gap_adv_start (esp_hid_gap.c:1059)
 <- hidd_event_callback [ESP_HIDD_DISCONNECT_EVENT branch] (ble_hid_device.c:341)
 <- esp_event の handler_execute/esp_event_loop_run/esp_event_loop_run_task
```

`esp_event_loop_run_task`上で動いてる = **非同期**。原因: 追記2で`esp_hidd_dev_deinit()`の呼び出しを削ったせいで、その内部で本来呼ばれるはずだった`ble_gap_event_listener_unregister(&nimble_gap_event_listener)`(esp_hidの`nimble_hidd.c`内の**static**なリスナー、外からunregisterする手段がない)が呼ばれなくなった。`ble_hs_deinit()`が(まだ繋がってた接続があれば)強制切断するとき、このリスナーはまだ生きてるので拾ってしまい、`ESP_HIDD_DISCONNECT_EVENT`が`esp_event`経由で非同期にこちらのコールバックへ配送される - `ble_hid_device_stop()`が完全に終わって(下手するとBTコントローラのdeinitまで終わって)から届くこともある。届いた側は「切断されたから再アドバタイズしなきゃ」と`esp_hid_ble_gap_adv_start()`を呼び、もう存在しないNimBLEホストを触ってクラッシュ。

**対処**: `s_ignore_disconnect_events`フラグを追加。`ble_hid_device_stop()`の先頭で`true`にし、`ble_hid_device_start()`の先頭で(次に呼ばれた時)`false`に戻す。`hidd_event_callback()`の`ESP_HIDD_DISCONNECT_EVENT`ケースはこのフラグが立ってたら`s_connected`更新以外は何もせず即`break`(WiFi再開もアドバタイズ再開もblackoutタイマーのarmもしない)。「stop()呼んだ後に来る切断イベントは全部無視、次のstart()が来るまでずっと無視し続ける」という単純な設計で、イベントが同期で来るか非同期で来るか・stop()の最中か後かを気にしなくて済むようにした。

ビルド・フラッシュ済み。実機での再検証(接続中に`ble_toggle false`してクラッシュしないか)はまだ。

## 追記4: 追記2/3の場当たり対応をやめ、根本対応(nimble_hidd.cのfork)に切り替え

追記2/3の対応(panicは止まったが、`s_dev`が二度と初期化できず再enable不可)を試したところ実機ログで再enable失敗を確認 (`NIMBLE_HIDD: HID device profile already initialized` / `esp_hidd_dev_init failed: ESP_FAIL`)。「今まで再enableできてるように見えてたのはpanicで再起動してたからでは」という指摘の通りだった。

原因を辿ると: `s_dev`(esp_hidの`nimble_hidd.c`内のstatic変数)をリセットできるのは`nimble_hidd_dev_deinit()`(同じくstatic、`esp_hidd_dev_deinit()`経由でしか呼べない)だけで、この関数は**必ず**`ble_gatts_stop()`も呼ぶ(スキップ不可)。つまり「`esp_hidd_dev_deinit()`を呼ばない」(→ crashは止まるが`s_dev`が永遠に残る)か「呼ぶ」(→ `s_dev`はリセットされるが`esp_hid_gap_deinit()`側の`ble_gatts_stop()`と二重に呼ばれてcrashする)の二択で、根本的に両立しない状態だった。

またこの過程で、追記1で書いた「`os_mempool_unregister`の無条件呼び出しがcrash原因」という最初の推測は誤りだったと判明(実装を読んだら「見つからなければ黙って失敗」という設計で二重呼び出し耐性アリ)。crash #1の正確なメカニズムは結局特定しきれていない(該当ビルドのELFは以後の作業で上書きしてしまい、これ以上の追跡は不可能)。

ユーザーと相談の上、以下2案から根本対応(fork)を選択:
- Option 1: コントローラ/NimBLEホストを`ble_toggle`後も常駐させ続ける(RAM回収を諦める安全策)
- Option 2: esp_hidの`nimble_hidd.c`を丸ごとこのプロジェクトにforkして`ble_gatts_stop()`の二重呼びを構造的に無くす(← 選択)

### 実装

`esp32-kvm-ip/main/nimble_hidd_fork.c`(ESP-IDFの`esp_hid`コンポーネント`src/nimble_hidd.c`の全文コピー)を新規追加。差分は3箇所のみ:

1. `nimble_hid_stop_gatts()`内の`ble_gatts_stop();`呼び出しを削除(コメントで理由を明記)。これ以降に呼ばれる`ble_svc_hid_deinit()`等の各サービスdeinitは`ble_gatts_stop()`の実行に依存していない(それぞれ独立に`ble_gatts_free_svcs()`を呼ぶだけで、これ自体NULLガード済みで冪等)ため、ここを削っても安全。`esp_hid_gap_deinit()`(`esp_nimble_deinit()`→`ble_hs_deinit()`)側の1回だけが本当のGATT全体teardownを担当するようになる。
2. `esp_ble_hidd_dev_init()`と`nimble_host_reset()`の2つの非staticなグローバル関数を`static`化(オリジナルの(fork元と別にコンパイルされ続ける)`esp_hid`コンポーネント自身の`nimble_hidd.c`とのシンボル重複を避けるため - `CONFIG_BT_NIMBLE_HID_SERVICE`を切ってオリジナルを丸ごと除外する案も検討したが、このKconfigは`ble_svc_hid.c`自体も道連れに無効化してしまうため却下)。
3. 末尾に`kvm_ble_hidd_dev_init()`という薄いラッパーを追加(`esp_hidd.c`の汎用`esp_hidd_dev_init(..., ESP_HID_TRANSPORT_BLE, ...)`と同じことをする: 外側の`esp_hidd_dev_t`をcallocして、このファイル自身の(static化した)`esp_ble_hidd_dev_init()`に委譲し、`transport`をセットするだけ)。

呼び出し側の`ble_hid_device.c`は:
- 起動時: `esp_hidd_dev_init(&s_hid_config, ESP_HID_TRANSPORT_BLE, ...)` → `kvm_ble_hidd_dev_init(&s_hid_config, ...)`に変更。
- 停止時: 追記2で入れた「`esp_hidd_dev_deinit()`を呼ばず`free(s_hid_dev)`だけ」という回避策と、それに伴う手動`ble_svc_hid_reset()`呼び出しを削除し、**素の`esp_hidd_dev_deinit(s_hid_dev)`呼び出しに戻した**(これがforkのおかげで安全になった)。

`esp_hidd_dev_input_set()`(mouse/keyboard/consumer/system_controlの毎レポート呼び出し - ホットパス)は一切変更なし。`dev->input_set`関数ポインタは(fork版の)`kvm_ble_hidd_dev_init()`が内部で設定するだけで、呼び出し側からは今まで通りesp_hidの汎用ラッパー経由で叩けるので、レポート送信経路はforkの影響を受けない。

追記3で入れた`s_ignore_disconnect_events`ガードは、fork版が(オリジナル通り)`ble_gap_event_listener_unregister()`をちゃんと呼ぶようになったことで理論上不要になったが、保険としてそのまま残した(害はない)。

ビルド成功、シンボル重複エラーなし(static化が効いている証拠)、実機フラッシュ済み。実機での再検証(`ble_toggle`の複数往復、`ble_wifi_off_while_connected true`との組み合わせ含め)はまだ。

### 参考(fork関連)

- `esp32-kvm-ip/main/nimble_hidd_fork.c`/`.h`(新規、fork本体)
- `/opt/esp-idf/components/esp_hid/src/nimble_hidd.c`(fork元)
- `/opt/esp-idf/components/esp_hid/src/esp_hidd.c`(`esp_hidd_dev_init()`/`_deinit()`/`_input_set()`の汎用ラッパー層、変更なし)
- `/opt/esp-idf/components/esp_hid/include/esp_private/esp_hidd_private.h`(`esp_hidd_dev_t`の実体、`INCLUDE_DIRS`で公開されておりforkから直接使える)

## 参考(追記)

- `ble_gatts.c`の`ble_gatts_free_mem()`/`ble_gatts_stop()`/`ble_gatts_free_svcs()`/`ble_gatts_free_svc_defs()`
- `ble_hs.c`の`ble_hs_deinit()`
- `nimble_hidd.c`の`nimble_hid_stop_gatts()`
- `ble_svc_hid.c`/`ble_svc_hid.h`の`ble_svc_hid_reset()`のドキュメントコメント("call ble_svc_hid_reset() after ble_gatts_reset()")

## 追記5: forkで直ったが、`ble_wifi_off_while_connected`時のWiFi復帰が抜けていた

追記4のfork適用後、実機で`ble_toggle true→false→true`往復・crashなしを確認("BLEはOK。ちゃんとtrue→false→trueできた。")。ただし`ble_wifi_off_while_connected true`の場合のみ別症状: `ble_toggle false`後、ステータスLEDは点いてるのにWiFiが実際には復帰せずping不通。ログ:

```
I (33849) ESP_HID_GAP: disconnect; reason=534
I (33856) BLE_HID: BLE HID device stopped
I (33857) MRBFILT: script: "#ble false"
W (33860) httpd_txrx: httpd_sock_err: error in send : 113
```

`ble_hid_device.c`側の`ESP_HIDD_DISCONNECT_EVENT`ハンドラ(`hidd_event_callback()`)が本来`wifi_manager_resume()`を呼ぶ場所だが、このログにはそのハンドラ自身の`"disconnected (reason ...)"`ログが一切出ていない - つまりこのイベントハンドラが**呼ばれてすらいない**。

**原因**: `nimble_hidd_fork.c`の`nimble_hid_stop_gatts()`(forkでも変更していない、オリジナルのまま残っていた部分)は

```c
if (s_gap_listener_registered) {
    ble_gap_event_listener_unregister(&nimble_gap_event_listener);
    s_gap_listener_registered = false;
}
if (dev && dev->connected) {
    ble_gap_terminate(dev->conn_id, BLE_ERR_REM_USER_CONN_TERM);
}
```

**GAPイベントリスナーを先にunregisterしてから**切断してる。つまり`ble_toggle false`で接続中に切る場合、実際に切断イベントが発生する頃にはこのforkの`nimble_hid_gap_event()`はもう登録解除済みで、`ESP_HIDD_DISCONNECT_EVENT`が`hidd_event_callback()`まで届かない。ログの`ESP_HID_GAP: disconnect; reason=534`は**別の**リスナー(`esp_hid_gap.c`が`ble_gap_event_listener_register`とは別に`ble_gap_adv_start(..., nimble_hid_gap_event, NULL)`で直接登録してる生のGAPコールバック)が拾ったもので、これは今回の話と無関係(esp_hid_gap.c側は元々unregisterされない)。

これはforkで新たに壊した話ではなく、**そもそも「接続中に自分からble_toggle falseする」という使い方自体、この`wifi_manager_resume()`の仕組みが最初から想定してなかった**(想定してたのは相手からの切断のみ)、という既存の抜け穴。今まではcrashで先に落ちてたので露呈してなかっただけ。

### 対応

`ble_hid_device_stop()`自身に`wifi_manager_resume()`を移設。`esp_hidd_dev_deinit()`呼び出し前に`s_connected`をスナップショットしておき(`was_connected`)、deinit後・`nimble_port_stop()`前に`was_connected && mruby_filter_ble_wifi_off_while_connected()`なら明示的に`wifi_manager_resume()`を呼ぶ - `ESP_HIDD_DISCONNECT_EVENT`が届くかどうかに依存しない、`ble_hid_device_stop()`が呼ばれれば確実に1回だけ通る場所。

ビルド・フラッシュ済み(フラッシュ後、修正前のバグでボードがWiFi死んだ状態になっていたため、ユーザーに電源再投入してもらってからOTA成功)。

## 未検証

- 実機でのA→B機器切り替えの実際の体感速度改善は未確認。
- 拒否した相手が短い間隔で再接続を繰り返し試行してきた場合の挙動(その都度「接続成立→即切断」を繰り返すだけで、大きな問題は無いはずだが実機未確認)。
- プライバシー機能(Resolvable Private Address)を使うOS側で、`peer_id_addr`が本当に安定した識別子として機能するか(ボンディング済みなら本来IRKで解決された同一アドレスになるはずだが、実機未確認)。

## 参考

- `esp32-kvm-ip/main/esp_hid_gap.c`の`nimble_hid_gap_event()`/`should_reject_reconnect()`
- `esp32-kvm-ip/main/ble_hid_device.c`の`hidd_event_callback()`の`ESP_HIDD_DISCONNECT_EVENT`分岐(シンプル化された側)
- `mds/usb_hid/2026-09-07_ble_hid_sink_impl.md`(holdoffの元々の導入経緯)
- `mds/usb_hid/2026-09-11_ble_dynamic_enable.md`(`ble_wifi_off_while_connected`/`ble_toggle`)
