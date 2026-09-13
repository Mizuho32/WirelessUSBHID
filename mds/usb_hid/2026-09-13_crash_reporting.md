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

## 参考

- [[ble_idle_crash]] - この保険を作るきっかけになった調査
- `esp32-kvm-ip/main/crash_report.c`/`.h`
- `esp32-kvm-ip/partitions.csv`の`coredump`エントリ
- `esp32-kvm-ip/sdkconfig.defaults`の`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`/`CONFIG_HEAP_TRACING_STANDALONE`
- `esp32-kvm-ip/main/mruby_webui.c`の`status_get_handler()`/`crash_clear_post_handler()`
- `esp32-kvm-ip/main/webui/index.html`の"Clear last crash"ボタン
- ESP-IDFドキュメント: Core Dump (`esp_core_dump_get_summary()`/`get_panic_reason()`), Heap Memory Debugging (`esp_heap_trace.h`)
