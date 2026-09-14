# UART source/sink - 無線UARTロガー/リレー

## 動機

「mruby配線だけでUART無線ロガーができると便利」というリクエスト。既存の[[mruby_filter_route]]の`source`/`sink`/`pipeline` DSL(keyboard/mouse/consumer/system_control専用)に、任意のGPIOに配線したUARTペリフェラルの生バイト列を運べる新しい種類の`source`/`sink`を追加した。

## ハードウェア的な制約

ESP32-S3はUARTコントローラを3系統(UART0/1/2)しか持たない:

- UART0 = コンソール(`CONFIG_ESP_CONSOLE_UART_NUM`)
- UART1 = `usb_host_rp2040_bridge.c`の`BRIDGE_UART_PORT`(`rp2040_bridge`バックエンドが選択されている場合のみ実際に使用)
- UART2 = 完全に空き

GPIOマトリクス経由で任意ピンに配線できるので、`port:`オプションで将来UART0/1以外を狙う分には特に制約はない(ただしUART0/1は`mruby_filter_resolve_uart_bridges()`が予約チェックで弾く)。

## 設計:なぜsourceとsinkでrx:/tx:が分かれているか

最初`source :x, :uart, rx: 4, tx: 5, baud: ...`のように両方を1つの`source`に詰め込む案を出したが、指摘の通りこれは既存の`source`=読む側/`sink`=書く側という役割分担から外れていた。

他の`source`/`sink`ペア(`usb_host`↔`typec`)は元々別々のペリフェラル(USB Hostコントローラ vs USB Deviceコントローラ)なので設定がバッティングしない。UARTは逆で、RXもTXも同じ1個のペリフェラルが担当してて、ピン配線もボーレートもペリフェラル単位の設定。そこで:

```ruby
source :dbg_uart_in,  :uart, rx: 4, baud: 115200
sink   :dbg_uart_out, :uart, tx: 5, baud: 115200
```

のようにDSL上はsource=RX/sink=TXで綺麗に分け、C側(`mruby_filter_resolve_uart_bridges()` → `uart_bridge_configure()`)で「同じ`port:`のsourceとsinkは同じ物理ペリフェラルの話」とみなして1回の`uart_driver_install`/`uart_set_pin`にまとめる。baudが食い違ってたらロード時ではなく解決時にエラーログを出して古い方を維持(片方だけ壊れた設定として無視)。

`port:`はv1から必須オプションではなくオプション(デフォルト`UART_BRIDGE_DEFAULT_PORT`=2)だが、常に指定可能 - 「任意の状況を考えている」という要望を踏まえ、決め打ちにはしなかった。

## アーキテクチャ

- 新しい`PIPE_UART`という`kind`を追加(既存の`PIPE_KEYBOARD`/`PIPE_MOUSE`/`PIPE_CONSUMER`/`PIPE_SYSTEM_CONTROL`と同じ並び)。keyboard/mouse/consumerが固定長HIDレポートのHashを運ぶのに対し、UARTの`to`/`branch`ブロックは生バイト列をRubyの`String`としてそのまま受け取る/返す(nilを返せばdrop)。
- `source :name, :uart, rx:, port:, baud:` / `sink :name, :uart, tx:, port:, baud:` - 既存の`source`/`sink`DSLに`:uart`タイプを追加しただけなので、`pipeline`/`from`/`to`/`branch`は無改造でそのまま使える。
- `:uart`ソースは他の任意の既存sink(`:udp`はもちろん)にそのまま`to`で繋げられる。ただし`:typec`/`:ble`は生バイト列を運べる実体がないので、パイプライン構築時(`to`/`branch`呼び出し時)に即エラーにする(`check_uart_sink_compat()`)。
- 双方向対応: `source ..., :uart`(UART RX→pipeline)と`sink ..., :uart`(pipeline→UART TX)の両方を実装。`:udp`ソースの`from ..., kind: :uart`も対応済みなので、ネットワーク越しにUART TXへ書き込む中継(リモートAT送信など)も可能。

