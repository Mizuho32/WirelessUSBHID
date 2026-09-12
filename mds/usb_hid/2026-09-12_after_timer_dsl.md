# `after(ms) { }` - 遅延コールバックDSL(長押し検出などに)

## きっかけ

BLEマルチペアのスロット切り替え(`ble_pair_switch`/`ble_pair_new`)を同じキーの短押し/長押しで使い分けたい、という話から: 「mrubyってThread対応してるっけ?タイムアウトする処理があると何かとほしい」という質問。

## mrubyにThreadは無い

このプロジェクトの`esp32s3_build_config.rb`は`stdlib`/`stdlib-ext`/`math`/`metaprog`しかgemboxに入れてない(`mruby-socket`を意図的に除外してるのと同じ「不要なもの削る」方針)。`Thread`クラス自体、CRubyと違いmrubyコアに標準搭載されておらず、外部mgem`mruby-thread`(pthreadベース)を別途追加しないと存在しない。

そもそもこのDSLは「1レポートごとに`mrb_funcall()`を叩く」設計(`mds/usb_hid/2026-08-28_mruby_filter_route.md`)なので、mruby側にスレッドを持たせるのは筋が悪い。

## 「長押し検出」に本当に必要なもの

**タイムアウトだけならThreadは要らない**。C側のタイマー(esp_timer)でよく、実際そういう作りにした。

重要な点: 物理キーボードが「押しっぱなしの間、同じレポートを送り続けるか」は機種依存で不明(matrix scan変化時だけ送るキーボードも普通にある)。なので「レポートが来るたびに`Time.now`で経過時間をチェックする」というmruby側だけのポーリング方式は、押しっぱなし中に一切レポートが来ない機種だと機能しない(次にレポートが来るのが離した瞬間 = 手遅れ)。

→ **実時間で確実に発火するC側タイマーが必要**、というのが結論。キー押下時に`after(ms) { ... }`でタイマーを仕掛け、離されたら(離すレポート自体は状態変化なので必ず来る)ブロック側が見る状態を更新しておけば、タイマー発火時に「まだ押されてるか」を判定できる。

## 実装

`mruby_filter.c`に追加:

- `MRB_DSL_MAX_TIMERS 4` - 同時に効かせられる`after()`の数(固定長配列、このDSLの「事前確保・動的アロケーションなし」という設計方針([[mruby_filter_route]])に合わせた)。
- `dsl_timer_slot_t { esp_timer_handle_t handle; mrb_value block; bool in_use; }` の配列 `s_dsl_timers[MRB_DSL_MAX_TIMERS]`。
- `ruby_after(mrb, self)`: `mrb_get_args(mrb, "i&", &ms, &blk)` でms + ブロックを受け取り、空いてるスロットを探して`mrb_gc_register()`でブロックをGCから保護、`esp_timer_create()`(スロット初回のみ、以後使い回し)+ `esp_timer_start_once(ms * 1000us)`。
- `dsl_after_timer_cb(arg)`: esp_timerのサービスタスク上で発火するコールバック。`mruby_dispatch_*()`と違い**呼び出し元がs_mrb_mutexを握ってない**ので自分で`xSemaphoreTake/Give`する。`invoke_block()`(`mrb_funcall_argv`、保護された呼び出し)+ 他のdispatch関数と同じMRB_TRY/MRB_CATCH+`check_error()`のパターンでブロックを実行、実行後`mrb_gc_unregister()`してスロットを解放。

キャンセルAPIは無い(v1)。押しっぱなし中に離された場合、ブロック自身が「まだ有効か」を自分で判定する前提(下記の例のように、外側で持ってる状態フラグを見る)。

## 使用例(長押し=新規ペア、短押し=切り替え)

