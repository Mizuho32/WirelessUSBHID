# 9ボタンマウス(ワイヤレスドングル)の短いレポート問題

`mds/usb_hid/2026-08-22_9buttons_mouse.md`から分離。あちらでReport ID/Consumer Control対応まで実装した後、実機再テストで「記述子と実際に届くバイト列が噛み合わない」という別の問題が見つかったので、こちらでその調査を追う。

## Consumer Control実装後の再テストで発覚: 記述子と実挙動の食い違い

上記の実装(マウスインターフェースのReport ID 2をConsumer Controlとしてパース)を実機に書き込んで再テストしたところ、想定と違う結果になった。

### 「本物のキーボードインターフェース」の中身は完全に素のBoot Keyboard
`Keyboard connected: Boot Protocol`と出ていたもう1つのインターフェースのReport Descriptorを取得(65バイト):
```
05 01 09 06 a1 01 05 07 19 e0 29 e7 15 00 25 01
75 01 95 08 81 02 95 01 75 08 81 01 95 05 75 01
05 08 19 01 29 05 91 02 95 01 75 03 91 01 95 06
75 08 15 00 26 f1 00 05 07 19 00 2a f1 00 81 00
c0
```
Report IDが一切無い、教科書通りの標準Boot Keyboard記述子(modifier 1byte + reserved 1byte + keycode配列6byte)。何も束ねられていない。つまりCtrl+C/Vは本当にただの「Ctrl修飾+Cキー/Vキー」として送られてるだけで、特別な仕組みは無い。

### マウスインターフェース側: 実際に受信する生バイトが記述子と全く噛み合わない
実際に各操作を行った時の生レポート(3バイト固定、`Mouse raw report`ログより):

| 操作 | 生バイト | 解釈 |
|---|---|---|
| 左クリック | `01 00 00` | buttons bit0 = 1 |
| 右クリック | `02 00 00` | buttons bit1 = 2 |
| ホイールクリック(中クリック) | `04 00 00` | buttons bit2 = 4 |
| 戻るボタン | `08 00 00` | buttons bit3 = 8 |
| 進むボタン | `10 00 00` | buttons bit4 = 16 |
| カーソル移動(上/下/左/右) | `00 00 ff`(or fe) / `00 00 01` / `00 ff(fe,fd) 00` / `00 01 00` | buttons=0, X=byte1(signed 8bit), Y=byte2(signed 8bit) |
| Volume Up | `e9 00 00` | 0xE9 = 本物のHID Consumer Usage「Volume Increment」 |
| Volume Down | `ea 00 00` | 0xEA = 本物のHID Consumer Usage「Volume Decrement」 |
| ホイール上/下(スクロール) | `00 00 00`(上下とも同じ) | 区別できる情報が無い |

これは記述子が宣言してる内容(Report ID 1固定・16bit X/Y・Report ID 2でConsumer Control)と全く違う:
- **Report IDバイトが実際の通信には一切乗っていない**(右クリック`02 00 00`がbyte0=0x02から始まってる時点で、`85 01`/`85 02`という宣言と矛盾する — Report IDが本当に乗っているなら先頭は常に一定の値になるはず)
- 通常のボタン/移動は`buttons`バイトが最大でも0x1F(5ボタン全部)までしかいかないのに対し、Volume Up/Downだけ0xE9/0xEAという明らかに大きい値が同じ位置に来る。つまりこのマウスは実際には**「先頭バイトが小さければbuttons+dx+dy(Boot Protocol風の3byte)として、大きければそこにConsumer Usage IDの下位バイトを直接埋め込む」という独自の間に合わせ処理をしていて、記述子通りには全く動いていない**らしい
- ホイールのスクロールは、この3バイト経路には反映されていない(上下とも`00 00 00`)

これによって「マウス全体が全く反応しない(カーソル・クリック含む)」の原因も判明: 今のコードは記述子通りのReport Protocol(Report ID 1、16bit X/Y前提)で読もうとしていたので、実際に届く3バイトの中身とは場所・意味とも噛み合っておらず、ほぼ常に0を抽出していた。

### 疑問: 普通のPC(ドライバ無し)ではこのマウスは全機能(ホイール含め)使えるのに、なぜ?
これは重要な指摘。普通のOSの汎用HIDマウスドライバは記述子を素直に信じてパースするはずなので、記述子と実挙動がこれほど食い違っていたら普通のPCでもまともに動かないはず。ここがまだ説明できていない。考えられる可能性:

