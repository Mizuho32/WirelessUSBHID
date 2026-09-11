# BLE HIDスタックの動的ON/OFF(`ble_dynamic`/`ble_toggle`)

## 動機

`sink :xxx, :ble, ...`を宣言すると、今まではボード起動時に自動でNimBLE/BTスタックが立ち上がり、そのまま起動中ずっと常駐し続ける一択だった。「普段はOFF(RAM/WiFi干渉を避ける)、ショートカットが押されたらON」という使い方をしたい、という要望。`sink :ble`自体は「宛先を宣言する」だけで、実際にスタックを起動するかどうかの決定はもっと別の場所(起動シーケンス中の一点)にあった - この切り離しがどこまで可能か調べた。

## 調査: 起動/終了は本当に対称にできるか

`ble_hid_device_start()`(既存実装)がやっていること:
1. `esp_timer_create()`(切断後の再アドバタイズ保留用タイマ)
2. `esp_hid_gap_init()` → BTコントローラ初期化・enable、`esp_nimble_init()`
3. `esp_coex_preference_set(ESP_COEX_PREFER_BT)`
4. `esp_hid_ble_gap_adv_init()` → GAP/セキュリティパラメータ設定
5. `esp_hidd_dev_init()` → HID/GATTサービス登録
6. `ble_store_config_init()` → NVSボンディングストア配線
7. `nimble_port_freertos_init(nimble_host_task)` → NimBLEホストタスク起動(`nimble_port_run()`を回し続ける)

これの逆(stop)が本当に安全にできるか、`esp_hid_gap.c`(ESP-IDFのexampleからvendorされたファイル)のソースを読んで確認した。結果: **既に対称なdeinit関数が用意されていた**(vendor元のexampleが元々BT/BLEモード切り替えデモを想定していたと思われる):

- `esp_hid_gap_deinit()`(既存、今まで一度も呼ばれていなかった) → NimBLEなら`esp_nimble_deinit()` + `esp_bt_controller_disable()` + `esp_bt_controller_deinit()`
- `esp_hidd_dev_deinit()`(`esp_hid`コンポーネント自体が提供) → 中身(`esp_hid/src/nimble_hidd.c`の`nimble_hid_stop_gatts()`、ソース確認済み)は「接続中なら`ble_gap_terminate()`で切断 → `ble_gatts_stop()` → HID/DIS/BAS/SPS/GATT/GAPサービスを個別にdeinit」を既にやってくれる

つまり「対称なstopを新設する」というより、**既存の(未使用だった)deinitパスをつなぎ直すだけ**で済んだ。

## 実装

### 起動/終了シーケンス(`ble_hid_device_stop()`、新規)

`ble_hid_device_start()`の逆順:

1. 再アドバタイズ保留タイマを止める(消さない - 後述)。放置すると、deinit後にコールバックが発火して既に無いBTコントローラへ`esp_hid_ble_gap_adv_start()`しに行く事故になる。
2. `ble_gap_adv_stop()`(アドバタイズ中でなければ黙って無視)
3. `esp_hidd_dev_deinit()` - 接続中なら切断も含めて丸ごと片付く
4. `nimble_port_stop()` + セマフォ待ち → NimBLEホストタスクの終了を確認してから
5. `esp_hid_gap_deinit()`(BTコントローラのdisable/deinit)
6. `esp_coex_preference_set(ESP_COEX_PREFER_WIFI)` - BT優先だった設定を戻す

### `nimble_port_stop()`は非同期 → セマフォで同期化

`nimble_port_stop()`はNimBLEホストのイベントループに「止まれ」と要求するだけで、実際に止まったことをブロックして待ってはくれない。`nimble_port_run()`が実際に返ってきてから`nimble_port_freertos_deinit()`(このタスク自身を`vTaskDelete(NULL)`で消す)を呼ぶ、という流れなので、`ble_hid_device_stop()`側が「ホストタスクが本当に終わった」のを確認してからBTコントローラのdisable/deinitに進まないと、ホストタスクがまだ後片付け中のところへコントローラを引っこ抜く競合になりうる。

対策: `nimble_host_task()`内、`nimble_port_run()`が返った直後(`nimble_port_freertos_deinit()`でこのタスク自体が消される直前)にバイナリセマフォを`give`し、`ble_hid_device_stop()`側は`nimble_port_stop()`の直後にそのセマフォを`take`して待つ。ESP-IDFのNimBLE系サンプルでよく見るstop/restartの定番パターン。

### タイマ・セマフォは「常駐」、スタック本体だけ動的

