# ワイヤレスUSB HID

USB HID機器(複数、HUB経由)をワイヤレス化したい。

## 機能

- **Device role** (`esp32-kvm-ip/`, `KVM_ROLE=DEVICE`): ESP32-S3がUSB HIDデバイスとしてTarget PCに接続し、WiFi/UDPで受けたキーボード・マウス入力を中継
- **Host role** (`KVM_ROLE=HOST`): 別のESP32-S3が物理USBキーボード/マウス(ワイヤレスドングル含む)をUSB Hostとして読み取り、Device role基板へUDP転送
  - USB Hostバックエンドは2系統: ESP32-S3ネイティブOTG、またはMAX3421E(SPI外付けチップ。ネイティブOTGが一部ドングルで報告を3バイトに切り詰めるバグの回避策)
  - Hub経由の複数デバイス、9ボタンマウス、Consumer Control(メディアキー)に対応
  - `filter_rules.h`でキー/ボタンの変換・フィルタ・ルーティングをカスタマイズ可能
- **server.py** (`esp32-kvm-ip/server/`): Windows/LinuxのPCから直接Device role基板へUDP送信する代替入力元。クリップボード貼り付け機能つき
- 調査・検証用サブプロジェクト: `rp2040_host_check/`(RP2040によるUSB Hostクロステスト)、`wireshark_9btn_mouse/`(USBキャプチャ解析)

## filter/conv/routeの編集場所(Host role)

- `esp32-kvm-ip/main/filter_rules.h`(gitignore、`filter_rules.h.example`をコピーして編集): キー/ボタンの変換・フィルタ
- `esp32-kvm-ip/main/route_rules.h`(gitignore、`route_rules.h.example`をコピーして編集): 同じレポートをUDPにも流すかどうか
- どちらも未作成なら`filter_rules_default.h`/`route_rules_default.h`(素通し)が使われる
- 呼び出し元は`hid_forwarder.c`一箇所(USB Hostバックエンドがmax3421/rp2040_bridgeどちらでも共通)
- **マウスのみ挙動が非対称**(雑仕様、将来リッチ化予定):
  - `filter_mouse_report()`はtype-c向けコピーにしか効かない。UDPには常に生値が乗る
  - `route_mouse_also_udp()`はtype-c接続中にUDPへもミラーするかどうかを、その生値(filter前)で判定する
  - 例: wheelをtype-cから消してUDPだけに残したい → `filter_rules.h`で`*wheel = 0`、`route_rules.h`の`route_mouse_also_udp()`で`wheel != 0 || pan != 0`のときtrueを返す(現在の実装済み設定)
  - keyboard/consumerは非対称化しておらず、従来通り「filter適用後の値をtype-c/UDP両方が見る」まま
- 詳細: `mds/usb_hid/2026-08-21_filter_conv_route.md`, `mds/usb_hid/2026-08-23_filter_conv_router_with_max3421.md`

## debug print類の場所

- `esp32-kvm-ip/main/usb_host_rp2040_bridge.c`: `BRIDGE_RATE_MONITOR`(受信rate/interval統計)、`BRIDGE_MINIMAL_TEST`(WiFi/type-c/dispatch_task抜きの最小構成ビルド)
- `esp32-kvm-ip/main/usb_device_typec.c`: `USB_DEVICE_TYPEC_DEBUG`(送信毎ログ)、`USB_DEVICE_TYPEC_RATE_MONITOR`(wait_for_ready blocking統計)
- `esp32-kvm-ip/main/main_host.c`: `HOST_MINIMAL_TEST`(WiFi/type-c/hid_forwarder抜きの最小構成ビルド)
- `rp2040_host_bridge/rp2040_host_bridge.ino`: `BRIDGE_DEBUG`、`RATE_MONITOR`、`POLL_CEILING_TEST`
- `rp2040_host_check/rp2040_host_check.ino`: `RAW_DUMP`、`RATE_MONITOR`
- いずれも該当ファイル冒頭付近の`#define ... 0`を`1`にして有効化


