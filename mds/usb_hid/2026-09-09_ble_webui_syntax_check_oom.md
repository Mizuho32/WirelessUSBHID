# BLE有効時、WebUIの「保存」がOOMで失敗する問題の調査

[2026-09-07_ble_hid_sink_impl.md](2026-09-07_ble_hid_sink_impl.md)のBLE HID出力実装後、実機で見つかった追加の不具合の調査記録。マウスFPS改善(WiFi/BT無線共存)とは別系統の問題。

## 症状

`sink :ble_mouse, :ble`等でBLEが有効な状態で、WebUI(`mruby_webui.c`)の「Save & reboot」を押すと、`mruby_filter_check_syntax()`(保存前のスクリプト構文チェック - 独立した`mrb_open_core()`のVMでPrismパース+codegenだけ行う、実行はしない)が

```
parse failed (out of memory?)
```

で失敗することがある(常にではない)。

## 調査の経緯

### 1. 最初の仮説: `ble_wifi_off_while_connected`のWiFi停止/再開サイクル

[2026-09-07_ble_hid_sink_impl.md](2026-09-07_ble_hid_sink_impl.md)で実装したこの機能は、BLE接続中`esp_wifi_stop()`、切断で`esp_wifi_start()`し直す。この停止/再開サイクルが内部SRAMを断片化させているのでは、という仮説を立てた。

実機ログ(`heap_caps_get_free_size`/`heap_caps_get_largest_free_block`、`MALLOC_CAP_INTERNAL`):

| 状況 | internal free | largest block |
|---|---|---|
| BLE無効スクリプト、RST直後 | 121776 | **61440** |
| BLE有効、切断→WiFi resumeから間もなく | 8382696(※後述の理由で無視すべき数値) | **12**〜**1024** |
| BLE有効、切断→WiFi resumeから少し待って | 30748〜30064 | **16384** |

→ 待てば`largest block`はある程度回復するので「再接続直後の一時的な谷」は確かに存在する。しかし待っても61440には戻らない。この仮説だけでは「待っても本調子に戻らない」を説明できなかった。

(このとき使っていた`esp_get_free_heap_size()`の数値、例えば`8382696`は実は**PSRAM込みの合計**で内部SRAMの診断には無意味だったと後で判明 - `esp_get_free_heap_size()`は`heap_caps_get_free_size(MALLOC_CAP_DEFAULT)`で、このボードのPSRAM領域は`MALLOC_CAP_SPIRAM | MALLOC_CAP_DEFAULT`両方のcapabilityで登録されている。`SPIRAM_USE_CAPS_ALLOC`が効くのは`malloc()`自体の実装側 - `heap_caps_malloc_default()`の`malloc_alwaysinternal_limit`がデフォルト`-1`(=常にINTERNAL限定)であって、PSRAM領域自体が`MALLOC_CAP_DEFAULT`タグを持たないわけではない。以後は`heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`系の値だけを見る。)

### 2. 真犯人の切り分け: BLEが常駐しているだけで発生

「BLE有効・PCと一度もBLE接続させない(=`ble_wifi_off_while_connected`も一度も発動しない)」状態で計測:

| 状況 | largest block |
|---|---|
| BLE無効 | 61440 |
| BLE有効・PCと未接続 | **17408** |

WiFi停止/再開サイクルは無関係で、`ble_hid_device_start()`が走った時点(PCと繋がってすらいなくても)で60KB→17KBまで落ちることが判明。[2026-09-07_ble_hid_sink_impl.md](2026-09-07_ble_hid_sink_impl.md)の静的フットプリント見積もり(約185KB Flash/22KB RAM)は「合計消費量」の見積もりであり、「実行時の最大連続空きブロック」がここまで小さくなることは分かっていなかった。

### 3. NimBLEのKconfig絞り込み(実施したが効果なし)

この時点で「NimBLEの常駐設定が大きすぎるのでは」という仮説で、実際に使っていない機能を`sdkconfig.defaults`で削った(`esp32-kvm-ip`側コミット参照):