1. こちらが明示的に`hid_class_request_set_protocol(REPORT)`を要求したことで、このチップが何か想定外の状態に入ってしまっている(要求が実際に成功したかどうかを一度もチェックしていなかった)
2. 普通のOSは何か違う手順(要求のタイミング・順序、あるいはそもそもRequestを送らない)で、このチップの「正しい」動作を引き出せている
3. まだ見えていない別の経路がある

### 追加した調査用ログ
`handle_driver_connected()`のマウス分岐に、`hid_class_request_set_protocol()`の戻り値と、直後に`hid_class_request_get_protocol()`で読み返した実際のプロトコル状態をログ出力するようにした(`usb_host_task.c`、`//* ... //*/`のトグルコメントで、既存の他の調査ログと同じ方式)。ビルド確認済み(Host role)。

## Linux側(`usbhid-dump`)で実機の生バイトを確認

`SET_PROTOCOL(Report)=ESP_OK, GET_PROTOCOL=ESP_OK (value=1)` — こちらの要求は失敗していない、デバイス自身も「Report Protocolのつもり」と認識している。それでも実際に届くデータは記述子と噛み合っていなかったので、Linux機に挿して`usbhid-dump -e stream`で直接観測した(`data/usbhid-dump.txt`)。

### デバイスの正体
`lsusb`より: **248a:8579 "Maxxter Wireless Receiver"** — マウス本体ではなく、**2.4GHz無線レシーバー(ドングル)**。マウス⇔ドングル間は独自の無線プロトコルで、ドングルがそれをUSB HIDとしてPCに中継している構成。USBインターフェースは2つ(Mouse, Keyboard)で、**どちらもエンドポイントの`wMaxPacketSize = 8 bytes`**(`lsusb -v`で確認)。

### 実機PCでの生バイト(抜粋、`data/usbhid-dump.txt`より)
| 操作 | 生バイト(Report ID込み、正しい長さ) |
|---|---|
| 左クリック | `01 01 00 00 00 00 00`(7バイト: ID=1,buttons=1,X=0,Y=0,wheel=0) |
| ホイール上 | `01 00 00 00 00 00 01`(wheel=+1) |
| ホイール下 | `01 00 00 00 00 00 FF`(wheel=-1) |
| 右クリック | `01 02 00 00 00 00 00` |
| 進む | `01 10 00 00 00 00 00` |
| Ctrl+C | `01 00 06 00 00 00 00 00`(8バイト、別インターフェース=Boot Keyboard、Report ID無し: modifier=Ctrl(1), key=C(0x06)) |
| Volume Up | `02 E9 00`(3バイト: ID=2, usage=0x00E9=Volume Increment) |
| Volume Down | `02 EA 00`(usage=0x00EA=Volume Decrement) |

**このデバイス(ドングル)は記述子通りに完全に正しく動いている。** マウスReport ID 1は7バイート丸ごと届いており、ホイールも符号付き±1としてちゃんと乗っている。Consumer Control(Report ID 2)も3バイート、本物のUsage ID(0xE9/0xEA)で届いている。前回の「記述子と実挙動が食い違ってる」という仮説は誤りだった — 食い違っているのは**ESP32側が受け取ってるバイト列**の方。

比較すると、ESP32側で観測した値は、実際の正しいバイト列から**先頭のReport IDバイトが欠け、後ろも切り詰められたもの**に見える(例: 進むボタン、実際は`01 10 00 00 00 00 00`〈7バイト〉、ESP32側は`10 00 00`〈3バイト〉)。

### `wMaxPacketSize`理論は否定
ESP-IDFの`usb_host_hid`ドライバは転送バッファを`iface->ep_in_mps`(エンドポイントの`wMaxPacketSize`)ぴったりに確保し(`hid_host.c`の`hid_host_interface_claim_and_prepare_transfer()`)、1回の転送でその分だけ要求する(`in_xfer->num_bytes = iface->ep_in_mps`)。**もし`wMaxPacketSize`がマウスの7バイートより小さければ、1回のUSB転送に収まらず分割されたうちの最初の断片しか見えない**、という仮説を立てていたが、`lsusb -v`で確認した実際の`wMaxPacketSize`は**8バイート**(両インターフェースとも) — 7バイートも3バイートも1回の転送に余裕で収まるサイズなので、**この仮説は否定された**。

