# mruby Phase2: WebUI

設計/プランは[[2026-08-28_mruby_filter_route]]の「2. スクリプト更新経路(フェーズ分け)」節参照。Phase1(シリアル書き換え、[[2026-08-29_mruby_phase1_impl]])に続き、ブラウザからスクリプトを編集・保存できるWebUIを実装した。

## 実装した構成

- `main/mruby_webui.h/.c`: `esp_http_server`ベースのHTTPサーバ(ポート80)。Host role(`KVM_ROLE=HOST`)のみ、`main_host.c`が`wifi_manager_init()`成功直後(`mruby_filter_resolve_udp_sinks()`/`mruby_filter_start_net_source()`と同じ並び)に`mruby_webui_start()`を呼ぶ
- `main/webui/index.html`: 完全に静的な1ページ(EMBED_TXTFILESで埋め込み)。サーバ側テンプレート化は一切せず、ページ自身のJSが`fetch()`で`/api/script`・`/api/status`を叩いてtextarea/ステータス欄を埋める
- エンドポイント:
  - `GET /` — 上記の静的ページをそのまま返す
  - `GET /api/script` — 現在有効なスクリプトの生テキスト(`mruby_filter_read_script()`、アップロード済みがあればそれ、無ければ埋め込み`default.rb`)
  - `POST /api/script` — bodyを丸ごと新しいスクリプトとして`mrb_script`パーティションに書き込み(`mruby_filter_write_script()`)、成功したら200を返してから約500ms後に`esp_restart()`
  - `GET /api/status` — `mruby_filter_active()`/`mruby_filter_hostname()`をそのままテキストで返すだけの簡易ステータス

## 設計上の判断

### サーバ側テンプレート化ではなくJS fetchにした理由

最初はプレーンHTML `<form>`(POST、`application/x-www-form-urlencoded`)案も検討したが却下した: フォームエンコードだと`(`/`)`/`:`/`#`/改行など、Rubyコードに大量に出てくる記号がほぼ全て`%XX`(3バイト)に展開される。64KBのスクリプトが最悪200KB近いリクエストボディになりうる計算で、ESP32側でその展開後サイズを受けるのは無駄が大きい。`fetch(url, {method:'POST', body: text})`はデフォルトで`text/plain`の生バイト列を送るため、スクリプトの実サイズとほぼ1:1(UTF-8のみ)で済み、C側もデコード処理が一切要らない(`httpd_req_recv()`をそのまま`mrb_script`パーティション用バッファに読むだけ)。

副産物として、ページ自体(`index.html`)は完全に静的な埋め込みファイルのままで良くなった: スクリプト本文をHTMLに埋め込む(`<textarea>{{content}}</textarea>`のようなテンプレート)ならHTMLエスケープ(`&`/`<`/`>`、および`</textarea>`という部分文字列そのものへの対策)が必要になるが、JSの`textarea.value = text`はブラウザが生テキストとして扱う(HTMLパース対象外)ため、C側でエスケープ処理を書く必要が一切ない。

### 保存後はVMのその場リロードではなく`esp_restart()`

設計doc([[2026-08-28_mruby_filter_route]])の原案は「POSTで受けてパーティションに書き込んでVMをリロード」だったが、実装では保存直後に`esp_restart()`する方式にした。理由:

- `usb_host_backends(*syms)`([[2026-08-29_mruby_phase1_impl]]参照)によるUSB Hostバックエンド選択は`main_host.c`の起動シーケンスに組み込まれており、動作中に「今使っているバックエンドを止めて別のバックエンドに切り替える」ホットスワップは(RP2040 bridge/MAX3421E/native OTGそれぞれ別ドライバの初期化・タスク構成を持つため)Phase2の範囲でやるには実装コストに見合わない
- `:udp` sourceの`listen`ポートが変わった場合も同様(`net_source_task`は起動時に一度bindするだけで、動的なrebind経路は無い)
- スクリプトのロード自体は`mruby_filter_init()`が起動時に一度だけ行う設計(DSLの「実行時の性能設計」節、[[2026-08-28_mruby_filter_route]])なので、その関数を実行中のVMに対して安全に再実行できる保証もない(既存の`s_sinks`/`s_pipelines`等グローバル状態のリセットは`reset_dsl_state()`任せで、動作中の別タスクからの参照との競合は考慮されていない)
- Phase1のシリアルアップロード(`bin/upload_mruby_script.py`)も「書き込み後に手動でリセット」という運用だったので、WebUI版もその延長(手動リセットの手間を「保存ボタンが自動でトリガーする」に変えただけ)と捉えれば一貫性がある