`debug_stream.c`(`mds/usb_hid/2026-09-10_mruby_debug_stream.md`)と同じ設計判断: 再アドバタイズ用`esp_timer_handle_t`とホスト停止待ち用`SemaphoreHandle_t`は初回`ble_hid_device_start()`で一度だけ作り、以降は`stop()`で消さずそのまま使い回す(固定・小さいコストのプリミティブは不滅にしておく方が、毎回作り直す複雑さを避けられる)。実際にRAM/無線を食う本体(BTコントローラ・NimBLEホスト・GATTサービス)だけがstart/stopのたびに生成・破棄される。

## DSL

- **`ble_dynamic true`**(スクリプトのトップレベルでのみ意味を持つ): デフォルトfalse = 従来通り「`:ble`宛先が1つでもあれば起動時に自動でスタート」。trueにすると、`main_host.c`はそのチェックをスキップする(`mruby_filter_ble_sink_declared() && !mruby_filter_ble_dynamic()`)。**この判定は`mruby_filter_init()`が返った直後、ディスパッチが始まる前の一点でしか行われない**ため、ランタイム中(ショートカット検出後など)に`ble_dynamic`を呼んでも手遅れ - あくまでスクリプト本体のトップレベルで宣言するもの。
- **`ble_toggle(true/false)`**: `ble_hid_device_start()`/`_stop()`を直接呼ぶ一発アクション。`system_control`と同じ「いつでもどこからでも呼べる」設計 - 典型的には`:keyboard`パイプラインの`to`/`branch`ブロック内でショートカット検出時に呼ぶ。
- **`ble_toggle()`(引数無し)= トグル**(フォローアップ、後述)。

## 配線(宣言的)と生死(動的)は別軸 - `sink`/`to`/`branch`はいつも通り常に有効

「`to`/`branch`って宣言的・静的に配線を組む仕組みなのに、`ble_dynamic`みたいな動的ON/OFFと相性悪くない?」という疑問が出たが、実際には競合しない。**`sink`/`to`/`branch`による配線は、`ble_dynamic`の値に関わらず、スクリプト読み込み時に無条件でいつも通り登録される** - これは一切変えていない:

- `sink :name, :ble, kind: :xxx` → `s_sinks[]`への登録は毎回そのまま起きる。
- `to :typec_kbd, :ble_kbd`(pipeline内) → `s_pipelines[PIPE_KEYBOARD]`への静的な配線も毎回そのまま起きる。

`ble_dynamic true`が変えるのは**ただ1点**: 起動直後に`main_host.c`が自動で`ble_hid_device_start()`を呼ぶかどうか、それだけ。配線自体は`ble_dynamic`の設定と無関係に、スクリプトが読み込まれた瞬間から常に「活性化」済みになっている。

実際に起きていること:

- 配線(pipeline)は最初から常に有効 - キーボードレポートが来るたびに、`:ble_kbd`宛先への送信関数(`send_keyboard_to_sink()`等)は毎回律儀に呼ばれる。
- ただしBLEスタック自体(`s_started`)が起動していなければ、その送信関数は`ble_hid_device_connected()`(`s_started && s_connected`)を見て即return - 呼ばれてはいるが何もしない。
- `ble_toggle true`がやっているのは「配線を有効化する」ことではなく、**その配線の先にある無線スタック自体を立ち上げる**こと。立ち上がって接続もできれば`s_started && s_connected`がtrueになり、最初から存在していた配線が初めて実際にデータを流し始める。

これはBLEで初めて出てきた考え方ではなく、**`:typec`宛先も昔から同じ構造**だったことに気づいた: `to :typec_kbd`という配線自体は常に静的に存在していて、実際に送るかどうかは`usb_device_typec_connected()`(PCが挿さって認識されているか)という**別軸のランタイム状態**で毎回チェックされている。`ble_dynamic`/`ble_toggle`は、その「配線は静的、生死は動的」という既存の構造に、もう一段(無線自体の電源ON/OFF)を足しただけ - 静的な宣言モデルと動的なON/OFFは元々そういう役割分担になっている。

## 既知のトレードオフ: 呼び出し元をブロックする

`system_control`の20msパルス待ちと違い、`ble_toggle`はBTコントローラ/NimBLEホストの実際の起動・終了にかかる時間(おそらく数十〜数百ms程度、実測はまだ)だけ**呼び出し元をブロックする**。`ble_toggle`は典型的にmrubyのディスパッチパス内(`s_mrb_mutex`保持中)から呼ばれるので、その間**他の全パイプライン(マウス含む)も止まる**。頻繁に起きる操作ではなく、明示的なユーザー操作(ショートカット)なので許容できる設計判断とした - `system_control`のブロッキングも同じ理由で受け入れている前例に倣った。実機で体感どの程度の長さになるかは未検証。

