# RP2040 host bridge

これが何で何のためかは`mds/2026-08-23_rp2040_as_host_bridge_plan.md`参照(MAX3421Eの代替/置き換え——RP2040自身がUSB Hostの役割を担い、デコード済みのHIDイベントをUART経由でESP32-S3に転送する)。

## ビルド/書き込み

`rp2040_hello_world/`/`rp2040_host_check/`と共通で`bin/build_flash_rp2040.sh`を使う(ボード = Raspberry Pi Pico):

```
bin/build_flash_rp2040.sh rp2040_host_bridge tinyusb_host build
bin/build_flash_rp2040.sh rp2040_host_bridge tinyusb_host flash
```

`flash`はPicotool経由でアップロードするので、ボードが事前にBOOTSELモードに入っている必要がある(BOOTSELを押しながら接続/リセット)。

## 配線

- USB Hostポート: `rp2040_host_check/`と同じVBUSジャンパー構成(`mds/2026-08-22_rp2040_host_check.md`参照——ボード自体のVBUSピンはデバイス専用給電用にダイオードでブロックされているので、ドングルに電源を認識させるには別途5VをVBUSへ直結するジャンパーが必要)。
- ESP32とのリンク: `Serial1`(多くのrp2040ボードでデフォルトGP0=TX、GP1=RX)を、ESP32-S3側のブリッジ用UARTピン(`esp32-kvm-ip/main/usb_host_rp2040_bridge.c`内の`BRIDGE_UART_TX_PIN`/`BRIDGE_UART_RX_PIN`)に接続する——両ボード間でTX/RXをクロス、GNDも共通化すること。どちらのピン番号も実配線に合わせて調整すること(ESP32側コードのピンはプレースホルダで、MAX3421Eの時と同様、配線後に確定させるもの)。

## デバッグ出力: `BRIDGE_DEBUG`(Serial2、RP2040の2つ目のUART)

`Serial1`(UART0)はESP32向けのバイナリブリッジプロトコル専用なので、`rp2040_host_check.ino`のような人間向けログには使えない。RP2040にはもう1つ独立したハードウェアUART(UART1)が空いているので、スケッチ冒頭の`BRIDGE_DEBUG`を`1`にすると、`Serial1`上の実際のブリッジ通信を妨げることなく、`Serial2`にmount/unmount/生レポートのダンプ(`rp2040_host_check.ino`と同じ形式)を出力するようになる。デフォルトピンはGP4=TX/GP5=RX(arduino-picoの`Serial2`デフォルト)——実配線と違う場合は`.begin()`前に`Serial2.setTX()`/`setRX()`で変更すること。

RP2040ボード自体のUSBコネクタ(ドングルを挿す方)の背後にはUARTは一切無い——チップ本体のUSB D+/D-ピンに直結されているだけなので(`mds/2026-08-22_rp2040_host_check.md`参照)、`BRIDGE_DEBUG`の設定に関わらずHost動作中はデバッグ出力に使えない。

ESP32を繋がずにドングル/TinyUSB Host側だけを単体でデバッグしたい場合は、代わりに`rp2040_host_check/`を使うこと——そちらは引き続き`Serial1`をプレーンテキスト出力に使っている。