- `BT_NIMBLE_ROLE_CENTRAL`/`BROADCASTER`/`OBSERVER`を無効化(このデバイスはPeripheral専用)
- `BT_NIMBLE_MAX_CONNECTIONS` 3→1(同時接続は常に1台)
- `BT_NIMBLE_ATT_PREFERRED_MTU` 256→23(HIDレポートは最大8バイト)
- `BT_NIMBLE_ATT_MAX_PREP_ENTRIES` 64→1(Prepare Write未使用)
- 未使用の例GATTサービス9個を無効化(Proximity/ANS/CTS/HTP/IPSS/TPS/IAS/LLS/HR)
  - `BT_NIMBLE_SPS_SERVICE`も最初は削ったが、`esp_hid`のNimBLEバックエンド(`components/esp_hid/src/nimble_hidd.c`)が`ble_svc_sps_init()`を直接呼んでいてリンクエラー(undefined reference)。GAP/BAS/DIS/SPS+HIDの5つは同ファイルが直接依存しているため残した。

実機再検証:

| 状況 | internal free | largest block |
|---|---|---|
| Kconfig絞り込み前、BLE有効・未接続 | 42904 | 17408 |
| Kconfig絞り込み後、BLE有効・未接続 | 44228 | **17408(変化なし)** |

**totalの空きはわずかに増えた(+1.3KB)が、largest_free_blockは1バイトも変わらなかった。** → NimBLE自体の常駐設定を削る方向は的外れと判明。17408という値はNimBLEの設定変更と無関係な何か(他のヒープ領域の構造)で決まっている。

### 4. `heap_caps_print_heap_info()`によるリージョン別ダンプで全体像が判明

`mruby_filter_check_syntax()`の失敗時ログを、単一の`largest_free_block`スカラー値から`heap_caps_print_heap_info(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`(登録されている内部ヒープ領域ごとに`free`/`largest_free_block`等を個別出力)に変更して再現:

```
Heap summary for capabilities 0x00000804:
  At 0x600fe000 len 8168   free 172 allocated 7244   largest_free_block 104
  At 0x3fce9710 len 22308  free 4   allocated 20752  largest_free_block 0
  At 0x3fcf0000 len 32768  free 20  allocated 31708  largest_free_block 12
  At 0x3fcaee80 len 239760 free 4   allocated 234060 largest_free_block 0
  Totals:
    free 200 allocated 293764 min_free 184 largest_free_block 104
```

**4リージョン合計で空きたった200バイト、largest_free_blockは104バイト。** 割り当て済み合計293764バイト(≒287KB)。つまり: BLE常駐分の「17KB天井」がどうこうという話ではなく、**このスクリプト(2415バイト)1回のパース試行だけで、その時点で空いていたはずの全て(数十KB)を使い切ってしまっている**。

### 結論

- NimBLE自体の常駐フットプリントを削る方向(手順3)は効果が薄いと確認済み。
- 真因は「mrubyのPrismパーサ+codegenが、このサイズのスクリプトに対して実際に必要とするピークメモリ量が、BLE常駐後に残っている内部SRAMよりずっと大きい」こと。GCが「取れるだけ取ろうとする」性質もあり、空きがある分だけ食いつぶしてから力尽きる(≒空き容量に応じて必要量が伸縮するように見える)。
- 起動時のスクリプト読み込み(`mruby_filter_init()`、同じくPrismパース)が普段失敗しないのは、BLE/NimBLEがまだ何も確保していない・一番空きが多いタイミングで走るから。パース自体の重さは変わらないはず。
- `mruby_filter_check_syntax()`用の使い捨てVMは`mrb_open_core()`→`mrb_basic_alloc_func()`(素の`malloc()`/`realloc()`)経由で内部SRAM限定(`SPIRAM_USE_CAPS_ALLOC`下では`malloc()`はPSRAMを一切使わない - 本プロジェクトの意図的な設定、[2026-08-30_mruby_phase2_webui.md](2026-08-30_mruby_phase2_webui.md)参照)。一方PSRAMは常時8MBほぼ丸ごと空いている。