### 新しいワイヤフォーマット(`protocol.h`)

既存の`udp_packet_t`は16バイト固定長(mouse/keyboard/consumer/system_controlのどれも8バイトのunion)。UARTの生バイト列は可変長なので、`udp_packet_t`に新しい`event_type_t`を足す形にはせず、**別のパケット形状**を追加した:

```c
#define RAW_BYTES_MAGIC   0xB17E
#define RAW_BYTES_MAX_LEN 512
typedef struct __attribute__((packed)) {
    uint16_t magic;  // RAW_BYTES_MAGIC
    uint16_t len;
    uint8_t  data[RAW_BYTES_MAX_LEN];
} raw_bytes_packet_t;
```

両方のパケット形状の先頭2バイトが`magic`で揃っているので、受信側(`mruby_filter.c`の`net_source_task()`)は`recvfrom`後にまず`magic`だけ見て、`PACKET_MAGIC`(0xCAFE)なら既存の固定長HID経路、`RAW_BYTES_MAGIC`(0xB17E)ならUART中継経路、とその場で分岐する。同じソケット/ポートを共有していても安全に共存する。

Device role側の`network_task.c`(mrubyを使わない素のUDP→type-c専用の別実装)は変更していない - `len != PACKET_SIZE`のチェックが既にRAW_BYTES_MAGICパケットを安全に弾くので、そのままで問題ない。

### 新規ファイル

- `main/uart_bridge.h`/`.c` - UARTペリフェラルの実体管理(`uart_driver_install`/`uart_set_pin`)、RXタスク(`uart_read_bytes()`をポーリングし256バイトチャンク単位で`mruby_dispatch_uart_rx()`に渡す)、TX書き込み(`uart_bridge_write()`)。

### mruby_filter.cへの追加

- `mruby_filter_resolve_uart_bridges()` - スクリプトロード後、WiFiより前に呼ぶ(UARTはlwIP非依存なので`mruby_filter_resolve_udp_sinks()`より早く、`mruby_filter_init()`直後に`main_host.c`から呼んでいる - RX取りこぼしの窓を最小化)。予約ポート(UART0=コンソール、UART1=rp2040_bridgeバックエンド選択時)はここで弾く。
- `mruby_dispatch_uart_rx(port, data, len)` - `uart_bridge.c`のRXタスクから呼ばれる。**制限事項**: 複数の`:uart`ソースが宣言されても全部`s_pipelines[PIPE_UART]`という1つの共有バケツに合流する(keyboard/mouseなど他のkindも同じ設計 - 物理デバイスが複数あっても1つのpipelineバケツを共有する)。今のところ「UART RXは同時に1系統だけ使う」というユースケース(無線ロガー)には十分。

## 動作確認

`./bin/build_host.sh build`(Host role)・`./bin/build_device.sh build`(Device role、`protocol.h`共有部分の影響確認)ともにクリーンビルド成功。実機での動作確認はまだ。

## サンプルスクリプト

`main/mruby_scripts/examples/uart_logger.rb` - keyboard/mouse/consumerの素通し + UART無線ロガー(`rx: 4` → `sink :log_pc, :udp`)の完全な動作例。双方向・ネットワーク中継のコード例もコメントで併記。

## 今後の課題

- 複数`:uart`ソース同時使用時の per-port pipeline分離(今は全部合流)。
- 実機での動作確認(ボーレート/チャンクサイズの実測、`RAW_BYTES_MAX_LEN`超えのフレームの扱いなど)。

## 参考

- [[mruby_filter_route]] - source/sink/pipeline DSLの元設計
- `esp32-kvm-ip/main/uart_bridge.h`/`.c`
- `esp32-kvm-ip/main/mruby_filter.c`の`dsl_source()`/`dsl_sink()`の`:uart`分岐、`mruby_filter_resolve_uart_bridges()`、`dispatch_uart_via()`
- `esp32-kvm-ip/main/protocol.h`の`raw_bytes_packet_t`
- `esp32-kvm-ip/main/mruby_scripts/examples/uart_logger.rb`
