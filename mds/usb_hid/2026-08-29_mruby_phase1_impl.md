# mruby Phase1実装(VM組み込み・ビルド配管)

設計/プランは[[2026-08-28_mruby_filter_route]]参照。このドキュメントは実際にesp32-kvm-ipへ組み込んだ内容と、その過程で踏んだビルド上の落とし穴の記録。

## 実装済み(host/device両ロールでビルド確認済み)

- `esp32-kvm-ip/components/mruby/mruby`: mruby本体をgit submoduleでベンダリング(upstream非改変)
- `esp32-kvm-ip/components/mruby/esp32s3_build_config.rb`: xtensa-esp32s3-elf向けクロスビルド設定(自前ファイル、submodule外)。`stdlib`/`stdlib-ext`/`math`/`metaprog`のみ(bigint/complex/rational/regexp/set/time/random/pack/mruby-socket等は除外)
- `esp32-kvm-ip/components/mruby/CMakeLists.txt`: `rake`をCMakeのカスタムターゲットとして呼び出し`libmruby.a`を生成、`${COMPONENT_LIB}`にリンク
- `main/mruby_filter.c/.h`: VM初期化・スクリプトロード(`main/mruby_scripts/default.rb`をEMBED_TXTFILESで埋め込み)、`filter_keyboard`/`filter_mouse`/`route_*_udp`への`mrb_funcall_argv`往復、`hostname`フック。スクリプトエラー時は`mrb_print_error`でログしてfail-open(filter系)/fail-closed(route系)
- `Kconfig.projbuild`に`CONFIG_MRUBY_FILTER_ROUTE_ENABLE`(Cフォールバックスイッチ、デフォルトy)
- `hid_forwarder.c`: 全hookで`mruby_filter_active()`により mruby / 既存C(`filter_rules.h`/`route_rules.h`)を分岐。Cフォールバック側は無傷のまま残置
- `wifi_manager_init()`のhostname引数はNULL許容に変更(NULLなら`esp_netif_set_hostname()`を呼ばない=noset)。`main_host.c`はコンパイル時定数`WIFI_HOSTNAME "-host"`をやめ、`mruby_filter_init()`をwifi初期化前に呼んで`mruby_filter_hostname()`を渡す
- `partitions.csv`新設(factory 3MB)、`sdkconfig`/`sdkconfig.defaults`を8MB flash + custom partition tableに更新。mruby込みのHost role image(約2MB)に対し3MB中33%空き、Device role(mrubyなし)は73%空き

## ビルドで踏んだハマりどころ(次に似た統合をする際のメモ)

