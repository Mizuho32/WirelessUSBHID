# クラッシュ安全網: coredump-to-flash + NVS永続化 + WebUI/ntfy通知 + on-demandヒープトレース

[[ble_idle_crash]]の調査中、「組み込みはこの手のクラッシュ調査が厄介 - もっと一般的な保険をかけたい」という話になり、`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`/`CONFIG_HEAP_TRACING`/パニックハンドラフックの3点セットを軸に実装した。Host role限定(esp32-kvm-ip、KVM_ROLE=HOST)。Device role(main.c)にはWebUIが無いため今回は対象外。

## 全体設計

**「パニックハンドラをフックする」のは避けた** - パニック中に任意コードを実行するのはスタック破壊/割込み無効/flash書き込み不整合などのリスクがあり、ESP-IDFも推奨していない。代わりに実務での定石通り:

1. **`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`**(sdkconfig.defaults) - パニック/ウォッチドッグ時、ESP-IDFが自動でレジスタ・バックトレース・全タスクのスタックを`coredump`パーティション(partitions.csv、新規追加、128K)にELF形式で書き込む。
2. **次回起動時**(`crash_report_init()`, `main/crash_report.c`) - `esp_reset_reason()`が`PANIC`/`*_WDT`/`BROWNOUT`/`CPU_LOCKUP`の時だけ、`esp_core_dump_get_panic_reason()`/`get_summary()`で人間可読な要約(理由・タスク名・PC)を取り出し、**NVS(namespace "crash")に保存してからcoredumpパーティション自体は消す**。
3. これにより「coredumpパーティションの生データ」ではなく「NVSの要約」が恒久的な記録になる - ユーザーの懸念(「意図せず2回RSTしたら消えるとかないよね?」)への直接の回答: 2回目以降の**通常の**再起動は`is_crash_like()`がfalseを返すので、NVSの記録には一切触らない。次のクラッシュが起きて初めて上書きされる。
4. **WebUI**(`mruby_webui.c`)の`/api/status`に`last_crash: ...`行を追加、`POST /api/crash_clear`で手動クリア可(WebUIに「Clear last crash」ボタンも追加)。
5. **ntfy.sh等への通知**(オプトイン) - スクリプトが`crash_notify_url "https://ntfy.sh/..."`を呼んでいれば、WiFi接続確立後に一度だけ`esp_http_client`でPOST。呼んでなければWebUI/NVSの記録だけで完結(通知なし)。

## on-demandヒープトレース

`CONFIG_HEAP_TRACING_STANDALONE`を有効化(常時ONではなく、実際に`heap_trace_start`/`heap_trace_dump`が呼ばれるまでコストなし - ただしKconfig自体をONにするだけで多少のIRAMサイズ増+malloc/free毎の軽微なオーバーヘッドは常にかかる、とESP-IDF自身のヘルプテキストに明記あり)。`mruby_filter.c`のDSLコマンド2つ(`crash_report.c`実装):

- `heap_trace_start` - `HEAP_TRACE_LEAKS`モードでトレース開始(解放されたら記録からも消えるので、確保されっぱなしのものだけが残る)。トレースバッファ(400レコード)はPSRAM確保 - 内部RAMを圧迫しない。
- `heap_trace_dump` - トレース停止+各レコード(サイズ・確保元コールスタック)をESP_LOGで出力。

[[ble_idle_crash]]のuuid16リークのような「1サイクルで数バイト漏れる」系のバグは、これがあれば数時間のfree byte監視ではなく一発で確保元のアドレス(addr2line可能)まで特定できたはず。

## 実機投入時の重要な注意 - パーティション表はWiFi OTAでは反映されない

`bin/upload_firmware.py`(WebUIの「Update firmware」と同じ`POST /api/firmware`)は**アプリイメージ本体だけ**を空いてるOTAスロットに書き込む - パーティション表(オフセット0x8000)には一切触れない。今回`coredump`パーティションを新規追加したが、**WiFi OTAで流しただけでは実機のパーティション表は古いまま**で、`coredump`パーティションは存在しない。

