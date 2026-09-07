# Host role: BLE HID出力 実装メモ

[2026-09-07_ble_hid_sink_plan.md](2026-09-07_ble_hid_sink_plan.md)の実装。ビルドは両ロールとも通った。実機検証は完了(下記「実機検証で見つかったバグと修正」参照) - 基本機能(ペアリング・再接続・キーボード/マウス/Consumer送信)は動作確認済み。既知の制限事項が1件残っている(マウスFPS低下、末尾参照)。

## 実装済み

- **`ble_hid_device.c`/`.h`**: BLE HID(HOGP)コンボデバイス。キーボード(Report ID 1)・マウス(Report ID 2)・Consumer Control(Report ID 3)を1本のReport Mapに統合(`esp_hid_common.c`のパーサが同一blob内の複数トップレベルApplication Collection+複数Report IDを正しく扱えることを実装前にソース確認済み)。レポートのバイト配置は`usb_descriptors.c`のReport Protocol形式と完全一致。APIは`usb_device_typec.h`と同じ形(`ble_hid_device_keyboard_report()`/`_mouse_report()`/`_consumer_report()`/`_connected()`)+ `_unpair()`。
- **`esp_hid_gap.c`/`.h`**: ESP-IDF公式`examples/bluetooth/esp_hid_device`から汎用GAP/ボンディング部分をほぼそのまま流用(NimBLE以外の分岐はKconfigでコンパイル対象外)。**変更点1箇所**: セキュリティ設定を元の`BLE_SM_IO_CAP_DISP_ONLY`+`sm_mitm=1`(パスキー表示前提、この基板には無理)から`BLE_SM_IO_CAP_NO_IO`+`sm_mitm=0`(Just Works)に変更。
- **mruby DSL**: `sink_kind_t`に`SINK_BLE`追加。`sink :name, :ble`で宣言(`:udp`と同じく宣言するだけで有効、専用トグルなし)。`send_keyboard/mouse/consumer_to_sink()`に分岐追加。`mruby_filter_ble_sink_declared()`で宣言有無をmain_host.cに伝える。
- **`main_host.c`**: `:ble`宣言があれば`ble_hid_device_start()`。失敗しても他は継続(non-fatal)。
- **`sdkconfig.defaults`**: `CONFIG_BT_ENABLED`/`CONFIG_BT_NIMBLE_ENABLED`/`CONFIG_BT_NIMBLE_HID_SERVICE`。HOST role以外(`main/CMakeLists.txt`のPRIV_REQUIRESに`bt`/`esp_hid`を足していない)では何もリンクされない(実測でDevice roleのFlash使用率はほぼ不変)。

## 未実装(プランのまま持ち越し)

- WebUIの「Unpair」ボタン(`ble_hid_device_unpair()`はもう用意済み、呼び出し口だけ未接続)
- ステータスLEDの新パターン・WiFi優先ロジック

## 実機検証で見つかったバグと修正

ビルドが通った後、実機で4件の不具合が見つかった。いずれも「BLEを実際に繋いで動かして初めて顕在化する」類のもので、ビルド検証だけでは踏めなかった。

### A. 再接続(リセット後)が必ず失敗する - 暗号化エラーのループ

**症状**: 初回ペアリングは必ず成功するが、リセット後の再接続は`encryption change event; status=7` → `disconnect; reason=517`(HCI 0x05 Authentication Failure)を繰り返すだけで確立しない。

**調査の迷走**: 最初に疑ったのは (1) NVSパーティションサイズ不足/破損 - `nvs`を24K→48Kに拡張、`phy_init`/`factory`のオフセットも調整して`erase-flash`+再フラッシュで検証 → **効果なし**。次に (2) LE Secure Connections(`sm_sc=1`)がLTKの再開時ストレージ周りで問題を起こしている説 - `sm_sc=0`(Legacy Pairing)に変更して検証 → **効果なし**。

**本当の原因**: `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG`/`CONFIG_LOG_DEFAULT_LEVEL_DEBUG`/`CONFIG_BT_NIMBLE_LOG_LEVEL_DEBUG`を一時的に全部有効にしてNimBLEの内部DEBUGログを取得したところ(3つとも必要 - `LOG_MAXIMUM_LEVEL`は「コンパイル時に含めるか」、`LOG_DEFAULT_LEVEL`は「実行時に実際に出すか」の別軸で、`BT_NIMBLE_LOG_LEVEL`のKconfigヘルプ自身が「NimBLEのログ詳細度はDefault log verbosityを超えられない」と明記している通り、3つ揃わないと1行も増えなかった)、失敗の瞬間の直前に