### 現状の理解
- ドングル自体のファームウェアは正しい(PCで確認済み)
- ESP32側のバッファサイズも十分(`wMaxPacketSize=8`)
- それでもESP32側では3バイートしか届いていない = **ESP32-S3のUSBホストコントローラと、このドングルの組み合わせ特有の何か**(ポーリングタイミング、あるいは`usb_host_hid`ドライバの別の箇所の問題)である可能性が高い。プロトコルアナライザ無しでここから先を確定させるのはかなり難易度が高い

### 検討したが見送った案: `iface->ep_in_mps`をESP32側でも独自に確認
`usb_host_get_active_config_descriptor()` + `usb_parse_endpoint_descriptor_by_address()`(`usb/usb_helpers.h`)を使えば、ESP32側でも標準USBエンドポイント記述子から`wMaxPacketSize`を独自に読み直せる。ただしこれには**独自の`usb_host_client_handle_t`を新規に登録する必要がある**(今の`usb_host_task.c`は`espressif/usb_host_hid`ドライバに全てのクライアント登録を任せていて、生の`usb_host`低レベルAPIを直接呼んでいない) — 見積もっていたより低コストではない。しかも`lsusb`で外部から`wMaxPacketSize=8`は既に確定済みなので、**ESP32側で同じ値を再確認する意義は薄い**(`USB_EP_DESC_GET_MPS()`は単純なフィールド抽出で、ESP-IDF全体で広く使われてる実績のあるコードなので、ここが間違っている可能性は低い)。

そのため、この特定の確認は**費用対効果が見合わないと判断し、見送る**。

## 実装: 観測された長さを信用するフォールバック

mdをまとめてる最中に気づいた: ESP32側で観測してた3バイート(`buttons,00,00`のような形)は、**まさに`hid_mouse_input_report_boot_t`(Boot Protocol、Report ID無し、buttons(1)+dx(int8)+dy(int8)=3バイート)そのものの形**。`GET_PROTOCOL`は「Report Protocolのつもり」と返してきていたが、**実際に送られてくるデータの形は終始Boot Protocol形式**だったと考えれば、これまでの観測が全て説明できる:

- 左クリック`01 00 00`〜進む`10 00 00`〜カーソル移動`00 00 ff`等: 素直な`buttons,dx,dy`
- Volume Up`e9 00 00`/Down`ea 00 00`: 本来`02 E9 00`/`02 EA 00`(Report ID 2 + usage)のはずが、**こちらも先頭のIDバイトが欠けた状態**で届いている

つまりこのドングルは、`GET_PROTOCOL`の返事に関わらず、**「先頭バイトが小さければbuttons+dx+dyとして、大きければそこにConsumer Usage IDの下位バイトを直接埋め込む」という一貫した挙動をしている**、と考えれば全部説明がつく(前々回の仮説を、根本原因の理解込みで裏付け直した形)。

### 修正
`hid_host_interface_callback()`のマウス分岐に、**ネゴシエートされたプロトコルより、実際に観測された長さを信用する**フォールバックを追加(`usb_host_task.c`):

1. 観測された長さが`sizeof(hid_mouse_input_report_boot_t)`(3バイート)、かつ`sub_class==BOOT_INTERFACE`の場合:
   - 先頭バイトがこのマウスの`button_count`から導ける最大値(5ボタンなら0x1F)を超えていたら → Consumer Usage IDとして`(data[0], data[1])`を直接16bit LEに組み立てて送る(`hid_extract_field()`は使わない — Report IDバイトが実在する前提の仕組みなので、ここでは前提が崩れている)
   - 超えていなければ → 素直にBoot Protocolのマウスレポートとして処理
2. どちらにも該当しない場合は、既存の(Report ID込みを前提とした)判定にフォールバック — 通常の行儀の良いデバイスはこれで変わらず動く

既知の限定事項: 「Boot Interface対応マウスで、かつConsumer Controlセレクタも同じインターフェースに束ねていて、かつそのReport IDがたまたま小さい値」という組み合わせのデバイスがもしあれば誤判定しうるが、現状このプロジェクトで確認できているのはこの1台のみで、そのケースには該当しない。

ビルド確認済み(Host role、警告0件)。ホイールは依然としてBoot Protocol形式に存在しない情報なので対応不可のまま。

