# mruby Phase2: WebUI

設計/プランは[[2026-08-28_mruby_filter_route]]の「2. スクリプト更新経路(フェーズ分け)」節参照。Phase1(シリアル書き換え、[[2026-08-29_mruby_phase1_impl]])に続き、ブラウザからスクリプトを編集・保存できるWebUIを実装した。

## 実装した構成

- `main/mruby_webui.h/.c`: `esp_http_server`ベースのHTTPサーバ(ポート80)。Host role(`KVM_ROLE=HOST`)のみ、`main_host.c`が`wifi_manager_init()`成功直後(`mruby_filter_resolve_udp_sinks()`/`mruby_filter_start_net_source()`と同じ並び)に`mruby_webui_start()`を呼ぶ
- `main/webui/index.html`: 完全に静的な1ページ(EMBED_TXTFILESで埋め込みフォールバック、後述の`webui_html`パーティションがあればそちら優先)。サーバ側テンプレート化は一切せず、ページ自身のJSが`fetch()`で`/api/script`・`/api/status`を叩いてtextarea/ステータス欄を埋める
- エンドポイント:
  - `GET /` — 上記のページを返す(`webui_html`パーティションが有効ならそれ、無ければ埋め込み`index.html`)
  - `GET /api/script` — 現在有効なスクリプトの生テキスト(`mruby_filter_read_script()`、アップロード済みがあればそれ、無ければ埋め込み`default.rb`)
  - `POST /api/script` — bodyを丸ごと新しいスクリプトとして`mrb_script`パーティションに書き込み(`mruby_filter_write_script()`)、成功したら200を返してから約500ms後に`esp_restart()`
  - `GET /api/status` — `mruby_filter_active()`/`mruby_filter_hostname()`と、フロントエンドが埋め込み/カスタムどちらか、をテキストで返す簡易ステータス
  - `POST /api/frontend` — bodyを丸ごとページ自身の新しい`webui_html`として書き込み(`write_webui_html_partition()`)。ページ上にボタンは無く、`bin/upload_mruby_script.py --partition webui_html`経由のシリアル専用(後述)

## シンタックスハイライト(CodeMirror、CDN経由)

