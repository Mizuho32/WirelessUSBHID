# Host role: 複数デバイス(Hub、多ボタンマウス)対応

## 経緯
Host roleでfilter/convの動作確認は完了。ただし実運用したい構成で以下の事象が出た:

- **USB-Hub経由で複数デバイス(マウス2台+キーボード1台)を接続**すると`hid-host: No HID device at USB port 1`というログが出る
- **9ボタンマウス**を直接挿した場合の挙動も気になっている

以下、原因調査結果。

## 調査結果

### `No HID device at USB port 1`の正体
このログは`espressif/usb_host_hid`コンポーネント自身が出しているもの(`managed_components/espressif__usb_host_hid/hid_host.c:524`):

```c
if (is_hid_device) {
    ...
} else {
    usb_host_device_close(s_hid_driver->client_handle, dev_hdl);
    ESP_LOGW(TAG, "No HID device at USB port %d", dev_addr);
}
```

- ログの`%d`は実は「USBポート番号」ではなく**USBデバイスアドレス(`dev_addr`)**。メッセージの文言がやや紛らわしいが、コード上はデバイスアドレス。
- `hid_host_device_init_attempt(dev_addr)`は、そのUSBアドレスのデバイスのコンフィグディスクリプタを見て「HIDインターフェースが1つも無い」場合にこの警告を出すだけ — **HIDインターフェースが無いデバイスなら普通に起こる、想定内の警告**。

**有力な仮説**: Hubを挿すと、Hub自体が最初にエニュメレートされてUSBアドレス(通常1番目)を割り当てられる。Hub自体のデバイスクラスは当然Hubクラス(0x09)であって**HIDインターフェースは持たない**。なので「USB port 1」= Hub自身のアドレスで、これはHIDインターフェースが無いから警告が出ているだけ、というのが最も自然な読み。マウス2台+キーボードは別のアドレス(2,3,4など)が振られ、そちらは正常にHID接続されている可能性が高い。

**確認方法(実機で見てほしいこと)**:
1. `No HID device at USB port 1`の後に続くログで、他のアドレス向けに`"Mouse connected: Report Protocol ..."`や`"Keyboard connected: Boot Protocol"`(`usb_host_task.c`が出すINFOログ)が出ているか
2. 実際にマウス/キーボードを動かして、Device機側(Target PC)にちゃんと入力が届いているか

これで「Hub自体の警告は無視してよい、実害なし」と確定できるはず。もし他のアドレスでも同様に弾かれていたら、それは別の問題(下記のハードウェア制約など)。

### ハードウェア制約(参考、今回の3台構成では余裕があるはず)
ESP32-S3のUSB-DWC OTGコントローラは**Hostチャンネル数が8**固定(`OTG_NUM_HOST_CHAN=8`、`soc/usb_dwc_cfg.h`)。チャンネルは同時にアクティブな全エンドポイント(Hub自身のステータス割り込みEP + 各デバイスの割り込みIN EP等)で共有される。マウス2台+キーボード1台+Hub自身で概算4チャンネル程度なので、8チャンネルの範囲内で収まるはず — チャンネル枯渇が原因の可能性は低い。デバイスをもっと増やす場合はこの上限を意識する必要がある。

### Kconfig側は既に対応済み
`sdkconfig.defaults`に`CONFIG_USB_HOST_HUBS_SUPPORTED=y`は設定済み。関連する`CONFIG_USB_HOST_HUB_MULTI_LEVEL`(複数Hub同時対応)はHub対応を有効にするとデフォルトでy。追加のKconfig変更は今のところ不要そうに見える。

### 9ボタンマウスについて
2層の制限がある:

1. **Host側パーサー(`hid_report_parser.h`)**: `HID_MAX_BUTTONS`が8。9番目のボタンはUsageが一致せず記録されないが、**パース自体は失敗しない**(X/Y/wheel/panや1-8番ボタンは正常に取れる)。9番ボタンだけ無視される。
2. **Device側USBディスクリプタ(`usb_descriptors.c`)**: そもそも**5ボタン固定**(`bit0=Left, bit1=Right, bit2=Middle, bit3=Back, bit4=Forward`)。Target PCに見せているマウスHIDデバイス自体が5ボタン仕様なので、Host側で仮に6-9番ボタンを拾えても、素通しで転送する経路(生のマウスボタンとして)は今のパイプライン全体として存在しない。

現実的には、6番目以降のボタンを使いたいなら「素のボタンとして転送」ではなく、今回やった戻る/進む→Alt矢印と同じ要領で**filter_rules.hでキーボードショートカット等に変換する**のが今の設計に合ってる(bit3=Back, bit4=Forwardは既にこの経路で使える)。6-9番ボタンを生のマウスボタンとして最後まで届けたいなら、`protocol.h`・Device側`usb_descriptors.c`・Host側`HID_MAX_BUTTONS`の3箇所を揃えて拡張する必要がある(今回はやっていない、別タスク)。

## 解決: 実は`CONFIG_USB_HOST_HUBS_SUPPORTED`が効いてなかった

実機ログ:
```
W (198630) HUB: External Hubs support disabled, Hub device was not initialized
W (198630) hid-host: No HID device at USB port 4
```