```ruby
$combo_down_at = {}  # slot番号 => 押し始めたTime.now、離されたらnilに戻す

pipeline :keyboard do
  from :local_kbd
  to(:typec_kbd) { |ev|
    slot_keys = { 0x1e => 1, 0x1f => 2, 0x20 => 3 }  # Ctrl+Alt+1/2/3
    slot = (ev[:modifiers] & 0x05 == 0x05) ? slot_keys[ev[:keycodes].first] : nil

    if slot
      unless $combo_down_at[slot]
        $combo_down_at[slot] = Time.now
        after(600) {
          # 600ms後、まだ押されたままなら長押し確定
          if $combo_down_at[slot]
            ble_pair_new(slot)
            debug_print "long-press slot #{slot}: pair_new"
            $combo_down_at[slot] = nil
          end
        }
      end
      next nil
    end

    # このコンボが押されてなかった(=離された/別のキー)場合、
    # 直前まで押されてた分があれば短押し確定
    $combo_down_at.each { |s, t|
      next unless t
      ble_pair_switch(s) if Time.now - t < 0.6
      $combo_down_at[s] = nil
    }
    ev
  }
end
```

## 未検証

- 実機での長押し/短押し判定の実際の体感・タイミング調整は未確認。
- `after()`のコールバックがesp_timerサービスタスク上で`s_mrb_mutex`を取得する際、他の重い処理(`ble_hid_device_stop()`等、長時間ブロックしうる呼び出し)がこのmutexを握ってる間はesp_timerサービスタスク自体が足止めされる - 他のタイマー(システム全体で共有)にも影響しうる。今のところ問題になってないが、`after()`の使用が増えた場合は要注意。

## 追記: 実機で発覚した重大バグ - ブロック内の例外がVM全体を巻き込んで壊す

実機テスト(`data/mruby_scripts/examples/wheel_to_udp_only3.rb`、`after()`でBLEペアリングの長押し判定)で、BLE無効状態のまま長押しコンボを叩いたところ:

```
E MRBFILT: after(): block raised past its own protected call (VM-level error) - script bug
```

というログの後、BLEの有効化/無効化もWiFiの挙動もめちゃくちゃになる(フリーズに近い)という症状が出た。

### 原因

スクリプト側の`after(){}`ブロックに`return if !ble_started? || nil`というガード節があった。`return`は**そのブロックを定義したメソッドの呼び出しフレーム**に戻ろうとする(mrubyもCRubyと同じ非局所returnの意味論)。だが`after()`のタイマーが発火するのは、そのブロックを作った`hook_ble`の呼び出しがとっくに終わった後、しかも別のタスク(esp_timerサービスタスク)上での`invoke_block()`経由の呼び出しなので、戻る先のフレームはもう存在しない。mruby内部はこれを`E_LOCALJUMP_ERROR`として例外化する。

ここまでは「スクリプトのバグ」として想定内(ドキュメント本文の「キャンセルAPIは無い」の節で触れている、ブロックが持ってるべき前提が崩れるケースの一種)。だが、実際に壊れたのは**この例外の後始末をこちら(`dsl_after_timer_cb`)がサボっていたから**だった。

`mruby`の`mrb_funcall_with_block()`(`components/mruby/mruby/src/vm.c`)は、`if (!mrb->jmp) { ...自前でMRB_TRY/CATCHを張る... }`という作り - **呼び出し側がすでに`mrb->jmp`をセットしていたら、この自前の保護をまるごとスキップする**。`dsl_after_timer_cb`は(他のdispatch関数と同じ作法で)`invoke_block()`を呼ぶ前に`s_mrb->jmp = &c_jmp`を自分でセットしていたので、ブロック内で起きた例外は`mrb_funcall_with_block()`の内部保護を素通りして、直接こっちの`MRB_CATCH`まで飛んできていた。

他のdispatch箇所(`mruby_dispatch_*()`)は同じ構造の`MRB_CATCH`内で`check_error()`(`s_mrb->exc`をログ出力してNULLに戻す)を呼んでいるが、書いたばかりの`dsl_after_timer_cb`はこの`check_error()`の呼び出しを書き忘れていた。結果、`s_mrb->exc`が立ちっぱなしのまま残る。mrubyの`vm.c`はcfunc呼び出し直後に`if (mrb->exc != NULL) mrb_exc_raise(...)`という形で**残ってる例外を再送出する**ので、以後あらゆるmrubyディスパッチ呼び出し(BLE toggleもWiFi関連の処理も全部)がこの「使い回された古い例外」に巻き込まれて次々失敗する - 実機で見えた「BLE有効化/無効化もWiFiもおかしい、色々おかしくなる」という全体崩壊症状の正体はこれだった。

### 修正

