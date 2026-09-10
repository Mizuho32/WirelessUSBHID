# mrubyスクリプトの`debug_print`をWebUIへライブ配信する(debug_stream.c)

`debug_print(*args)`(`mds/usb_hid/2026-08-28_mruby_filter_route.md`で導入)はこれまでESP_LOGI(シリアルの`idf.py monitor`)専用だった。今回、WebUI経由でも見られるようにした。

## 要件・設計判断(相談しながら決めた点)

- **既存のWebUI用httpd(`mruby_webui.c`)には相乗りしない**。理由: `esp_http_server`はデフォルトで**インスタンスにつき単一タスク**(`select()`で全ソケットを見て、リクエスト処理も同じタスク内で順番に実行 - `components/esp_http_server/src/httpd_main.c`の`httpd_server()`)。ストリーム用ハンドラが「送るものがあるまで居座る」実装だと、繋がってる間**そのインスタンスの他の全エンドポイントが詰まる**(WebUIのページ読み込み・スクリプトsave・statusなど)。WebSocketでもHTTP chunked/SSEでもこの制約自体は変わらない。
- 上記より、**別ポート(81)の別httpdインスタンス**(専用タスク)に分離。メインWebUIには一切影響しない。
- **常時起動ではなく、WebUIの明示Start/Stopボタンでのみ起動/停止**。理由: 別インスタンスは「専用タスクのスタック+リスニング/クライアントソケットのバッファ」を常駐で食う。これは`mruby`のparseピークのような一時的消費と違い**ボードが起きてる間ずっと確保されっぱなし**になる負担で、`mds/usb_hid/2026-09-09_ble_webui_syntax_check_oom.md`で判明した「BLE常駐だけで内部SRAMのlargest_free_blockが61440→17408まで落ちる」という、ただでさえ厳しい内部SRAM事情にそのまま積み増しされる。見る時だけ払うコストにするため、on-demand化した。
- Start/Stopは自動検知(タブを閉じたら自動停止、等)ではなく**ユーザーがWebUIから明示操作**(ユーザーの要望: 「WebUIは開きっぱなしにするとかあるので、デバッグしたいときだけユーザーUIから明示Star/Stopのが都合がいい」)。タブを閉じてもサーバー側は動き続けるので、見終わったら「Stop debug stream」を押す必要がある(index.htmlのヒントに明記)。
- 過去ログ(バックログ)は保持しない。Start後に流れてきた分だけを表示する(シンプルさ優先)。
- `debug_print`の出力先はUART(ESP_LOGI)とこのHTTPストリームの2つ。UARTはデフォルトON、スクリプトから`debug_print_uart false`でOFFにできる(既存の`usb_suspend_wifi_sleep`等と同じ、真偽値トグルのDSLパターン)。HTTPストリーム側はスクリプトのトグルではなく、WebUIのStart/Stop(=そのインスタンスが起動してるかどうか)そのものがゲートになる。

## 実装

### `main/debug_stream.c`/`.h`(新規、Host role専用)

- `debug_stream_init()`: 起動時に1回、キュー(`QueueHandle_t`、16エントリ×120byte≒2KB)だけ確保。httpdインスタンスはまだ立てない。
- `debug_stream_push(line)`: 非ブロッキング(`xQueueSend(..., 0)`)。mrubyのホットパス(`mruby_filter.c`の`s_mrb_mutex`保持中)から呼ばれるので、詰まってたら待たずに捨てる。httpdが起動してなければ即return(キューに積まない)。
- `debug_stream_start()`/`_stop()`: 別httpdインスタンス(ポート81)の起動/停止。両方冪等(2重Start/2重Stopは無害)。
- `GET /stream`ハンドラ: キューを`xQueueReceive()`で読んでSSE形式(`data: <line>\n\n`)で`httpd_resp_send_chunk()`。

### キューは「常駐」、httpdインスタンスだけ動的