### 実機確認: 動いた(ホイール除く)
カーソル移動・左右中クリック・進む/戻るは実機確認済み。Volume Up/Downは最初「離してもUp/Downされっぱなし」になる問題があった — リリース時の生バイト`00 00 00`が「ボタン無し・移動無し」の普通のマウスレポートと区別つかず、Consumer側の「離した(usage_id=0)」が送られていなかったのが原因。全部0の3バイートが来た時は、マウスの無入力扱いと**併せてConsumer側にも`usage_id=0`(離す)を送る**ように修正(0はどちらの意味でも実害の無い値なので、両方送って問題ない)。修正後、Volume Up/Downも含めて動作確認済み。

ホイールのみ、Boot Protocol形式にそもそも情報が存在しないため未対応のまま。

## ホイール対応に向けた調査: なぜ3バイートしか届かないのか

ホイールを直すには、根本原因(なぜESP32側では7バイートのはずが3バイートしか届かないのか)を解決する必要がある。`wMaxPacketSize=8`(バッファは十分)なので、単純に「バッファが小さい」という話ではない。試したこと:

### `hid_class_request_set_idle()`をマウスにも追加 → 効果無し
今までキーボードにだけ呼んでいて、マウスには呼んでいなかった(`hid_class_request_set_idle(hid_device_handle, 0, 0)`)。「安いチップがこれを送らないとフルのレポート生成モードに入らない」という仮説で試したが、**効果無し**(ホイールを回してもログの反応変わらず、3バイートのまま)。

### 次の一手: Wireshark + usbmon
ESP32側でこれ以上コードをいじって当てずっぽうを続けるより、**Linux機でこのドングルに接続する際、OS(カーネルのUSB/HIDスタック)が実際にどんな順番でどんなコントロールリクエスト(`SET_PROTOCOL`, `SET_IDLE`, `GET_REPORT`等)を送っているかをWireshark + usbmonで直接キャプチャする**方が確実。ESP32側の`usb_host_task.c`が送ってる手順と比較して、「OSはやってるがこちらはやっていない手順」を特定できれば、それが実は根本原因を突き止める近道になるかもしれない。

### 次にやること
1. Wireshark + usbmonで、このドングルの接続直後のUSB制御転送シーケンスをキャプチャ
2. ESP32側(`usb_host_task.c`のUSB Hostタスク起動〜`handle_driver_connected()`)が実際に送っているシーケンスと比較
3. 差分があれば、それを再現するようにESP32側の初期化シーケンスを調整して再テスト

キャプチャ用のスクリプト(tcpdump + usbmon、Wireshark GUI無しでも解析可能)は`wireshark_9btn_mouse/`に用意した。手順は`wireshark_9btn_mouse/README.md`参照。

## Linux実機キャプチャの解析結果と、SET_PROTOCOL除去実験(失敗)

`wireshark_9btn_mouse/capture.sh`で実際にキャプチャ(`capture.pcap`, 14MB, bus全体・約16秒)。`tshark`(`sudo pacman -S wireshark-cli`でインストール)でデバイスアドレス115(このドングル)の列挙シーケンスを抽出:

```
GET_DESCRIPTOR(DEVICE) → GET_DESCRIPTOR(CONFIGURATION)x2(9byte→59byte) → GET_DESCRIPTOR(STRING)x3
  (言語リスト, "Wireless Receiver", "Telink" ← 製造元。安価な2.4G HID用チップの定番ベンダー)
→ SET_CONFIGURATION
→ SET_IDLE(interface0=マウス, ReportID=0, Duration=0)
→ GET_DESCRIPTOR(HID Report, interface0, 148byte ← 既知のマウス記述子と一致)
→ SET_IDLE(interface1=キーボード)
→ GET_DESCRIPTOR(HID Report, interface1, 65byte ← 既知のキーボード記述子と一致)
→ SET_REPORT(Output, interface1 ← キーボードのLED初期化、Linuxの標準動作)
→ 以降はinterrupt pollingのみ
```

**重要な発見: LinuxはこのドングルにSET_PROTOCOLを一度も送っていない。** ESP32側は明示的に`hid_class_request_set_protocol(HID_REPORT_PROTOCOL_REPORT)`を送っている。HID仕様上デバイスは電源投入時デフォルトでReport Protocolのはずなので、「SET_PROTOCOL要求自体がこのTelinkチップのファームウェアの何かおかしな状態を誘発しているのでは」という仮説を立てた。