実害は無い(`esp_partition_find()`が見つからないだけで、espcoredump側は書き込みをスキップするだけ - クラッシュや起動失敗にはならない。ota_0/ota_1/nvs等の既存パーティションはオフセット不変なので無関係)が、**coredump機能自体は今のところ空振り**(何も記録されない)。WebUI/ntfy通知の配線自体は生きているので、次に実際のクラッシュが起きても`crash_report_init()`は素通りする(`esp_core_dump_get_summary()`等が単にESP_ERR_NOT_FOUND相当を返すだけ)。

**フル反映にはシリアル(またはパーティション表を含む一括書き込み)が必要**: `./bin/build_host.sh flash -p /dev/ttyUSB0`のような、bootloader/partition-table/appを一括で書くフローでないとパーティション表は更新されない。次に実機にケーブルで繋ぐ機会に反映すること。

## 追記1: いくつか質問への回答と、その場で直したもの

- **「coredumpパーティションは揮発性ってこと?」** → いや違う、flashパーティションなので不揮発。設計上「1回読んだら消す」運用にしてるだけで、媒体としては他のパーティション(NVS/OTA等)と同じ。恒久的な記録場所として**あえて使わない**ことにして、要約をNVSに移してるのがこの実装の肝。
- **「ntfyをスクリプトで呼べば...ってのはPC側で?」** → いや、ESP32自身(ボード上で動いてるmrubyスクリプト)。`crash_notify_url "https://ntfy.sh/..."`をスクリプトに1行書いておけば、そのボード自身がWiFi接続後に自分でPOSTする。PC側の設定は不要。
- **「解決したcrashの通知をされるとうざい」** → 対応した。クラッシュ検知時、直前にNVSへ保存済みの要約(理由・タスク名・PC値)と**完全一致**するかを比較し:
  - 一致(=同じバグでのクラッシュループ) → 通知は送らず、`count`をインクリメントするだけ。WebUIには`last_crash: ... [recurred Nx since last cleared]`と表示される。
  - 不一致(=新規/別のクラッシュ、または前回クリア済み) → 通常通り通知。
  - **「id割り当てて解決マーク」の実現方法**: 別途IDを新設せず、既存の「Clear last crash」ボタン(`crash_report_clear()`)がそのまま「これは解決した」のマークを兼ねる設計にした - クリアすると次回(たとえ同じバグが再発しても)「新規」扱いに戻り、また1回だけ通知される。シグネチャ自体は「理由+タスク名+実行アドレス」の完全一致判定(ESP32は静的リンクなので同じバグは毎回同じアドレスで落ちる、ASLR等は無い)。

## 追記2: `restart`/`simulate_crash` DSLコマンド

- **`restart`** - 素の`esp_restart()`。`ESP_RST_SW`として記録され、`is_crash_like()`はこれをクラッシュ扱いしない(意図的な再起動なので当然)。
- **`simulate_crash`** - この機能一式(coredump→NVS要約→WebUI/ntfy通知)を実際に動かして確認するための、意図的な`abort()`。null pointer参照などの「本物の異常」ではなく`abort()`を選んだのは、100%確実に再現できる制御されたクラッシュにしたかったから(ESP-IDFのnewlib `abort()`は本物のパニックハンドラ/coredump経路に乗る - 次回起動時`ESP_RST_PANIC`として現れ、本物のクラッシュと全く同じ経路で処理される)。

## 追記3: coredumpは消さない、通知は毎回する(誤解の修正)

以下2点、指摘を受けて修正した:

- **「coredump消さなくて良くない?」** → その通りだった。ESP-IDFの`CONFIG_ESP_COREDUMP_FLASH_NO_OVERWRITE`(このプロジェクトでは未設定=デフォルトの`n`)により、**次のクラッシュで既存のcoredumpは自動的に上書きされる** - 手動で消す必要は元々無かった。`esp_core_dump_image_erase()`の呼び出しを削除。むしろ消さないほうが良い: 生のELFダンプ(レジスタ・フルバックトレース・全タスクスタック)が残るので、NVSの短い要約だけでは足りない時に`idf.py coredump-info`等でシリアル経由の深掘りができる。128Kの固定パーティションなので保持し続けても容量的なデメリットも無い。
- **「同じバグでもクラッシュなら通知していい」** → 元々の設計意図を誤解していた。「うざい」の実体は**通常再起動(OTA更新やスクリプトの`restart`等)で誤って通知が飛ぶこと**への懸念で、それは元から`is_crash_like()`(reset reasonのチェック)で完全に防がれている - `crash_notify_url`はクラッシュ相当のreset reasonでしか発火しない。「同じバグの再発を通知しない」抑制ロジックは不要だったので削除し、**クラッシュなら毎回必ず通知する**ように変更。ただし「何回目の再発か」の情報自体は有用なので、`recurred Nx since last cleared`のカウント表示はそのまま残した(通知の可否には影響しない、WebUI表示のみ) - 「Clear last crash」ボタンはこのカウンタを1にリセットするだけの、純粋に表示上の意味になった。

