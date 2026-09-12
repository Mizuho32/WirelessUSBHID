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

## 参考

- `esp32-kvm-ip/main/mruby_filter.c`の`ruby_after()`/`dsl_after_timer_cb()`
- `mds/usb_hid/2026-08-28_mruby_filter_route.md`(DSLの「実行時の性能設計」原則)
- `mds/usb_hid/2026-09-12_ble_multi_pair.md`(この機能のきっかけになったスロット切り替えの話)