### 実験: マウス側のSET_PROTOCOL(Report)呼び出しを削除 → 効果無し、revert済み

`usb_host_task.c`の該当箇所(`SET_IDLE`は残し、`SET_PROTOCOL(Report)`だけ削除)を変更してビルド・実機テストしたが、**依然として3バイートのまま**(ログも3 bytes)。つまりSET_PROTOCOLのネゴシエーション自体は今回の切り詰め現象の原因ではなかった。実害が無い変更ではあるものの、他のマウス(将来的にSET_PROTOCOLを本当に必要とする行儀の良くないデバイス)への互換性リスクだけが残るので、この変更は**revertして元の明示的SET_PROTOCOL呼び出しに戻した**(ビルド確認済み)。

なお、この実験と同じタイミングでVolume Up時の挙動がおかしくなった(前回直したはずの「離しても押しっぱなし」的な症状)という報告があったが、この変更(SET_PROTOCOLの有無)とは論理的に無関係な箇所(該当のstuck-held修正は`hid_host_interface_callback()`側で、今回一切触っていない)なので、**ドングル側の無線受信ロスなど別要因の可能性が高い**。revert後に再現するか要確認、優先度低。

### 現状の理解(更新)
- `wMaxPacketSize`理論: 否定済み
- SET_PROTOCOLネゴシエーション理論: **否定済み**(今回)
- 残る説明: ESP32-S3のUSBホストコントローラ(dwc_otg)とこのドングルの間の、リクエスト内容ではなく**タイミング/バスの物理層挙動**に起因する何か。これを特定するには、ESP32⇔ドングル間を流れる実際の通信をキャプチャする必要があるが、usbmonはLinuxホスト側でしか使えないため、**ESP32が関与する通信は原理的にこの方法ではキャプチャできない**(usbmonで見れるのは今回のようにLinux PCとドングルの組み合わせだけ)。ハードウェアUSBプロトコルアナライザ(Beagleなど)が無い限り、これ以上の直接観測は難しい。
- ホイール対応は一旦保留とし、既知の限定事項として受け入れる方向が妥当かもしれない。

### 最終確認: GET_PROTOCOLはvalue=1(Report)を返す — それでも3バイート

revert後の実機ログ: `SET_PROTOCOL(Report)=ESP_OK, GET_PROTOCOL=ESP_OK (value=1, 0=Boot 1=Report)`。つまりこのドングルは「Report Protocolのつもり」と自己申告し、こちらのSET_PROTOCOL要求にも正常にACKしている——**にも関わらず、実際に送ってくる生レポートは相変わらず3バイート(Boot Protocol形状)のまま**。これは「デバイスの自己申告するプロトコル状態」と「実際のレポート生成パイプライン」が内部的に完全に食い違っている、というこのドングルのファームウェア(Telinkチップ)側の一貫した不具合であることの最終確認になった。ESP32側・ホスト側からのリクエスト内容をどう変えても解決しない理由がこれで裏付けられた。

一区切りとして、このドングルの**ホイール非対応は既知の限定事項として受け入れる**。

(余談: この確認と同時に「Volume Up/Downがまた不安定」との報告があったが、直前の変更(SET_PROTOCOL revert)はこの症状のロジックに触れていないため、ドングル側の無線受信ロス等の間欠的な問題である可能性が高い。優先度低の別問題として保留。)

### 訂正: 上記「ドングル側ファームウェアの不具合」という結論は誤りだった

`mds/usb_hid/2026-08-22_rp2040_host_check.md`でRP2040(TinyUSB Hostスタック、ESP32とは別のホストコントローラ・別のソフトウェアスタック)に同じドングルを繋いだところ、**何の工夫もなく最初から7バイート丸ごと(ホイール込み、Report ID込み)正しく届いた**。つまりこのドングルのファームウェアは実は最初から正しく動いており、上で「ドングル側ファームウェアの一貫した不具合」と結論づけたのは誤り。真の原因はESP32-S3のUSBホストコントローラか`espressif/usb_host_hid`ドライバ(ESP-IDF側)固有の問題だった。詳細・実機ログは`mds/usb_hid/2026-08-22_rp2040_host_check.md`参照。
