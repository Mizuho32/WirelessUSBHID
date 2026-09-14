# アイデアメモ: M5Stackを無線シリアルコンソール/ステータスパネルにする

[[uart_bridge]]を作った直後の雑談から。実装はまだ、思いついたことのメモ。

## 着想

大昔のシリアルコンソール(VT100など)は「TX/RX+GND+ボーレートで文字ストリームを受けて画面に出す」だけの装置。[[uart_bridge]]がやってることも構造的には全く同じで、違うのはWi-Fi経由で無線化されている点とLCD表示な点だけ。

M5Stack(ESP32+LCD)側に「UART(または`:udp`)で受けた文字列を画面に出すだけ」のスケッチを書けば、原理上はそのまま無線シリアルコンソール/常時表示のステータスパネルになる。

## 構成案

- **有線**: `sink :x, :uart, tx:` から出た生バイトをM5StackのUART RXへ直結。`M5GFX`/`LovyanGFX`で画面に流し込むだけ。
- **無線(こっちが本命)**: 配線せず`source :dbg_uart, :uart` (または任意のステータス文字列生成元) → `sink :m5_display, :udp, host: "M5StackのIP", port: ...`でネットワーク越しに飛ばし、M5Stack側はUDPリスナーで受けて画面表示。Wi-Fi対応M5Stackモデルなら物理配線ゼロで成立する。
- **双方向(欲張るなら)**: M5Stackのボタン/タッチからの入力を`source :net_in, :udp, listen: ...` → `from ..., kind: :uart` → `sink :dbg_uart_out, :uart, tx:`経由でesp32-kvm-ip側に戻せば、簡単なコマンド送信もできる小さいステータスパネルになる。

## 用途イメージ

- mrubyスクリプト側でタイマー(`after`)から定期的にステータス文字列(BLEペア状態、WiFi状態、heap_monitorの数値など)を流し込み、M5Stack側に常時表示。
- M5Stackからの入力バイト列を簡単なコマンドとしてパースし、`system_control`/`ble_pair_switch`などのDSL呼び出しにマッピングすれば、簡易インタラクティブシェルっぽいものも狙える。

## 未着手事項

- M5Stack側のスケッチ設計(表示レイアウト、行送り/スクロール、UDP受信のパケットロス対策など)
- 具体的な配線 or ネットワーク経路の選定
- コマンドパーサの設計(双方向にする場合)

## 参考

- [[uart_bridge]] - このアイデアの土台になった`:uart` source/sink DSL
