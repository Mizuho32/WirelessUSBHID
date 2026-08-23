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


