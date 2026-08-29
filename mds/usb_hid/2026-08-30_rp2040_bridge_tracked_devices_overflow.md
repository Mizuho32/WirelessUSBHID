# RP2040 bridge: tracked_devicesテーブル溢れで9ボタンマウスが起動順序依存で無反応になる

[[2026-08-23_rp2040_as_host_bridge_plan]]で導入した「再アナウンス」機構(`rp2040_host_bridge.ino`の`tracked_devices[]`/`REANNOUNCE_INTERVAL_MS`)のキャパシティ不足が原因のバグを見つけて直した。

## 症状

ハブ経由で5ボタンマウス(8byte)・キーボード(8byte)・9ボタンマウス(7byte)を接続。

- **RP2040→ESP32の順に電源が入る(またはESP32だけRST)と、5ボタンマウスとキーボードしか反応しない**。9ボタンマウスは無反応
- RP2040側のraw report(`BRIDGE_DEBUG`のSerial2ダンプ)は9ボタンマウス含め全デバイスの入力を正常に受け続けている ⇒ USB Host側(RP2040)は正常
- **RP2040をRSTすると(ESP32は起動したまま)全デバイスが復活する**

## 原因

`tuh_hid_mount_cb()`(`rp2040_host_bridge.ino`)は、USB列挙が実際に起きた瞬間に一度だけ、無条件に`send_frame(BRIDGE_MSG_MOUNT, ...)`を送る。ESP32が既に起動してUARTを読んでいれば、これでどのデバイスも即座に認識される。

問題は「ESP32が後から(RP2040が既に全デバイスを列挙し終えた後に)起動/再起動したケース」。この場合、上記の一度きりのMOUNTフレームはESP32がまだ起動してない間に送られてしまっているので届かない。ESP32がその後デバイスの存在を知る唯一の手段は、`loop()`内の定期再アナウンス(`REANNOUNCE_INTERVAL_MS`=2000ms毎、`tracked_devices[]`に入ってるデバイスのMOUNTを送り直す)だけ。

`tracked_devices[MAX_TRACKED_DEVICES]`は**4個**しか枠が無かった。一方この接続構成のHIDインターフェース数は:

- 5ボタンマウス: メインインターフェース 1つ
- キーボード: Boot Keyboard・Vendor Page(0xFF60)独自IF・Report ID多重化の複合IF、で**3つ**([[2026-08-22_consumer_control]]参照)
- 9ボタンマウス: メインマウスIFに加え、マクロキー用キーボードIF・恐らくConsumer Control IF・場合によってはベンダーIF、で**最大4つ程度**([[2026-08-22_9buttons_mouse]]の調査参照)

合計で5〜8インターフェース、4枠を確実に超える。`alloc_tracked_device()`は満杯なら`NULL`を返すだけで、呼び出し側(`tuh_hid_mount_cb()`)は**エラーも出さず黙って無視**していた。列挙順(電源投入時のタイミング依存)で後に列挙されたインターフェースがこの「黙って弾かれる」枠に当たり、それがたまたま9ボタンマウス関連のインターフェースだった、という話。

「RP2040をRSTすると直る」のは、RSTで再列挙が起きた瞬間はESP32が既に起動済みなので、`tracked_devices`テーブルの容量に関係なく最初の無条件MOUNTフレームがそのまま届くため(再アナウンス経路を経由する必要が無い)。

## 対処

`rp2040_host_bridge.ino`:

- `MAX_TRACKED_DEVICES`を4→8に増量(実際のインターフェース数に対して余裕を持たせた値。1枠あたり`desc[512]`を含むため8枠でも約4KB、RP2040の264KB SRAMからすれば無視できるサイズ)
- `tuh_hid_mount_cb()`の`alloc_tracked_device()`失敗時(テーブル満杯)に`DEBUG_PRINTF`で警告するよう追加(今まで完全に無音で診断しづらかった)

## 未検証

実機での再現テスト(この構成での電源投入順序を変えても9ボタンマウスが反応するか)はユーザー側で実施予定。ビルド確認もarduino-cli等がこの環境に無いため未実施(構文は目視確認のみ)。