```
D NimBLE: looking up our sec;
D NimBLE: peer_addr_type=0 peer_addr=...
D NimBLE: ble_hs_hci_cmd_send: ogf=0x08 ocf=0x001b len=2   ← LE Long Term Key Request **Negative** Reply
```

が出ていた。PC側が前回ペアリング時のLTKで暗号化再開しようとし、ESP32(周辺機器側)にLTKを問い合わせたが、ESP32側がその鍵を持っておらず「無い」と返答していた。ログ全体を検索しても`nvs_open_from_partition nimble_bond`(NimBLEのボンドストアが実際にNVSを開いた形跡)が一度も出てこない。原因は`CONFIG_BT_NIMBLE_NVS_PERSIST`(デフォルト`n`)。このオプションが無効だと、NimBLEの`store/config`ボンドストアは**RAM上にしか**ボンド情報を保持しない(Kconfigヘルプそのまま: "Enable this flag to make bonding persistent across device reboots")。だからリセット直後の初回接続から即座に失敗していた - NVSサイズもsm_scも最初から無関係で、そもそも一度もNVSに書き込まれていなかった。

**修正**: `CONFIG_BT_NIMBLE_NVS_PERSIST=y`を追加。`sm_sc`は`1`(Secure Connections、より安全なデフォルト)に戻した。DEBUGログ設定3つは撤去。nvs拡張(48K)とパーティションオフセット調整は無害な改善なのでそのまま残した。

**注意点**: この修正を入れる前にペアリングされていたボンドは、そもそも一度もESP32側のNVSに書かれていなかったもの。PC側の古いペアリング情報は「削除→再ペアリング」しないと同じ失敗が再現する。

### B. WebUIの「保存」でボードが再起動する

**症状**: WebUIからスクリプトを保存(`mruby save`相当)すると`abort()`でボードが再起動することがあった。

**原因**: `mruby_filter_check_syntax()`(保存前の構文チェック用、使い捨ての`mrb_state`でパース)が呼ぶ`mrb_parse_nstring()`は、この版のmruby(Prismパーサ)ではAST構築だけでなくcodegenまで実際に走る。スクリプトが大きく/複雑になり、このチェック専用ステートの内部SRAMヒープを使い切ると`NoMemoryError`を投げようとするが、この呼び出しが保護されたコンテキスト(`mrb->jmp`が設定された状態)の外で行われていたため、mruby自身の`exc_throw()`が「`mrb->jmp`が無い→`abort()`」経路に落ちてボードごと再起動していた。

**修正**: `mruby_filter_check_syntax()`のパース呼び出しを、mruby自身の`mrb_core_init_protect()`(`error.c`)と同じ`MRB_TRY`/`MRB_CATCH`パターンで囲んだ。これでパース中の例外(メモリ不足含む)は正常にlongjmpで戻ってきて「構文エラー(メモリ不足?)」を返すだけになり、クラッシュしなくなった。

### C. 通常操作中(マウスを動かしている最中)にもボードが再起動する

**症状**: Bより深刻 - スクリプト保存時だけでなく、**マウスを動かしている最中**にも同じ`abort()`が発生した。

**原因**: Bと同じ「保護されていない例外」クラスのバグだが、今度はメインの長寿命mruby VM(`s_mrb`)側、しかも**マウス/キーボード/Consumerレポートの度に通るホットパス**(`dispatch_keyboard_via`/`dispatch_mouse_via`/`dispatch_consumer_via`)で発生していた。各関数内の`build_mouse_event()`等(イベントHashを生成する`mrb_hash_new_capa()`呼び出し)は、ユーザースクリプトのブロック呼び出し(`invoke_block()`が内部で使う`mrb_funcall_argv()`は「保護されたトップレベル呼び出し」なので元々安全)より**前**に、保護されないまま実行されていた。BLE/NimBLEがランタイムで内部SRAMを消費するようになった分、この経路のメモリ不足が実際に踏まれるようになった、という側面もありそう。

**修正**: `dispatch_keyboard_via`/`dispatch_mouse_via`/`dispatch_consumer_via`の3関数それぞれの本体全体を、Bと同じ`MRB_TRY`/`MRB_CATCH`パターンで囲んだ。例外が起きてもそのレポート(の残りステージ)だけ捨てて処理を続行する。

### D. Type-Cが実際にPCへ列挙されていないとBLE(も含め:udp等)が一切送信されない

**症状**: ESP32をPCにType-C接続してBLEが動く一方、電源のみ(列挙なし)だとBLEに何も送られない。