1. **PRIV_REQUIRESでのinclude伝播漏れ**: `main`の`idf_component_register`に`PRIV_REQUIRES mruby`を書いても、`main`のコンパイルコマンドに`-I .../mruby/include`が現れなかった(`esp_driver_uart`/`esp_timer`で既に既知だった問題と同一)。`target_link_libraries(${COMPONENT_LIB} PRIVATE idf::mruby)`で明示リンクして解決
2. **presymヘッダ**: mrubyはgemboxの内容に応じて`mruby/presym/id.h`等をビルド時に生成する(`mruby/build/esp32s3/include/`配下)。`idf_component_register`の`INCLUDE_DIRS`に`mruby/include`だけでなくこの生成ディレクトリも足す必要があった
3. **`-Wl,--wrap=longjmp`とstatic archiveのリンク順**: ESP-IDFはXtensa向けに`longjmp`を全体ラップしている(FreeRTOSコンテキストスイッチとレジスタウィンドウの問題対策、`esp_rom/CMakeLists.txt`)が、ラッパー実体(`esp_rom.a`内の`.o`)は「その時点までに誰かが参照して初めて」static archiveから引っ張られる。mrubyの例外処理(`setjmp`/`longjmp`、`src/error.c`ほか)がこのプロジェクトで初めて`longjmp`を実際に呼ぶコードだったため、リンク順(`libmruby.a`が`esp_rom.a`より後)のせいで未解決参照になった。`-u __wrap_longjmp`で強制解決(このプロジェクトの他の類似ワークアラウンドと同じ手法)
4. **実機で発覚: task "main"のスタックオーバーフロー**(ビルドは通っても実機で落ちた): `app_main()`が動く"main"タスクは`CONFIG_ESP_MAIN_TASK_STACK_SIZE`(デフォルト3584byte)しか無く、`mruby_filter_init()`のVM初期化+Prismパーサーがこれを食い潰した。`sdkconfig`/`sdkconfig.defaults`で8192に変更。同じ理由で、実際にHID入力を処理する側(`hid_forwarder_*()`→`mrb_funcall_argv()`)のタスク、具体的には`usb_host_rp2040_bridge.c`の`dispatch_task`、`usb_host_max3421.c`の`max3421_host_task`、`usb_host_task.c`のHIDドライバbackground task(いずれも元4096byte)も同じクラスの問題を踏む可能性が高いため、先回りして8192に統一した。
5. **実機で発覚: GCアリーナの解放漏れによるヒープリーク**: スタックオーバーフローを直した後、マウスを動かすと確実に`(unknown):0: Out of memory (NoMemoryError)`と、UART側の`checksum mismatch`/`frame len ... exceeds max`(resyncing)が頻発した。原因は`mruby_filter.c`の各hook(`mruby_filter_keyboard_report()`/`mruby_filter_mouse_report()`/`mruby_route_*_also_udp()`)が`mrb_gc_arena_save()`/`mrb_gc_arena_restore()`を呼んでいなかったこと。`mrb_funcall_argv()`が返す一時オブジェクト(スクリプト側の配列リテラル等)がGCアリーナに積まれたまま解放されず、マウスの高頻度呼び出し(`dispatch_task`から`hid_forwarder_mouse_sample()`経由で呼ばれる)でヒープを食い潰し続けていた。ヒープ枯渇がシステム全体に波及し、UARTフレーム処理側のバッファ確保等まで巻き込んで壊れていたとみられる(checksum mismatchはmruby本体のバグではなく、この副作用)。各hookの入口で`ai = mrb_gc_arena_save(s_mrb)`、全returnパスの直前で`mrb_gc_arena_restore(s_mrb, ai)`を呼ぶよう修正。**mrubyをCコードに組み込む際、`mrb_funcall`系を高頻度に呼ぶループがあるなら、アリーナの保存/復元は必須**という一般的な教訓。

## 新規に増えたビルド前提

- **git submodule**: `esp32-kvm-ip/components/mruby/mruby`は`Wireless_USBHID`(親)→`esp32-kvm-ip`(submodule)→`mruby`(その中のsubmodule)という2階層目のsubmoduleになった。フレッシュチェックアウトでは`git submodule update --init --recursive`が要る(`--recursive`を付け忘れると空ディレクトリのままビルドが`mruby.h: No such file`等で失敗する)。
- **Ruby + rake**: mruby自身のビルドシステムが必要。ESP-IDFのxtensaツールチェインとは別に、ビルドを実行するホスト機にRuby(rake同梱)が要る。`components/mruby/CMakeLists.txt`の`find_program(RAKE_EXECUTABLE rake)`が見つからない場合は`FATAL_ERROR`で明示的に落ちるので、専用のセットアップスクリプトは用意していない(READMEに一言添えれば十分と判断)。

## 未着手(Phase1の先)

- DSL(source/sink/pipeline)本体の実装 - 現状の`main/mruby_scripts/default.rb`は素通しのみ
- Phase2: LittleFS/SPIFFSスクリプトパーティション + WebUI編集
- 実機での`mrb_funcall()`往復コスト計測、hostname設定の反映確認、フォールバック動作確認(ユーザー側で実施予定)