`index.html`は[CodeMirror 5](https://codemirror.net/5/)をCDN(`cdn.jsdelivr.net`、`@5`エイリアスで5系最新に追従)から読み込み、`textarea`を`CodeMirror.fromTextArea(..., {mode: 'ruby', ...})`でラップしてシンタックスハイライト付きエディタにする。CodeMirror本体はこのプロジェクトのパーティションには一切含まれず(`webui_html`/埋め込み`index.html`はページ自身のHTML/CSS/JSグルーコードのみ)、ブラウザが実行時にCDNから取得する。

- **CDN依存のフォールバック**: `typeof CodeMirror !== 'undefined'`をチェックし、CDNが読み込めなかった場合(このLANにインターネット接続が無い等)は`cm = null`のまま、素の`<textarea>`編集にフォールバックする(`getScriptText()`/`setScriptText()`/`setEditable()`が`cm`の有無で分岐)。オフラインでも編集自体は必ず動く設計
- CodeMirror本体を埋め込み(バンドル)ではなくCDN参照にしたのは、ユーザーから「外部jsを使って」という明示の指定があったため。バンドルする場合はCodeMirrorのJS/CSS/rubyモードの実ファイルを取得してEMBED_TXTFILESに足す必要があり、`webui_html`パーティションのサイズもそれに応じて拡張が要る(今回は不要)

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

### フロントエンド自体もシリアルでアップロード可能に(`webui_html`パーティション)

「mrubyスクリプトみたいにフロントエンドもUARTで更新できるとクール」という要望を受け、`index.html`自体も`mrb_script`と全く同じ仕組み(4バイト長ヘッダ + 生バイト、`bin/upload_mruby_script.py`で書き込み)で差し替え可能にした: 新設`webui_html`パーティション(subtype `0x51`、32K)。

- `bin/upload_mruby_script.py --partition webui_html path/to/index.html`でアップロード
- `mrb_script`と違い**リセット不要**: `index_get_handler()`は毎リクエストごとにパーティションを読みに行くだけで、VMや起動シーケンスに絡む状態を一切持たないため、アップロード後すぐ次回アクセスから反映される
- WebUIのページ自身に「フロントエンドをアップロード」ボタンは置いていない(HTTPのPOST 1本で自分自身を差し替える経路を用意すると、壊れたHTML/JSをアップロードした場合に「直すための唯一の手段(そのページ)自体が使えなくなる」というリスクがある)。`/api/frontend`はエンドポイントとしては存在するが、シリアル経由のツールからのみ叩く運用にした
- `bin/upload_mruby_script.py`は元々`mrb_script`専用だったが、この機能追加を機に`--partition`引数で任意のraw storageパーティションを指定できるよう一般化した。上限サイズもこれまでの決め打ち定数(`MAX_SCRIPT_SIZE`)ではなく、`partitions.csv`をその場でパースして対象パーティションの実サイズから算出するようにした(後述「実装上の注意点」参照)

## 実装上の注意点

- `mruby_filter.c`の`load_uploaded_script()`(VMへロード)と新設`mruby_filter_read_script()`(WebUI表示用の生バイト読み出し)は、どちらも同じ`mrb_script`パーティション読み出しロジック(`read_script_partition_raw()`に共通化)を使う。VMが起動していなくても(`s_active == false`でも)動く — パーティションの生バイトを読むだけでmrb_stateに触れないため、mruby自体が初期化に失敗していてもWebUIで壊れたスクリプトを直接見て修正・再アップロードできる
- `mruby_filter_write_script()`は`bin/upload_mruby_script.py`と同じオンフラッシュ形式(4バイトリトルエンディアン長 + UTF-8本文)で書く。`esp_partition_erase_range()`でパーティション全体(64KB、消去セクタ境界と一致)を一括消去してから書き込み、`parttool.py write_partition`と同じ粒度
- POSTハンドラは`httpd_resp_send()`で200を返した直後に`esp_restart()`を呼ばず、別タスク(`restart_task`、500ms delay後に`esp_restart()`)を起こす形にしている。httpdワーカータスク自身のソケット送信・後始末が完了する前に再起動すると、クライアントがレスポンスを受け取れない可能性があるため
- `esp_http_server`の`main`コンポーネントへのinclude伝播は、`esp_driver_uart`/`esp_timer`/`esp_partition`/`mruby`と同じ問題([[2026-08-29_mruby_phase1_impl]]のビルドで踏んだハマりどころ1番)を踏んだ。`PRIV_REQUIRES`に足すだけでは`-I`が伝播せず、`target_link_libraries(${COMPONENT_LIB} PRIVATE idf::esp_http_server ...)`で明示リンクする既存のワークアラウンドに追加する形で解決
- スクリプト/フロントエンドのサイズ上限(`mruby_webui.c`の`MAX_SCRIPT_SIZE`/`MAX_FRONTEND_SIZE`)は`partitions.csv`の対応パーティションサイズ-4バイトヘッダと手動で揃える必要がある定数のまま(C側はコンパイル時定数なので`partitions.csv`を実行時に読む訳にはいかない)。`bin/upload_mruby_script.py`側は前述の通りパースして自動追従するようにしたので、揃える必要があるのは`partitions.csv`と`mruby_webui.c`の2箇所に減った(Phase2当初は`upload_mruby_script.py`の決め打ち定数も含めて3箇所だった)
- `read_webui_html_partition()`/`write_webui_html_partition()`は`mruby_filter.c`の`read_script_partition_raw()`/`mruby_filter_write_script()`とほぼ同じロジックだが、共通ヘルパーへの統合はしていない: 対象パーティションが違う(それぞれ`webui_html`/`mrb_script`固有の定数を持つ)上に呼び出し元は各ファイル1箇所ずつなので、抽象化するほどの重複ではないと判断した

## ビルド確認

Host role: 3MBパーティション中32%空き(Phase1時点の33%からWebUI追加分でわずかに減少)。Device role(mrubyもWebUIも含まない)は73%空きで変化なし。両ロールともビルド成功。実機での動作確認(ページの表示、保存→再起動→新スクリプト反映、CodeMirrorのCDN読み込み、`webui_html`アップロード)は未実施。

### 実機で発覚: `GET /api/script`で断続的な"out of memory" 500

実機で`httpd_txrx: httpd_resp_send_err: 500 Internal Server Error - out of memory`が時々出る、との報告。原因は`script_get_handler()`が毎回`MAX_SCRIPT_SIZE + 1`(約64KB)のバッファを決め打ちでmallocしていた上に、`mruby_filter_read_script()`が内部でさらに`read_script_partition_raw()`(malloc→memcpy→free)を経由していたため、ページ読み込みのたびに一瞬**2つの64KB近いバッファが同時に生きる**(実際のスクリプトサイズが数KBでも関係なく)状態になっていたこと。

この基板は`sdkconfig`で`CONFIG_SPIRAM`が未設定(PSRAM無効) — [[2026-08-28_mruby_filter_route]]の「想定ハードウェア」節は8MB PSRAM前提で容量に余裕があると見積もっていたが、実際のビルド設定では有効化されておらず、mrubyのVMヒープもWiFi/lwIPバッファも各タスクのスタック(mruby関連は軒並み8192byteに倍増済み、[[2026-08-29_mruby_phase1_impl]]参照)も、全て内蔵SRAM(実質300〜400KB程度)を奪い合っている。そこへ`GET /api/script`(ページ読み込みのたびに`GET /api/status`と揃って毎回呼ばれる)が瞬間的に128KB近く要求すれば、ヒープの空き状況やフラグメンテーション次第で失敗するのは無理もない。

対処(いずれもコード側のみ、`partitions.csv`/`sdkconfig`は変更していない):

- `mruby_filter.c`: `find_mrb_script_partition()`(4バイトのヘッダだけ読んで検証、mallocなし)を新設し、`read_script_partition_raw()`(`load_uploaded_script()`用、そのままmalloc+memcpyが必要)と`mruby_filter_read_script()`で共有。後者は中間バッファを完全に廃止し、パーティションから呼び出し側のバッファへ直接`esp_partition_read()`するように変更(二重確保が解消)
- 新設`mruby_filter_script_len()`: 実際のスクリプト長を(中身を読まず)返す。`mruby_webui.c`の`script_get_handler()`はこれで実サイズ分だけmallocするようになり、常に64KB決め打ちで確保していたのをやめた(典型的なDSLスクリプトなら数KBで済む)
- `mruby_webui.c`: `status_get_handler()`の「`webui_html`パーティションに中身があるか」チェックも、フルバッファをmalloc→即freeする実装(`read_webui_html_partition()`)から、ヘッダだけ読む`find_webui_html_partition()`に変更(最大32KB分の無駄なmalloc/freeを排除)

**未対応・保留**: PSRAMを`CONFIG_SPIRAM=y`で有効化すれば、これらのmalloc全般(将来的にはmrubyのVMヒープ自体も)が8MBの外部メモリへ逃がせるようになり、この種のヒープ逼迫はより根本的に解消される可能性が高い。ただし[[2026-08-28_mruby_filter_route]]の「一番のリスク」節で懸念していた通り、PSRAMアクセスは内蔵SRAMより低速(キャッシュに乗らない範囲は特に)なため、マウスの高頻度dispatch経路への影響を実機で確認しないまま有効化するのは避けた。今回はWebUI側の無駄な確保を削るだけに留め、PSRAM有効化は別途判断が要る変更として保留している。

## 未着手(Phase3、design docの「余力があれば」項目)

- 保存前のシンタックスチェック(mrubyパーサーだけ先に走らせて構文エラーを弾く)
- スクリプトのバージョン履歴(直前世代を1つ保持してロールバック)