**原因**: `hid_forwarder.c`のキーボード/マウス/Consumerの3つの報告経路すべてが、`usb_device_typec_connected()`(実際にPCへ列挙された状態)が真の時**だけ**mrubyのdispatch関数(`:typec`/`:udp`/`:ble`全シンクの入口)を呼ぶ作りになっていた。mruby以前・BLE以前(Type-Cが唯一の出力先だった頃)の設計の名残りで、mruby導入後・BLE追加後もこの外側のゲートだけ更新されていなかった。`usb_device_typec_*_report()`自体は未接続時に即座に戻る安全なno-opなので、このゲート自体が不要だった。

**修正**: mruby有効時(`mruby_filter_active()`)は、Type-C接続状態に関係なく常にdispatch関数を呼ぶよう変更。C版フォールバック(`filter_rules.h`/`route_rules.h`、mruby未使用時)側の挙動は変えていない。

## その他の調整・未解決事項

- **`ESP_HIDD_PROTOCOL_MODE_EVENT`診断ログが出ない**: 調査の結果、esp_hidのNimBLEバックエンド(`nimble_hidd.c`)はこのイベントを一度も発行しない実装だと判明(Bluedroidバックエンドの`ble_hidd.c`/`bt_hidd.c`だけがpostする)。ESP-IDF側の欠落で、こちら側の設定漏れではない。副次的に、`nimble_hidd.c`は接続確立の度にProtocol Mode属性を明示的にREPORTへリセットしていることも確認できたので、「Boot/Reportモードの取り違えでマウスレポートが誤ったキャラクタリスティックに配送される」という当初の仮説は優先度を下げた(構造上BootモードとReportモードの両方のキャラクタリスティックが存在しPCが両方subscribeしてくることは実際に確認できたが、実害があるかは未確認のまま)。
- **NimBLE自身のログ(`notify_tx`/`GATT procedure`/`att_`系、"NimBLE"タグ)が出続ける**: レポート送信の度に出る`notify_tx`はコンソールを埋めるだけでなく、このプロジェクトで過去に実測済みの「UARTブロッキング出力がホットパスの遅延要因になる」現象([2026-08-24_rp2040_bridge_fps_investigation.md](2026-08-24_rp2040_bridge_fps_investigation.md))と同じ構図になっている疑いがある。
  - まず`CONFIG_BT_NIMBLE_LOG_LEVEL_WARNING`を試したが効果なし - 調査の結果、`MODLOG_DFLT()`(`modlog.h`)の実際のフィルタはKconfigのこの値ではなく、esp_logの**実行時**タグ別レベル(`esp_log_level_set()`、デフォルトは`CONFIG_LOG_DEFAULT_LEVEL`)で決まっていることが判明。
  - `esp_hid_gap_init()`冒頭で`esp_log_level_set("NimBLE", ESP_LOG_WARN)`を直接呼ぶよう変更 → 実機確認済み、**抑制できた**(直後の1回のテストでは変化が見えなかったが、再フラッシュ後の再検証で確認)。両方のKconfig変更(`CONFIG_BT_NIMBLE_LOG_LEVEL_WARNING`と併用)を残している。
- **BLEルーティング時のマウスFPS低下**(実機確認済み、未解決): BLE経由でマウスを送ると、Type-C/UDP経由と比べて明らかに動きが粗くなる。上記のNimBLEログ垂れ流しは止まったので、これは別要因 - BLEの接続インターバル自体(Type-Cの数百Hzに対しBLEは規定上もっと粗い)による本質的な制約の可能性が高いが未検証。現状「動く」ところまでは確認できたが、レイテンシ/FPSの詰めは持ち越し。

## ビルドで踏んだハマりどころ

普段よりだいぶ深いリンカの話が続いた。

### 1. 生成済み`sdkconfig`が古い設定を握ったまま

`sdkconfig`(gitignore対象、生成物)に以前ビルドした際の`# CONFIG_BT_ENABLED is not set`が確定済み行として残っており、`sdkconfig.defaults`に新しい`CONFIG_BT_ENABLED=y`を足しても反映されなかった(Kconfigのマージルール: 既に明示的に決定済みのオプションは、たとえ無効化の記録でも新しいdefaultsで上書きされない)。`rm sdkconfig`して再生成することで解決。

### 2. `esp_hid`のNimBLE HIDバックエンドがまるごとコンパイル対象外