## 追記4: ntfyのトークン(認証)対応

指摘の通り、当初`crash_notify_url`はURLだけでトークンを考慮してなかった。ntfy公式の`curl -H "Authorization: Bearer tk_..." -d "body" url`と同じ形に対応:

- `crash_notify_url "https://ntfy.sh/my-topic"` - 従来通り(公開/無認証トピック向け、Authorizationヘッダ自体を送らない)
- `crash_notify_url "https://ntfy.sh/my-topic", "tk_xxxxxxxx"` - 第2引数(省略可)がトークン。セルフホストのアクセス制御付きサーバーや、ntfy.shの*reserved*トピック向け。内部で`Authorization: Bearer <token>`ヘッダとして送信。

ついでに、HTTPステータスコードのチェックも追加(以前は`esp_http_client_perform()`の戻り値=トランスポート層の成功/失敗しか見ておらず、トークン間違い等で401/403が返ってきても「成功」扱いのまま気づけなかった)。2xx以外なら`ESP_LOGW`で警告を出すようにした。

## 追記5: `crash_notify_test` DSL + ソケット枯渇の修正

- **`crash_notify_test`** - `simulate_crash`(再起動を挟む)を使わずに、`crash_notify_url`のURL/トークンが合ってるかその場で確認できるDSL。今すぐntfy.shへテストPOSTを1本飛ばす。`crash_notify_url`未設定なら`ESP_ERR_INVALID_STATE`で例外を上げる。
- 実機で試したところ`esp-tls: Failed to create socket (family 2 socktype 1 protocol 0)`で失敗。TLS以前、素の`socket()`(TCP)自体が失敗していた - **HTTPSが無理という話ではなく、ソケット枯渇**だった。原因: `CONFIG_LWIP_MAX_SOCKETS`のデフォルト10のうち、`mruby_webui.c`のhttpdサーバーが`HTTPD_DEFAULT_CONFIG()`の`max_open_sockets=7`で7個も予約(うち3個はhttpd自身の内部処理用固定コストで、実際のクライアント接続に使えるのは実質4個)。残り3個をhid_forwarder.cのUDPソケット・debug_stream.cの別httpdインスタンス・DNS/NTP解決等で奪い合っており、そこに新規のHTTPS接続を足そうとして空きが無かった。
  - 対処: `CONFIG_LWIP_MAX_SOCKETS`を10→16に引き上げ(sdkconfig.defaults)。WebUI側の予約数を削る方向も検討したが、それだとWebUI自体が複数タブ/debug streamの常時接続と衝突して不安定になるリスクがあったため、上限を上げる方を選んだ(1ソケットあたり数百バイト程度の固定コストで、起動時に一度確保されるだけ - 今回ずっと追ってた類の"漏れ"とは性質が違う)。

## 参考

- [[ble_idle_crash]] - この保険を作るきっかけになった調査
- `esp32-kvm-ip/main/crash_report.c`/`.h`
- `esp32-kvm-ip/partitions.csv`の`coredump`エントリ
- `esp32-kvm-ip/sdkconfig.defaults`の`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`/`CONFIG_HEAP_TRACING_STANDALONE`
- `esp32-kvm-ip/main/mruby_webui.c`の`status_get_handler()`/`crash_clear_post_handler()`
- `esp32-kvm-ip/main/webui/index.html`の"Clear last crash"ボタン
- ESP-IDFドキュメント: Core Dump (`esp_core_dump_get_summary()`/`get_panic_reason()`), Heap Memory Debugging (`esp_heap_trace.h`)