## 対策: syntax-check用VMだけPSRAMへ

実装済み(`esp32-kvm-ip`コミット`ba803f9`)。

- `main/mruby_alloc_psram.c`/`.h`(新規): `mruby_alloc_prefer_psram(bool)`というグローバルなトグルと、それが有効な間だけ`heap_caps_realloc(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`で確保する`mrb_basic_alloc_func()`の差し替え実装。無効な間は従来通り素の`realloc()`(内部SRAM限定)にフォールバックするので、このトグルを使わない限り完全に無害。`free()`側は差し替え不要(ESP-IDFの`free()`は`heap_caps_free()`に解決され、確保元のヒープ領域をポインタから自動判別するので、PSRAM由来のポインタも素の`free()`で正しく解放できる)。
- `mrb_basic_alloc_func()`はmruby本体が「アプリ側での再定義」を公式にサポートしている拡張点(`components/mruby/mruby/src/allocf.c`のコメント参照)だが、mruby自身の(Rakefile駆動の)ビルドが`src/allocf.c`内の同名シンボルを既に`libmruby.a`に入れてしまうので、そのままでは重複定義でリンクエラーになる。`components/mruby/mruby`は素のupstream submoduleとして保つ方針(既存コメント通り)なので、submodule自体は書き換えず、`components/mruby/CMakeLists.txt`に`ar d libmruby.a allocf.o`でそのオブジェクトだけ取り除く`mruby_strip_allocf`カスタムターゲットを追加(既存の`mruby_build`と同じく「安全に毎回実行できる」設計 - 既に無いメンバーを`ar d`しても無害なexit 0であることを確認済み)。
- `mruby_filter.c`の`mruby_filter_check_syntax()`は、このVM(`mrb_open_core()`〜`mrb_close()`)の生存期間だけ`mruby_alloc_prefer_psram(true)`/`(false)`で囲む。グローバル関数1本しかないため、本番の`s_mrb`が別タスクでたまたま同じ瞬間にGCしていたらそちらも巻き込まれてPSRAM行きになるが、実害はなく一瞬遅くなるだけ。

実機ビルド確認: 両ロールとも変化なし(Host 25%空き、Device 74%空き)。`components/mruby/mruby/build`を丸ごと消してのクリーンビルドでも同じ手順で成功し、リンク後の`.map`ファイルで`mrb_basic_alloc_func`が`main/mruby_alloc_psram.c.obj`由来に切り替わっていることを確認済み。実機での最終確認(BLE有効・save)は今後行う。

## 保守用に残した診断コード

- `main/mruby_filter.c`: `mruby_filter_check_syntax()`の両失敗分岐(`mrb_open_core()`失敗/parse・codegen失敗)で`heap_caps_print_heap_info(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`を呼ぶ。失敗時のみ発火するログなので普段のノイズにはならない。
- `main/wifi_manager.c`の`IP_EVENT_STA_GOT_IP`ログ、`main/ble_hid_device.c`の`wifi_manager_resume()`成功ログに、internal free/largest blockの数値を追記(既存ログ行への追記のみ、新規ログ行の追加ではない)。

## 参考

- `esp32-kvm-ip/main/mruby_filter.c`の`mruby_filter_check_syntax()`
- `esp32-kvm-ip/main/ble_hid_device.c`/`esp32-kvm-ip/main/wifi_manager.c`(ヒープログ追記箇所)
- `esp32-kvm-ip/sdkconfig.defaults`(NimBLE Kconfig絞り込み - 効果は薄いが実害もないため維持)
- `esp32-kvm-ip/components/mruby/mruby/src/allocf.c`(`mrb_basic_alloc_func()`のアプリ側再定義サポート)
- `esp32-kvm-ip/main/mruby_webui.c`の`webui_alloc()`(PSRAM明示利用の既存実績)