- `dsl_after_timer_cb`の`MRB_CATCH`内に`check_error();`を追加(他のdispatch箇所と同じ形に統一)。これで単発のブロック内エラーはそのタイマー1回分だけがログに出て消える扱いになり、VM全体を巻き込まなくなる。
- `wheel_to_udp_only3.rb`側も`return if !ble_started? || nil` → `next unless ble_started?`に修正。上の修正後も`return`はこの「戻り先が無い」例外を毎回起こすだけで、意図した「早期リターンでガード」としては機能しない(ブロック全体が例外で中断されるだけ) - `after(){}`(に限らず、このDSLのブロック全般)でのガード節は`return`ではなく`next`を使うのが正しい。

### 教訓

`after(){}`に限らず、このDSLの「ブロックを定義時のフレームから切り離して後から`invoke_block()`で叩く」という設計([[mruby_filter_route]])全般に言えること: **ブロック内で`return`を使ってはいけない**(呼び出し元フレームがすでに無いので必ず例外化する)。ガード節・早期終了は`next`を使う。

## 追記2: pair_newの鍵消去タイミングを切断完了後に変更

上のバグ調査と並行して、「ペアリング済みスロットに`ble_pair_new()`を発行しても、PC/スマホ側のペアリング候補一覧に出てこない」という別件も報告された。

`ble_pair_slots.c`の`switch_or_new()`は、pairingモードの場合`ble_store_util_delete_peer()`(NimBLE側のLTK/IRK等の鍵消去)を**接続中かどうかのチェックより前**、つまり相手とまだ生きた暗号化リンクが繋がってる最中に即座に呼んでいた。そのすぐ後で`ble_hid_device_disconnect_current()`により切断している - 相手からすると「暗号化リンクの鍵を抜かれてから切られる」という順序になる。

`erase_old_bond()`という関数に切り出し、実際に`activate(slot, true)`を呼ぶ直前(=対象スロットが本当にアドバタイズ開始されるタイミング、切断が絡む場合は切断完了後)にだけ呼ぶよう変更。相手が接続中だった場合は「まず普通に切断される→切断完了後に鍵を消してペアリングモードに入る」という順序になる。

ただし、これで「相手のペアリング候補一覧に出るようになる」保証はできない、という点は本体([[ble_multi_pair]])に書いた非対称ボンドの制約のまま: 相手のOSが自分の側でまだ「ボンド済み」と覚えている限り、こちらがどれだけ綺麗に忘れても相手の候補一覧には出てこない可能性がある。実機再確認待ち。

## 追記3: pair_newしても「元の相手」が即座に再確保してしまう問題

追記2の鍵消去タイミング修正後も再テストしたところ、症状は変わらなかった: `ble_pair_new(1)`発行→PC/スマホ側の候補一覧に出ない→BLE再起動して`ble_pair_switch`しようとすると`ble_pair_slot_bonded?(1)`が`true`のまま(=まだ何か覚えてる)。

### 原因

`ble_pair_new()`は「未知の新しいデバイスなら誰でも繋げる」ようにするため、無方向(undirected)アドバタイズにする設計([[ble_multi_pair]])。だが、こちらが消したはずの**古いPC自身がまだ近くにいて、まだボンド済みだと思っている**場合、無方向アドバタイズが始まった瞬間、ユーザーが別デバイスでペアリング操作をする前に**古いPCが自分から再接続を試み、そのまま新しいボンドを張り直してしまう**。

`ble_pair_slots_on_connect()`は「pairingモード中に繋がってきた相手を無条件でそのスロットに記録する」実装だったので、結果的に同じ古いPCが再びslot1に書き込まれる - これが「pair_new発行しても現れない(古いPCが即座に再確保している)」「再起動後もbonded 1のまま」という観測の正体だった。

### 修正

`erase_old_bond()`が消したアドレスを`s_avoid_addr`として覚えておき、pairingモード中にその**同一アドレス**から接続要求が来た場合は`ble_pair_slots_on_connect()`内でスロットに記録せず、`ble_gap_terminate()`で即切断 + NimBLE側のボンドも念のため再削除(SM再ペアリングがコールバックより先に完了してしまうレースへの保険)し、pairingモードは維持したまま「別のデバイス」からの接続を待ち続けるようにした。

