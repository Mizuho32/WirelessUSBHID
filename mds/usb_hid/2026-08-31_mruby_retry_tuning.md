# mrubyから設定可能なリトライ/タイムアウト系パラメータ

## RP2040ブリッジprobeのリトライ/タイムアウト

### 背景
ESP32とRP2040は同じ電源で同時に起動することが多く、RP2040側の起動(USB Hostスタック初期化含む)が遅れると、`usb_host_rp2040_bridge_probe()`の固定800ms窓を待っている間にRP2040の最初のハートビートが間に合わず、実際にはRP2040が繋がっているのに「検出できない」= HID操作が効かないことがあった。

### 実装
- `usb_host_rp2040_bridge_probe()`(`main/usb_host_rp2040_bridge.c`)を、固定800msの単発待ちから**試行回数×タイムアウトのループ**に変更。試行ごとにログ出力。
- mrubyから設定可能(`main/mruby_filter.c`/`.h`):
  - `rp2040_bridge_probe_retries N`(デフォルト3)
  - `rp2040_bridge_probe_timeout_ms N`(デフォルト800、従来と同じ)
  - 未設定ならデフォルトのまま(3回×800ms、最悪ケースで+約1.6秒の追加起動遅延)。1回未満・50ms未満は下限にクランプ。
- `main/mruby_scripts/default.rb`にコメントアウトの設定例を追加。

### 影響範囲
Host role限定(`usb_host_rp2040_bridge.c`/`mruby_filter.c`はHOST SRCSのみ)。Device roleは無関係、ビルド確認済み。実機で動作確認済み。

## WiFi再接続失敗時の自動再起動

### 背景
`wifi_manager.c`の自動再接続ループ(reason-201のフルスキャン切替、802.11b/gプロトコルフォールバックを経ても)は、失敗し続けても無限にリトライするだけで、復旧させる手段がRSTしか無いケースがあった。

### 実装
- `wifi_manager.c`の`event_handler`で、`WIFI_EVENT_STA_DISCONNECTED`が**連続で**一定回数(reason-201のフルスキャン切替やb/gフォールバックを挟んでも継続してカウント)を超えたら`esp_restart()`する。
- mrubyから設定可能: `wifi_reconnect_restart_after N`(デフォルト20、`0`で無効化=従来通り無限リトライ)。
- Device roleにはmrubyが無いので、`wifi_manager.c`側に同じデフォルト値(20)をハードコードしたフォールバックを用意(`power_manager.c`と同じweak externパターン)。
- `main/mruby_scripts/default.rb`にコメントアウトの設定例を追加。

### 影響範囲
Host/Device両ロールとも対象(`wifi_manager.c`は両ロール共通)。両ロールともビルド・リンク確認済み。

### 未検証
実機での動作確認(意図通りのタイミングで再起動されるか)。
