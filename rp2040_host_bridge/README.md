# RP2040 host bridge

これが何で何のためかは`mds/usb_hid/2026-08-23_rp2040_as_host_bridge_plan.md`参照(MAX3421Eの代替/置き換え——RP2040自身がUSB Hostの役割を担い、デコード済みのHIDイベントをUART経由でESP32-S3に転送する)。

## ビルド/書き込み

`rp2040_hello_world/`/`rp2040_host_check/`と共通で`bin/build_flash_rp2040.sh`を使う(ボード = Raspberry Pi Pico):

```
bin/build_flash_rp2040.sh rp2040_host_bridge tinyusb_host build
bin/build_flash_rp2040.sh rp2040_host_bridge tinyusb_host flash
```

`flash`はPicotool経由でアップロードするので、ボードが事前にBOOTSELモードに入っている必要がある(BOOTSELを押しながら接続/リセット)。

## 配線

- USB Hostポート: `rp2040_host_check/`と同じVBUSジャンパー構成(`mds/usb_hid/2026-08-22_rp2040_host_check.md`参照——ボード自体のVBUSピンはデバイス専用給電用にダイオードでブロックされているので、ドングルに電源を認識させるには別途5VをVBUSへ直結するジャンパーが必要)。
- ESP32とのリンク: `Serial1`(多くのrp2040ボードでデフォルトGP0=TX、GP1=RX)を、ESP32-S3側のブリッジ用UARTピン(`esp32-kvm-ip/main/usb_host_rp2040_bridge.c`内の`BRIDGE_UART_TX_PIN`/`BRIDGE_UART_RX_PIN`)に接続する——両ボード間でTX/RXをクロス、GNDも共通化すること。どちらのピン番号も実配線に合わせて調整すること(ESP32側コードのピンはプレースホルダで、MAX3421Eの時と同様、配線後に確定させるもの)。

## デバッグ出力: `BRIDGE_DEBUG`(Serial2、RP2040の2つ目のUART)

`Serial1`(UART0)はESP32向けのバイナリブリッジプロトコル専用なので、`rp2040_host_check.ino`のような人間向けログには使えない。RP2040にはもう1つ独立したハードウェアUART(UART1)が空いているので、スケッチ冒頭の`BRIDGE_DEBUG`を`1`にすると、`Serial1`上の実際のブリッジ通信を妨げることなく、`Serial2`にmount/unmount/生レポートのダンプ(`rp2040_host_check.ino`と同じ形式)を出力するようになる。デフォルトピンはGP4=TX/GP5=RX(arduino-picoの`Serial2`デフォルト)——実配線と違う場合は`.begin()`前に`Serial2.setTX()`/`setRX()`で変更すること。

RP2040ボード自体のUSBコネクタ(ドングルを挿す方)の背後にはUARTは一切無い——チップ本体のUSB D+/D-ピンに直結されているだけなので(`mds/usb_hid/2026-08-22_rp2040_host_check.md`参照)、`BRIDGE_DEBUG`の設定に関わらずHost動作中はデバッグ出力に使えない。

ESP32を繋がずにドングル/TinyUSB Host側だけを単体でデバッグしたい場合は、代わりに`rp2040_host_check/`を使うこと——そちらは引き続き`Serial1`をプレーンテキスト出力に使っている。

## FPS切り分け用ツール: `RATE_MONITOR` / `POLL_CEILING_TEST`

`mds/usb_hid/2026-08-24_rp2040_bridge_fps_investigation.md`の実測案(点1: RP2040自体のレポート受信レート)向け。どちらも出力先は`BRIDGE_DEBUG`と同じ`Serial2`。

- **`RATE_MONITOR`**(デフォルト1=有効): `tuh_hid_report_received_cb()`が呼ばれた回数を1秒ごとに`Serial2`へ`[rate] N reports/sec`として出力し続ける常時カウンタ。`BRIDGE_DEBUG`の生レポートダンプと違い、カウンタのインクリメント+1秒に1回のprintfのみなので、タイミングを乱してクラッシュを隠していた例の生ダンプほどの負荷にはならない見込み(が、要注意)。
- **`POLL_CEILING_TEST`**(デフォルト0=無効): `setup()`内で一度だけ走る対話的な計測。有効にすると起動後`Serial2`に「そのまま待て」→「今からN秒間マウスを動かし続けて」と出て、その間のピーク瞬間レート・平均レートを計測して表示してから通常動作に戻る。`setup()`をブロックして実際にマウスを動かす必要があるので、ベンチ目的で意図的に有効にする時だけONにすること。

どちらも別コア(RP2040のcore1、arduino-picoの`setup1()`/`loop1()`)は使っていない——TinyUSBの`tuh_*`呼び出しは`USBHost.task()`を回している1つのコア/タスクからしか触られない前提なので、カウンタを見るためだけに複数コアに跨いだロックを持ち込む価値はないと判断し、既存の`loop()`一回ごとのチェックで済ませている。