「Hub自体の警告で実害なし」という上の仮説は誤り。`External Hubs support disabled`とはっきり出ている通り、**Hub自体が初期化されておらず、配下のデバイスは最初から認識されようがない**状態だった。

原因: `sdkconfig.defaults`に`CONFIG_USB_HOST_HUBS_SUPPORTED=y`を追加したのは今回のHost role実装時だが、プロジェクトの実体である`sdkconfig`(gitignore対象、生成物)はそれより前(Device roleのみだった頃)に一度生成済みだった。**`sdkconfig.defaults`は、`sdkconfig`がまだ存在しない/該当オプションが未設定の場合にだけ反映される**ため、既存の`sdkconfig`には`# CONFIG_USB_HOST_HUBS_SUPPORTED is not set`のまま残っていて、`idf.py build`を何度実行してもこれが自動で有効化されることはなかった。実機で確認して初めて発覚 — 前回のmdで「Kconfig側は既に対応済み」と書いたのは誤りだった。

対処: `sdkconfig`を直接編集して`CONFIG_USB_HOST_HUBS_SUPPORTED=y`(+ 依存の`CONFIG_USB_HOST_HUB_MULTI_LEVEL=y`)を反映。ビルド確認済み(この変更はsdkconfig.hを変えるので今回だけ大きめの再ビルドが発生した — role切り替えの話とは別で、これは1回限りの設定修正)。Device roleも退行なし確認済み。

再度実機テストしてほしい。

## 解決その2: `No more HCD channels available`(実機テストで発覚)

`CONFIG_USB_HOST_HUBS_SUPPORTED`修正後、再テストしたところHub自体は初期化されるようになったが、キーボード直結 + Hub(マウス+キーボード)の構成で新しい問題が発生:

```
I (80518) USBHOST: Keyboard connected: Boot Protocol
I (80519) USBHOST: HID device connected (unsupported, proto 0) - ignoring
I (80520) USBHOST: HID device connected (unsupported, proto 0) - ignoring
E (80593) HCD DWC: No more HCD channels available
E (80594) USBH: EP Alloc error: ESP_ERR_NOT_SUPPORTED
E (80594) USB HOST: EP allocation error ESP_ERR_NOT_SUPPORTED
E (80598) USB HOST: Claiming interface error: ESP_ERR_NOT_SUPPORTED
E (80604) hid-host: hid_host_interface_claim_and_prepare_transfer(780): Unable to claim Interface
E (80612) hid-host: hid_host_device_open(1426): Unable to claim interface
```

### 原因
ESP32-S3のUSB-DWC OTGコントローラは**Hostチャンネル数が8固定**(`OTG_NUM_HOST_CHAN`, `soc/usb_dwc_cfg.h`)。以前「3台構成なら余裕があるはず」と書いたが、これは**1デバイス=1チャンネル**という前提が間違っていた。

実際は`hid_host_device_open()`を呼んだ時点(`hid_host_interface_claim_and_prepare_transfer()`内)でチャンネルが1つ消費される。ところが`usb_host_task.c`の`handle_driver_connected()`は、**キーボード/マウスとして使うかどうかの判定より前に、見つかった全てのHIDインターフェースを無条件で`hid_host_device_open()` + `hid_host_device_start()`していた**。実際のキーボードは大抵、Boot Keyboardインターフェースの他に「メディアキー/コンシューマーコントロール」用の追加インターフェースを持っている(ログの`proto 0, unsupported, ignoring`がまさにそれ)。この**使わないインターフェースにもチャンネルを浪費していた**ため、直結キーボード(1) + Hub自身(1) + Hub配下キーボードのBoot IF(1) + 同メディアキーIF(1) + マウスのBoot/Report IF(1) + マウスの追加IF(あれば1)、という感じで簡単に8チャンネルを使い切ってしまっていた。

### 修正
`handle_driver_connected()`を、**マウスかキーボード(Boot Interface)と判定できたものだけ`hid_host_device_open()`する**ように並び替え。使わないインターフェースはopenすらしない(=チャンネルを一切消費しない)。ビルド確認済み(警告/エラー0件)。

これでチャンネル消費は「実際に使うインターフェースの数」だけに抑えられるはず。それでも8個を超える構成(例: マウス4台とか)なら依然として枯渇しうるので、その場合は追加の対策(例: 同時に使うデバイス数を絞る、Hub自身の1チャンネル消費を踏まえて計画する)が要る。

## 次にやること
1. ~~修正版で再度実機テスト(キーボード直結 + Hub(マウス+キーボード)の組み合わせ)~~ → 実機確認済み、その後さらにConsumer Control対応(`mds/2026-08-22_consumer_control.md`)まで実装
2. 9ボタンマウスの6番目以降のボタンを本当に使いたい場合は、素のボタン拡張(3箇所修正)かfilter_rules.hでのショートカット変換か、方針を決める

## 9ボタンマウスをHubに繋いだ時の`No more HCD channels available`(2026-08-22 追記)

[mds/2026-08-22_9buttons_mouse.md](mds/2026-08-22_9buttons_mouse.md)
