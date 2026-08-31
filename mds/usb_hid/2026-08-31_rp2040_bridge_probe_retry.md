# RP2040ブリッジprobeのリトライ/タイムアウト設定化

## 背景
ESP32とRP2040は同じ電源で同時に起動することが多く、RP2040側の起動(USB Hostスタック初期化含む)が遅れると、`usb_host_rp2040_bridge_probe()`の固定800ms窓を待っている間にRP2040の最初のハートビートが間に合わず、実際にはRP2040が繋がっているのに「検出できない」= HID操作が効かないことがあった。

## 実装
- `usb_host_rp2040_bridge_probe()`(`main/usb_host_rp2040_bridge.c`)を、固定800msの単発待ちから**試行回数×タイムアウトのループ**に変更。試行ごとにログ出力。
- mrubyから設定可能(`main/mruby_filter.c`/`.h`):
  - `rp2040_bridge_probe_retries N`(デフォルト3)
  - `rp2040_bridge_probe_timeout_ms N`(デフォルト800、従来と同じ)
  - 未設定ならデフォルトのまま(3回×800ms、最悪ケースで+約1.6秒の追加起動遅延)。1回未満・50ms未満は下限にクランプ。
- `main/mruby_scripts/default.rb`にコメントアウトの設定例を追加。

## 影響範囲
Host role限定(`usb_host_rp2040_bridge.c`/`mruby_filter.c`はHOST SRCSのみ)。Device roleは無関係、ビルド確認済み。

## 未検証
実機での再現性(そもそもの間欠検出失敗が、デフォルト値(3回)で十分解消するか)。
