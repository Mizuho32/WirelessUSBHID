# ESP-IDF側の既知issue調査(9ボタンマウス3バイート切り詰め問題)

`mds/2026-08-22_rp2040_host_check.md`でRP2040クロステストにより、9ボタンマウス(Maxxterドングル)の3バイート切り詰め問題は**ドングル側ではなくESP32-S3のUSBホストコントローラか`espressif/usb_host_hid`ドライバ(ESP-IDF側)固有の問題**と確定した。現在`IDF5.5.1`が最新ではないので、既知issue・アップデートで直る可能性を軽く調査した。

## 調査結果

### 有力な手がかり: DWC_OTGのショートパケット特有の面倒さ
Espressif公式のメンテナー向けドキュメント(USB Host Maintainers Notes, DWC_OTG Controller)に以下の記述がある:

> Interrupt IN転送でショートパケット(MPSより短いデータ)を受け取ると、そのQTDに`IOC`(interrupt on complete)ビットが立ってなくても**追加の割り込みが発生する**。しかもチャンネルは自動停止しない。ソフトウェア側がこの追加割り込みを見て**手動でチャンネルを止め、QTDリストに残ってる分をキャンセルする**必要がある。

さらに:
> Due to the interrupt transfer peculiarities, it may be easier for software to allocate a QTD for each transaction instead of an entire transfer.

(Interrupt転送特有の面倒さがあるので、1トランザクションごとに専用のQTDを割り当てた方が楽、とEspressif自身がコメントしてるレベル)

これは「デバイスが短いパケットを送ってきた時のホスト側の扱いがDWC_OTG特有に面倒」ということを示していて、今回の「7バイート期待してるのに3バイートしか届かない」という症状のクラスとしては十分あり得る話。ただし、**これはピッタリ今回のバグと一致すると確定した記述ではなく、一般論としての注意点**。

### コンポーネントのバージョン確認
- `espressif/usb_host_hid`(managed component): 現在の最新は1.2.0。このプロジェクトの`managed_components/espressif__usb_host_hid`も既に1.2.0 → **ここは古さの問題ではない**
- 低レベルの`usb_host`(DWC_OTGドライバ本体)は**ESP-IDF本体にバンドルされている**ため、component managerでは単独アップデートできない。試すには**ESP-IDF自体(5.5.1→5.5.2や最新)をアップグレードするしかない**

### 見つけたが症状が一致しなかったissue
- [#10538 USB HID host trouble with optical mouse ICs](https://github.com/espressif/esp-idf/issues/10538) — PAW352x系光学マウスが接続1分後にクラッシュ。レポート長の話ではない
- [#14244 USB HID Host - Some USB devices not detected on boot](https://github.com/espressif/esp-idf/issues/14244) — 起動時のタイミングの問題で、レポート長とは無関係

### 参考リンク
- [USB Host Maintainers Notes (DWC_OTG Controller) - ESP32-S3](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/usb_host/usb_host_notes_dwc_otg.html)
- [espressif/usb_host_hid • ESP Component Registry](https://components.espressif.com/components/espressif/usb_host_hid)

## 結論・現状
「ズバリ一致する既知issue」としては見つからなかった。確認するには実際にESP-IDFをアップグレードして再テストするしかなさそうだが、**この件は一旦保留**。

## 追記: 実際にESP-IDF 6.0.2で再テスト → 変化なし

`/opt/esp-idf`に新規インストールされたESP-IDF 6.0.2で`build.host`をクリーンビルドし直して実機確認したが、**症状は変わらず(依然3バイート切り詰め)**。つまりIDFのバージョンが古いことが原因ではなかった — DWC_OTGドライバ自体、複数のIDFメジャーバージョン(5.5.1→6.0.2)にまたがって同じ挙動をしているということになり、その間に何か直った訳ではない。

この時点で否定できた仮説: `wMaxPacketSize`理論、SET_PROTOCOLネゴシエーション理論、ドングル側ファームウェア理論(RP2040クロステストで否定済み)、そして今回のIDFバージョン理論。残るのはESP32-S3のUSBホストハードウェア自体(dwc_otg IP自体のerrata級の何か)という可能性が濃厚だが、これ以上はプロトコルアナライザ級の直接観測無しに確定させるのは難しそう。