キュー自体はStart/Stopに合わせて作り直さず、`debug_stream_init()`で一度作ったまま(プロセス寿命分、固定約2KB)保持する設計にした。理由: `debug_stream_push()`はmrubyのホットパス(任意のタイミング)から、`debug_stream_start()`/`_stop()`はWebUIの別タスクから、非同期に触る。もしキュー自体もStart/Stopで作成/破棄していたら、「`_stop()`がキューを`vQueueDelete()`した直後に、ちょうど別タスクの`push()`がそのハンドルへ`xQueueSend()`する」というuse-after-freeの競合窓が(狭いとはいえ)実在する。キューを不滅にしておけば、この種の競合はそもそも起こらない(最悪、誰も読んでないキューに積んで捨てられるだけ)。httpdインスタンス(タスク+ソケット、RAM消費の大部分)だけを動的にすれば、狙った「見てる時だけ払う」効果はほぼそのまま得られる。

### `httpd_stop()`はハンドラが戻らないと完了しない(実機ソースで確認済み)

`components/esp_http_server/src/httpd_main.c`を読んで確認した重要な事実:

```c
esp_err_t httpd_stop(httpd_handle_t handle)
{
    ...
    cs_send_to_ctrl_sock(hd->msg_fd, hd->config.ctrl_port, &msg, sizeof(msg)); // shutdown信号を送るだけ
    while (hd->hd_td.status != THREAD_STOPPED) {
        httpd_os_thread_sleep(100); // 呼び出し元をここでブロックして待つ
    }
    ...
}
```

シャットダウン信号は、そのインスタンス自身のタスクが`select()`ループ(`httpd_server()`)に戻ってきて初めて処理される。つまり`GET /stream`のハンドラ関数が`xQueueReceive(..., portMAX_DELAY)`のような**無期限待ち**のまま戻らなければ、そのタスクは永遠に`select()`に戻れず、`httpd_stop()`は**呼び出し元を無限に待たせる**。今回`_stop()`はWebUIのメインhttpdの方のワーカータスク(`POST /api/debug_stream/stop`ハンドラ)から呼ぶので、これを見誤ると**メインWebUIごと固まる**ところだった。

対策: `GET /stream`ハンドラは`xQueueReceive()`に200ms程度の短いタイムアウトを付けて定期的にループへ戻り、`s_should_stop`フラグ(`debug_stream_stop()`が`httpd_stop()`を呼ぶ**前**にセットする)を見て自発的にreturnする。これで`httpd_stop()`は数百ms以内に完了する。

### CORS

ポートが違う(81 vs メインWebUIの80)= ブラウザから見ると別オリジン。`Access-Control-Allow-Origin: *`を`/stream`のレスポンスに付けないと、「接続はできるがJSでは読めない」という分かりにくい詰まり方をする。`stream_get_handler()`で対応済み。

### `ctrl_port`の衝突

`HTTPD_DEFAULT_CONFIG()`はどのインスタンスも同じデフォルト`ctrl_port`(32768、ループバックの制御用UDPソケット)を使う。メインWebUIのインスタンスが既にそれを使っているので、debug_stream側は明示的に別の値(32769)を指定しないと`httpd_start()`が失敗する。

### WebUI(`index.html`)

「Start debug stream」/「Stop debug stream」ボタン+`<pre>`のログ表示欄。Startを押すと`/api/debug_stream/start`をPOSTしてから、別ポートへ`EventSource`で接続。表示は直近500行にバウンド。

## 実機ビルド確認

Host/Device両ロールともビルド成功、Flash使用率は従来通り(Host 25%空き、Device 74%空き、変化なし)。実機での動作確認(WebUIからのStart/Stop、実際のストリーミング)は未実施 - 次回実機テストで確認予定。

## 参考

- `esp32-kvm-ip/main/debug_stream.c`/`.h`
- `esp32-kvm-ip/main/mruby_filter.c`の`dsl_debug_print()`/`ruby_debug_print_uart()`
- `esp32-kvm-ip/main/mruby_webui.c`の`debug_stream_start_post_handler()`/`debug_stream_stop_post_handler()`
- `esp32-kvm-ip/main/webui/index.html`
- `mds/usb_hid/2026-09-09_ble_webui_syntax_check_oom.md`(この機能をon-demand化した動機になった内部SRAM事情)