## 未検証・今後の懸念

- **実機での動作確認は未実施**。特に「start → stop → start」の複数サイクルが本当に安定して繰り返せるか(BTコントローラの再初期化はESP32系では割とデリケートな領域として知られている)は、コードレベルの調査(`esp_hid_gap.c`が対称なinit/deinitペアを既に持っている、`esp_hidd_dev_deinit()`の中身を実装ソースまで読んだ)止まりで、実機での複数回切り替えはまだ試していない。
- `ble_store_config_init()`(NVSボンディング永続化の配線)を毎回の`start()`で再度呼んでいるが、二重登録的な副作用が無いかは未確認(ただの関数ポインタ登録なので理論上は冪等のはず)。
- stop中に切断されたPC側が持つ「まだペアリングされてるはず」という認識とのズレ(stopしてもボンディング自体はNVSに残るので、再度startすれば普通に再接続できる想定 - これも実機未確認)。

## フォローアップ: `ble_toggle`引数無し = トグル

初版は`ble_toggle(true/false)`の明示指定のみで、トグルさせたいスクリプト側は自前で状態(on/offのHash)を持って管理する必要があった。「引数無しでトグルできないか」という提案を受けて対応。

- `ble_hid_device.c`に`ble_hid_device_started()`(新規)を追加 - `ble_hid_device_connected()`(スタック起動中 **かつ** 接続中)と違い、**スタックが起動中かどうかだけ**を返す(`s_started`そのもの)。
- `ruby_ble_toggle()`(mruby_filter.c)が`mrb_get_argc(mrb) == 0`なら`!ble_hid_device_started()`で自動的に反転、引数があれば従来通りその値をそのまま使う。DSL登録も`MRB_ARGS_REQ(1)` → `MRB_ARGS_OPT(1)`に変更。

スクリプト側で状態をHashに持たせる旧方式より**正確**でもある: もし`ble_toggle true`が何らかの理由で失敗していても(`s_started`が実際にはfalseのまま)、スクリプト側の自己申告のフラグはtrueのまま食い違いうるが、`ble_hid_device_started()`を都度見て反転する新方式なら実際の状態と食い違わない。

`default.rb`の使用例もこれに合わせて簡略化(`ble_state = { enabled: false }`的なHash管理コードを削除、`ble_toggle`を素で呼ぶだけに)。

## フォローアップ: `ble_enable` → `ble_toggle`に改名

引数無し = トグルが主な使い方になったので、「`_enable`より`toggle`の方が名前として素直」というフィードバックで改名(動作は変えていない、引数ありなら明示set・引数無しならトグル、両方とも従来通り)。`ble_hid_device.c`/`.h`、`mruby_filter.c`/`.h`、`main_host.c`、`default.rb`の全参照箇所を揃えて置換。

## フォローアップ: 状態取得DSL(`ble_started?`/`ble_connected?`)

「set(`ble_toggle`)はあるのにget(状態取得)が無いのはチグハグ」という指摘を受けて追加。`ble_hid_device_started()`/`ble_hid_device_connected()`をそのまま返すだけの読み取り専用DSL:

- `ble_started?` - BLEスタック自体が起動中か(`s_started`)
- `ble_connected?` - 起動中 **かつ** 実際にペアリング済み機器と接続中か(`s_started && s_connected`)

`debug_print`と組み合わせて`ble_toggle`直後に今の状態をログする、といった使い方を想定(`default.rb`に例追加)。

## 参考

- `esp32-kvm-ip/main/ble_hid_device.c`の`ble_hid_device_start()`/`_stop()`/`_started()`/`nimble_host_task()`
- `esp32-kvm-ip/main/esp_hid_gap.c`の`esp_hid_gap_init()`/`_deinit()`(vendor元から既に対称なペアだった)
- `esp32-kvm-ip/components/esp_hid/src/nimble_hidd.c`(ESP-IDF本体、vendorしていない)の`nimble_hid_stop_gatts()`/`nimble_hidd_dev_deinit()`
- `esp32-kvm-ip/main/mruby_filter.c`の`ruby_ble_dynamic()`/`ruby_ble_toggle()`/`mruby_filter_ble_dynamic()`
- `esp32-kvm-ip/main/main_host.c`のBLE自動起動チェック
- `esp32-kvm-ip/main/mruby_scripts/default.rb`(トグル例)
- `mds/usb_hid/2026-09-10_mruby_debug_stream.md`(「小さい常駐プリミティブ+動的な本体」という同じ設計パターンの前例)