これで理屈上は「古いPCの自動再接続を無視して、新しいデバイスの接続だけ受け付ける」形になるはず。ビルド・フラッシュ済み、実機再確認待ち。

## 追記4: pair_newをフルBLE再起動にしたら今度はクラッシュした

追記3の「古い相手を弾く」修正後も再テストしたところ、症状は変わらなかった: 候補一覧には出るがペアリング処理自体が失敗する(相手側で"forget"してもダメ、PC/スマホ側のBluetooth再起動ではなく**ESP32側のBLE再起動**をしてから`pair_new`するとうまくいく、という報告)。

### 原因調査 → いったんの結論

`ble_hid_device_stop()`(`ble_toggle false`相当)は単なるアドバタイズ停止ではなく、`nimble_port_stop()` + `esp_hid_gap_deinit()`(NimBLEホスト全体 + BTコントローラそのもの)を完全に畳んで作り直す。一方それまでの`ble_pair_new()`は`ble_gap_adv_stop()`+`esp_hid_ble_gap_adv_start()`だけ(アドバタイズの再起動のみ)。ユーザーが手動で試して効いた「BLE再起動→pair_new」の"再起動"部分こそが本質だろうと判断し、`ble_pair_new()`自体に`ble_hid_device_stop()`+`start()`のフルリスタートを内蔵させた(this file - `ble_pair_slots.c`)。

### 実機でクラッシュ

ところがこれで今度はボード自体がクラッシュした:

```
BLE HID device stopped
BLE_INIT: BT controller compile version...   ← 即座にstart()
...34ms後...
E task_wdt: IDLE0 (CPU 0)が反応してない (esp_timerタスクがCPU0占有)
assert failed: xQueueSemaphoreTake queue.c:1713 (pxQueue->uxItemSize == 0)
```

ユーザーから「WiFi由来なら特例化できる?」と聞かれたが、ログを見る限りWiFiは無関係だった(クラッシュ前後に`wifi_manager_suspend/resume`のログが一切出ていない - 直前の切断で`s_connected`は既にfalseになっていたため、`ble_hid_device_stop()`内のWiFi復帰処理自体が実行されていない)。

`assert failed: xQueueSemaphoreTake queue.c:1713 (pxQueue->uxItemSize == 0)`は、セマフォのはずのハンドルが実は(item sizeが0でない)普通のキューになっている、という不整合 - 典型的な「解放されたはずのFreeRTOSプリミティブを使い回そうとして中身が別物にすり替わってる」系のクラッシュパターン。`ble_hid_device_stop()`の`xSemaphoreTake(s_nimble_host_stopped_sem, ...)`はNimBLE**ホスト層**の後始末を同期化するだけで、その下のBTコントローラ(無線ハードウェア側)は`esp_bt_controller_deinit()`直後に間髪入れず`init()`すると、ハードウェアが完全に落ち着く前にぶつかる、というESP32 BTの既知の癖と見られる。手動で`ble_toggle false`→`true`を別々に叩いていた時は、人間の操作の間に自然と間が空いていたので踏んでいなかった。

### 修正

`ble_hid_device_stop()`と`ble_hid_device_start()`の間に`vTaskDelay(pdMS_TO_TICKS(300))`を挟んだ。場当たり的ではあるが、手動操作で自然に生まれていた"間"を明示的に再現しただけとも言える。ビルド・フラッシュ済み、実機再確認待ち。

この300msぶん、`ble_pair_new()`の呼び出しは(元々`ble_hid_device_stop()`自体がブロッキングなのに輪をかけて)さらに長く`s_mrb_mutex`を握ったまま止まる - 他のディスパッチ呼び出しがこの間待たされる。今のところ許容範囲と判断しているが、体感で問題になるようなら要再検討。

## 追記5: フルBLE再起動アプローチをrevert(delayを入れてもクラッシュがループする)

追記4の`vTaskDelay(300ms)`を入れたビルドを試したところ、症状が変わった: クラッシュ自体は起きるが**ループする**という報告。ログのタイムスタンプを見ると`BLE HID device stopped`→`BLE_INIT`が確かに300ms空いており(待ち自体は効いていた)、それでもBLE_INIT開始からわずか31msでtask watchdog→クラッシュに至っていた。

