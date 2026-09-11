# BLE有効化で動かなくなる問題の正体: `MRB_DSL_MAX_SINKS`(8)を使い切っていた

## 症状

「`sink :ble_cc, :ble, kind: :consumer`を有効にすると動かなくなる」という報告 → 詳細化: BLEのsinkを2個までは大丈夫、3個目でダメになる。しかもtype-cも道連れで動かなくなる。`ble_dynamic true`(BLEスタック自体は起動すらしない設定)でも同じ症状。

## 原因: BLEでもNimBLEでもなく、mrubyのDSL側の固定サイズ制限

実機の起動ログ(ユーザー提供のtmp.log/tmp2.log、両方とも同一)に、WiFi/BLEより前、スクリプト読み込み直後の時点でこの行があった:

```
I (1621) MRBFILT: script: "sink: too many sinks declared\n-e:43:in sink\n-e:43"
```

`mruby_filter.c`の`#define MRB_DSL_MAX_SINKS 8` - `sink()`呼び出しは最大8個までしか登録できない固定サイズ配列(`sink_def_t s_sinks[MRB_DSL_MAX_SINKS]`)。スクリプト43行目でこの上限に達し、`sink: too many sinks declared`が`mrb_raise()`された。

`ble_dynamic`の有無に関係なく同じ行が出ていた("BLEのログも出るけど…"の"BLEのログ"はこの後起きるBLEスタック起動自体は成功している別の話 - 起動ログ自体にE/Wは無い、という観察は正しい)ことから、**これはBLEスタックの起動/実行時の問題ではなく、スクリプト読み込み(`mrb_load_nstring()`)時点、WiFiすら始まる前の段階で既に起きているDSLパース時のエラー**だと判明した。

## なぜtype-cまで巻き添えになるか

このエラーメッセージ自体が`debug_print()`経由(`ESP_LOGI(TAG, "script: %s", ...)`のログ形式と一致)で出力されている = **スクリプト側に`rescue`があり、例外を握りつぶして`debug_print`でログしてから続行している**とみられる。Rubyの`rescue`は例外発生地点から一番近い`begin`まで巻き戻るため、43行目の`sink`呼び出し失敗**以降**、同じ`begin`ブロック内に書かれていたtype-c関連の`sink`/`pipeline`設定が**一度も実行されないまま**スキップされていた可能性が高い。BLEを2個から3個に増やしたことで、たまたま「あと1個」の余裕を使い切り、その次(スクリプトの記述順で後にある)type-c関連の設定が巻き添えで消えた、という筋書き。

## 対応

`MRB_DSL_MAX_SINKS`を8→16に倍増(`mruby_filter.c`)。type-c(3) + BLE(3) + system_control(2) = 8だけで既に余裕ゼロだったので、UDPリレー等の他用途を含めると簡単に超える。`s_sinks[]`/`s_sources[]`両方がこの定数でサイズ決定されるが、要素はどちらも小さい構造体なので余裕を持たせるコストは無視できる。

## 教訓

- **この種の`sink`/`pipeline`登録エラーは、スクリプトが自前で`rescue`していると`debug_print`を有効にしていない限り気づきようがない** - 今回は`debug_print`(UART出力)がデフォルトONだったおかげでログに残っていた。
- 症状が「機能A(BLE)を有効にすると無関係に見える機能B(type-c)まで壊れる」という形の場合、両者に共通する何か(この場合は固定サイズ配列の上限)を疑うのが筋が良い。BLEスタック自体のログにE/Wが無いことを確認済みなら、なおさら「BLEの外側」を疑うべきだった。

## 参考

- `esp32-kvm-ip/main/mruby_filter.c`の`MRB_DSL_MAX_SINKS`/`dsl_sink()`/`dsl_source()`
- ユーザー提供の実機起動ログ(tmp.log: `ble_dynamic true`時、tmp2.log: 同無し・BLE接続まで到達)