`esp_hidd_dev_init()`→`esp_ble_hidd_dev_init()`が未定義エラー。`components/esp_hid/src/nimble_hidd.c`の中身全体が`#if CONFIG_BT_NIMBLE_HID_SERVICE`(デフォルト`n`)で囲われていた。`sdkconfig.defaults`に`CONFIG_BT_NIMBLE_HID_SERVICE=y`を追加して解決。ついでにデフォルトの`CONFIG_BT_NIMBLE_SVC_HID_MAX_RPTS=3`がちょうどこちらの3レポートIDと一致していたので変更不要だった。

### 3. `esp_hid_gap.c`側の未定義関数

ベンダリング元の`esp_hid_device_main.c`が提供していた`ble_hid_task_start_up()`(暗号化確立イベントで呼ばれる、デモ用のstdin読み取りタスク起動フック)を移植していなかったので未定義参照に。今回はレポートを直接プッシュする設計でそのタスク機構自体が不要なので、`ble_hid_device.c`側にno-opスタブとして定義するだけで解決。

### 4. `libmruby.a`がリンクコマンドの最後尾に来る構成と`__getreent`

`bt`/`esp_hid`を`main`のリンク対象に足しただけで、mruby(Prismパーサ)が使う`__getreent`が未解決になった。`libmruby.a`は以前から最終リンクコマンドの一番最後に来る構成で、esp_libcの`__getreent`定義はそれより手前のどこかで別の`-u xxx_include_impl`系フラグ経由で偶発的に取り込まれていたらしく、`bt`/`esp_hid`を足したことでその偶発的な取り込みが崩れた(正確な原因はリンカの内部処理順まで追いきれず未解明)。`target_link_libraries(... INTERFACE "-Wl,-u,__getreent")`で明示的に強制解決。

### 5. `_ctype_`(本命、一番深かった)

mruby-sprintfが`isspace()`/`isdigit()`等を呼んでおり、mrubyは独自のRakefileベースのクロスコンパイルでビルドされるため、このツールチェーンのctype.hがclassic newlib形式の`_ctype_[c+1]`テーブル参照に展開されていた。ところがこのxtensa-esp-elfツールチェーン(実体はpicolibc)には`_ctype_`という名前のシンボルがどこにも存在しない(picolibcは`_ctype_b`という別名、コード中の`_ctype_b`は無関係の既存参照)。

ダミーテーブル(全ゼロ、このプロジェクトのどのスクリプトも`Kernel#sprintf`のctype依存機能を使わないので実害なし)を用意しても、それだけでは直らなかった:

- `libmruby.a`がリンクコマンドの最後尾にあるため、`sprintf.o`が`_ctype_`を要求するタイミングは全アーカイブのスキャンが終わった後
- リンカの単一方向スキャンでは、それより前にある`libmain.a`(ダミーテーブルの置き場所)をもう一度見に行くことはない
- `--whole-archive`で`libmain.a`を強制フル取り込みしても、取り込まれる**タイミング自体**が`_ctype_`が要求されるより前なので、要求が後から出ても後の祭り
- `-Wl,--start-group ... --end-group`で**リンクコマンド全体**を囲むと解決することを実験で確認(強制的に複数パス走査させることで解決) - ただしESP-IDFが自動生成する巨大なリンクコマンドをこの形にフックする簡単な方法がなく、採用せず

最終的な解決策: `mruby_ctype_shim.c`に`_ctype_`ダミーテーブルと一緒に`mruby_ctype_shim_touch()`という空関数を用意し、`mruby_filter_init()`(常に早期に呼ばれる)から実際に呼び出す形にした。これで「リンカのフラグによる強制」ではなく「実コードからの実参照」によって`libmain.a`内のこのオブジェクトが**早い段階の通常スキャンで**取り込まれるようになり、`_ctype_`がグローバルシンボル表に載った状態で後から来る`sprintf.o`の要求を満たせるようになった。

## リソース実測(確定)

- Host role: Flash 25%空き(BLE追加前は31%空き) - 想定していた約185KBの増加と符合
- Device role: 74%空き(ほぼ不変) - BT関連コンポーネントがリンクされていないことの確認になる

## 参考

- `esp32-kvm-ip/main/ble_hid_device.c`/`.h`
- `esp32-kvm-ip/main/esp_hid_gap.c`/`.h`(ベンダリング元: ESP-IDF `examples/bluetooth/esp_hid_device`)
- `esp32-kvm-ip/main/mruby_ctype_shim.c`
- `esp32-kvm-ip/main/CMakeLists.txt`のHOST role側コメント(`__getreent`/`_ctype_`の経緯を記載)
- [2026-09-07_ble_hid_sink_plan.md](2026-09-07_ble_hid_sink_plan.md) - 設計・決定事項