「delayを300ms入れても直らない」ことから、単純な待ち時間不足ではなく、`ble_hid_device_stop()`直後に`ble_hid_device_start()`を呼ぶこと自体がこの機体/IDFの組み合わせで根本的に不安定、と判断。delay値を弄って様子見を続けるのは実機がクラッシュループする以上リスクが高いため、**このアプローチ全体をrevertした**:

- `ble_pair_new()`は追記3までの状態(NVS/NimBLEボンド消去 + undirectedアドバタイズ再開のみ、フルスタック再起動なし)に戻した。「古い相手を弾く」ロジック(追記3)はクラッシュと無関係だったのでそのまま維持。
- つまり「pair_newだけで確実に新規ペアリングできる」という当初の目標(このuser問い合わせの発端)は**未達のまま**。ペアリング直後にもう一度pair_newするなど、相手が古い鍵をまだ覚えていそうな場面では、引き続き手動で`ble_toggle false`→(実時間を置く)→`ble_toggle true`→`ble_pair_new()`という3ステップを踏むのが確実な回避策。

### 今後の選択肢(未着手)

- delay値をさらに増やす(500ms〜1秒等)方向は、今回「300msでも直らずクラッシュがループした」実績を踏まえると、当てずっぽうで実機を壊すリスクの方が大きいと判断し、いったん保留。
- 代わりに検討する価値がありそうな方向: `ble_hid_device_stop()`と`start()`を同一関数呼び出し内で同期的に連続実行するのではなく、`after()`で本当に実時間の間隔を空けて2段階の script tick に分ける(stopしてscriptに戻り、後続のafter()タイマーでstartを呼ぶ)。これなら他のタスク(esp_timerサービスタスクも含め)が挟まる分、今回のクラッシュの根本原因(コントローラ再初期化のレース?)を回避できる可能性があるが、未検証。

## 追記6: 「古い相手を弾く」ロジック(追記3)もrevert - ユーザーの判断

追記5でフルBLE再起動(追記4)をrevertした後、ユーザーから「拒否するロジック(追記3のs_avoid_addr)を入れる前の状態に戻せる? そっちの方がマシな挙動だった」との指示。

理由: 追記3の拒否ロジックは、実際には狙った場面(追記4の調査でログ解析した「たった今ボンドしたばかりの相手が古い鍵のまま再接続してくる」ケース)では発火すらしておらず(アドレス比較が一致しなかった - おそらくRPAのローテーション等)、効果がはっきりしないまま複雑さだけ増えていた。一方、拒否ロジックが無い状態(追記2までの、鍵消去タイミングの是正のみ)の挙動 - 「古い相手が即座に再ボンドしてしまい、pair_newを再度発行する必要がある(手間)」 - の方が、少なくとも実害(ペアリング処理そのものの失敗やクラッシュ)は無く、まだ許容できる、というユーザー判断。

### 対応

`ble_pair_slots.c`/`.h`/`esp_hid_gap.c`から`s_avoid_addr`/`addr_eq()`/`ble_pair_slots_on_connect()`内の拒否分岐/`conn_handle`引数を全部revert。`ble_pair_slots_on_connect()`は元の`(const ble_addr_t *peer_addr)`シグネチャに戻した。`erase_old_bond()`も鍵消去のみ(avoidアドレス記録なし)に戻した。

現状の`ble_pair_slots.c`は「追記2まで」の状態と同じ: NimBLEボンド+NVSスロット記録の消去タイミングを是正済み(切断完了後に消去)、拒否ロジック無し、フルBLE再起動無し。ビルド・フラッシュ済み。

## 未検証(更新)

- 追記1・2の修正(`check_error()`追加、鍵消去タイミング変更)は現行状態に含まれる。
- 追記3(古い相手の再接続拒否)・追記4(フルBLE再起動)はいずれもrevert済み。
- 長押し/短押し判定自体の体感タイミングも引き続き未確認。

## 参考

- `esp32-kvm-ip/main/mruby_filter.c`の`ruby_after()`/`dsl_after_timer_cb()`
- `mds/usb_hid/2026-08-28_mruby_filter_route.md`(DSLの「実行時の性能設計」原則)
- `mds/usb_hid/2026-09-12_ble_multi_pair.md`(この機能のきっかけになったスロット切り替えの話)