結果として「reflash無しで挙動を変えたい」という当初の動機は満たしつつ(アプリイメージの再ビルド・再書き込みは不要)、「保存したらすぐ効く」ではなく「保存したら数秒だけ再起動が挟まる」仕様になっている。ページのJSはこれを踏まえ、保存後に「Refresh this page in a few seconds」という案内を出すだけに留めている。

### ボタンレス運用(常時起動)で問題ないか

[[2026-08-28_mruby_filter_route]]で懸念していた「ブラウザアクセス時にだけ起動」(物理ボタン等でのゲーティング)は採用せず、WiFi接続後は常時`httpd_start()`しておく方式にした。`esp_http_server`はリクエストが来ない間は待受ソケットが1つ開いているだけで、専用タスクもリクエスト処理時以外はブロックして休止する(常時ポーリングするような実装ではない)ため、アイドル時の負荷は無視できる程度と判断。ボタン等のゲーティング機構を別途実装するコストの方が見合わない。

### 認証は無し

[[2026-08-28_mruby_filter_route]]の前提通り「自分専用の道具」であり、非プログラマ向けの配布物ではない。WebUIにBasic認証等は付けていない(同一LAN内からアクセスできれば誰でもスクリプトを書き換えられる)。将来の懸念になった場合はここに追記する。

## 実装上の注意点

- `mruby_filter.c`の`load_uploaded_script()`(VMへロード)と新設`mruby_filter_read_script()`(WebUI表示用の生バイト読み出し)は、どちらも同じ`mrb_script`パーティション読み出しロジック(`read_script_partition_raw()`に共通化)を使う。VMが起動していなくても(`s_active == false`でも)動く — パーティションの生バイトを読むだけでmrb_stateに触れないため、mruby自体が初期化に失敗していてもWebUIで壊れたスクリプトを直接見て修正・再アップロードできる
- `mruby_filter_write_script()`は`bin/upload_mruby_script.py`と同じオンフラッシュ形式(4バイトリトルエンディアン長 + UTF-8本文)で書く。`esp_partition_erase_range()`でパーティション全体(64KB、消去セクタ境界と一致)を一括消去してから書き込み、`parttool.py write_partition`と同じ粒度
- POSTハンドラは`httpd_resp_send()`で200を返した直後に`esp_restart()`を呼ばず、別タスク(`restart_task`、500ms delay後に`esp_restart()`)を起こす形にしている。httpdワーカータスク自身のソケット送信・後始末が完了する前に再起動すると、クライアントがレスポンスを受け取れない可能性があるため
- `esp_http_server`の`main`コンポーネントへのinclude伝播は、`esp_driver_uart`/`esp_timer`/`esp_partition`/`mruby`と同じ問題([[2026-08-29_mruby_phase1_impl]]のビルドで踏んだハマりどころ1番)を踏んだ。`PRIV_REQUIRES`に足すだけでは`-I`が伝播せず、`target_link_libraries(${COMPONENT_LIB} PRIVATE idf::esp_http_server ...)`で明示リンクする既存のワークアラウンドに追加する形で解決
- スクリプトサイズの上限は`bin/upload_mruby_script.py`の`MAX_SCRIPT_SIZE`(64K - 4バイトヘッダ)と揃えた定数を`mruby_webui.c`にも持たせている(`mrb_script`パーティションの実容量に依存する値なので、`partitions.csv`を変更する場合はこの3箇所すべて揃える必要がある: `partitions.csv`のサイズ、`upload_mruby_script.py`の定数、`mruby_webui.c`の定数)

## ビルド確認

Host role: 3MBパーティション中32%空き(Phase1時点の33%からWebUI追加分でわずかに減少)。Device role(mrubyもWebUIも含まない)は73%空きで変化なし。両ロールともビルド成功。実機での動作確認(ページの表示、保存→再起動→新スクリプト反映まで)は未実施。

## 未着手(Phase3、design docの「余力があれば」項目)

- 保存前のシンタックスチェック(mrubyパーサーだけ先に走らせて構文エラーを弾く)
- スクリプトのバージョン履歴(直前世代を1つ保持してロールバック)
