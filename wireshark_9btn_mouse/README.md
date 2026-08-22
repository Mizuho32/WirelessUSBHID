# 9ボタンマウス(Maxxterドングル)接続初期化キャプチャ

`mds/2026-08-22_wireless_dongle_short_reports.md`の「次にやること」用のツール。Linux PC上でこのドングルを接続する際にOSが実際に送っているUSB制御転送シーケンスをキャプチャし、ESP32側(`esp32-kvm-ip/main/usb_host_task.c`)の初期化シーケンスと比較するため。

## tcpdumpで十分(Wireshark GUI不要)

Linuxのusbmonカーネルモジュールが出す"USB Linux" linktypeは`tcpdump`が直接デコードできる。GUIのWiresharkや`tshark`が無くてもキャプチャ・簡易解析どちらもできる。

## 手順

1. 初回のみ: usbmonを有効化
   ```
   ./enable_usbmon.sh
   ```
2. ドングルを一旦抜く
3. キャプチャ開始(バス番号は`lsusb`で確認。今のところBus 001)
   ```
   ./capture.sh 1
   ```
4. ドングルを挿し直し、数秒待ってから、マウス移動・クリック・ホイールを2〜3回動かす
5. `Ctrl-C`でキャプチャ終了 → `capture.pcap`ができる

## 解析

### Wireshark GUIがあれば
`capture.pcap`を開いて、フィルタ`usb.device_address == <該当アドレス>`(または`usb.idVendor == 0x248a`)。列挙直後の`SET_CONFIGURATION`/`SET_IDLE`/`SET_PROTOCOL`/`GET_DESCRIPTOR`等のControl転送の並び順を見る。

### GUI無しでも: tcpdumpだけで読む
```
sudo tcpdump -r capture.pcap -v | less
```
`-v`を付けるとControl転送のSETUPステージが`bmRequestType`/`bRequest`/`wValue`/`wIndex`/`wLength`込みで読める。見るべき値:
- `SET_IDLE` = bRequest `0x0a`
- `SET_PROTOCOL` = bRequest `0x0b`
- `GET_DESCRIPTOR` = bRequest `0x06`(wValueの上位バイトが`0x22`ならHID Report Descriptor)
- `SET_CONFIGURATION` = bRequest `0x09`

ドングル再接続直後(タイムスタンプで判別)からこのへんのリクエストが**どういう順番で・何回**発行されているかを、`usb_host_task.c`の`handle_driver_connected()`(と`espressif/usb_host_hid`ドライバ内部)が実際に送っている順番と突き合わせる。

## ファイル
- `enable_usbmon.sh` — usbmonモジュールのロード+debugfsマウント(初回のみ)
- `capture.sh` — キャプチャ本体(tcpdumpラッパー)
- `capture.pcap` — 生成される生キャプチャ(gitignore対象、後述)
