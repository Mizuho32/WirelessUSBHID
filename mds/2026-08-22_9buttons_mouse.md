## 9ボタンマウスをHubに繋いだ時の`No more HCD channels available`

### 9ボタンマウスについて
3ボタンは通常と同じ。残り6ボタンにカスタム機能を割り当てられ、現在(ルートプロジェクト コミット59f531b時付近)はCtrl C/V, Volume, 進む/戻る の機能を割り当てている。
Hub経由でHostに繋ぐと、Ctrl C/Vだけが動作し、残り7ボタンは動作しない。カーソルも動かない。

### 構成
Hub配下: キーボード(メディアキーあり、`mds/2026-08-22_consumer_control.md`で対応済み) + マウス2台。1台目は5ボタンマウス(問題なし)、2台目として9ボタンマウスを挿すと`No more HCD channels available`が発生。

### チャンネル予算の見積もり
定常状態(9ボタンマウス自体に追加のUSBインターフェースが無いと仮定した場合)で持続的にopenされるのは:

- Hub自身: 1
- キーボード Boot Interface: 1
- キーボード Consumer Control Interface: 1(認識成功、`mds/2026-08-22_consumer_control.md`)
- マウス1台目(5ボタン、Report Protocol): 1
- マウス2台目(9ボタン)のメインインターフェース: 1

合計 **5/8**。ボタン数そのものはチャンネル消費に無関係(HID_MAX_BUTTONSやDevice側5ボタン固定は純粋にソフトウェア側のフィールド解釈の話)なので、9ボタンマウスの「メイン」インターフェース1つが5ボタンマウスと同じく1チャンネルしか使わないなら、5/8で全然余裕があるはず。

キーボードのベンダー独自インターフェース(0xFF60、`mds/2026-08-22_consumer_control.md`参照)は「Consumer Controlかもしれない候補」として一時的にopen→即座にclose(認識できなかったので)されるが、これは`usb_host_app_task`が1イベントずつ直列に処理しているので、他のトランジェントなopenと重なることはない(次のCONNECTEDイベントを処理する前に必ずclose済み)。

### つまり
**見積もり上は5/8で全然余裕があるはずなのに枯渇する**、というのが今の理解。これは:

1. 見積もりが外れている(9ボタンマウス自体が、メインのマウスインターフェース以外に**独自のベンダー向けインターフェース(マクロ/DPI設定/RGB制御用など、ゲーミングマウスによくある)を1つ以上持っていて**、それぞれが「Consumer Controlかもしれない候補」としてopen/close(または最悪、誤ってConsumer Controlとして認識されて永続open)される
2. または、closeしたはずのチャンネルが実際にはハードウェア側で即座に解放されていない(タイミング/非同期の問題)
3. または、そもそも見積もりに入れていない何か(Hub自身が新規デバイスのenumeration時に一時的に追加のチャンネルを使う、など)がある

のどれかだと思うが、これは**推測の域を出ない** — 前回、Hub対応やチャンネル枯渇そのものについても最初の仮説が2回とも外れた(`mds/2026-08-22_multi_device.md`の「有力な仮説」節、「ハードウェア制約」節を参照)経緯があるので、今回も実機ログを見ないと確信は持てない。

対策の方向性としては、確認が取れれば以下が有力候補(実装はまだしていない):
- Consumer Control候補のprobeを「同一アドレスに本物のキーボードIF(proto=KEYBOARD, Boot Interface)が既にある場合だけ」に絞る(Consumer Controlはキーボード寄りの機能なので、マウス由来のベンダーIFを毎回probeする必要はそもそも無い)
- チャンネル解放のタイミング/同期を`hid_host.c`のソースで確認する

### 次にやること
1. 9ボタンマウス単体で(キーボード無し、または他のマウス無しで)Hub経由で接続した場合に同じエラーが出るか確認 → 出なければ「合わせ技」の問題、出れば9ボタンマウス単体の構成の問題
2. `mds/2026-08-22_consumer_control.md`でやったのと同じ要領で、9ボタンマウスの各インターフェースのReport Descriptorを取得し、実際に何個・どんなインターフェースを持っているか確認

### デバッグログについて
`ESP_LOGD`にしてみようとしたが、このプロジェクトの`sdkconfig`は`CONFIG_LOG_MAXIMUM_LEVEL=INFO`なので、`ESP_LOGD`は実行時フィルタではなく**コンパイル時に呼び出しごと消える**(`esp_log_level_set()`では救えない)。`CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y`にすると直るが、`sdkconfig.h`が変わるので今回だけ大きめの再ビルドが必要になる — デバッグ目的だけでそれをやるのは重いのでやめ、**`ESP_LOGI`のまま残して、使わない時は手動でコメントアウトする**方針にした(`usb_host_task.c`の`handle_consumer_report()`と`handle_driver_connected()`のproto0分岐)。

### 9ボタンマウスの挙動から見えてきたこと(重要な手がかり)
Hub経由で接続すると **Ctrl C/Vだけ動作し、残り7ボタンは無反応、カーソルも動かない**。これは大きなヒント:

- Ctrl+C/Vが動く ⇒ このマウスは**マクロキー用に別途「キーボードとして振る舞うインターフェース」**を持っていて(Ctrl+C/Vは生のマウスボタンでは表現できないので当然)、それが`is_keyboard`判定(`proto==KEYBOARD && sub_class==BOOT_INTERFACE`)を通って正常にopen/startされている、ということ
- **カーソルすら動かない** ⇒ 本来一番重要なはずのメインのマウスインターフェース(X/Y移動)自体が`open()`に失敗している(チャンネル枯渇のタイミングで運悪く後回しにされた)可能性が高い
- Volume/進む/戻るが動かない ⇒ Volumeは恐らくConsumer Controlインターフェース経由、進む/戻るはメインマウスの追加ボタン経由だが、いずれもチャンネルが足りずopenできていないと考えれば筋が通る

つまりこのマウスは実質「メインマウス + マクロ用キーボードIF + (恐らく)Volume用Consumer Control IF + 場合によってはさらにベンダーIF」という**複数インターフェースを持つ複合デバイス**である可能性が高く、それが`mds/2026-08-22_multi_device.md`で立てた仮説(9ボタンマウス自体が追加インターフェースを持っている)を裏付ける形になっている。実機のReport Descriptorダンプで確定させたい。

## 実機Descriptorの解析(2026-08-22 追記)

直結・単体(Hub無し、他デバイス無し)でこのマウスの`is_mouse`インターフェースのReport Descriptorを取得できた(148バイト)。この解析でわかったことと、HEXのどこを見てそう判断したかを記録しておく。

### 生バイト列
```
05 01 09 02 a1 01 85 01 09 01 a1 00 05 09 19 01
29 05 15 00 25 01 75 01 95 05 81 02 75 03 95 01
81 01 05 01 09 30 09 31 16 01 80 26 ff 7f 75 10
95 02 81 06 09 38 15 81 25 7f 75 08 95 01 81 06
c0 c0 05 0c 09 01 a1 01 85 02 75 10 95 01 15 01
26 8c 02 19 01 2a 8c 02 81 00 c0 05 01 09 80 a1
01 85 03 09 82 09 81 09 83 15 00 25 01 19 01 29
03 75 01 95 03 81 02 95 05 81 01 c0 05 01 09 00
a1 01 85 05 06 00 ff 09 01 15 81 25 7f 75 08 95
07 b1 02 c0
```

### 読み方の要点(HIDのバイト構造)
HID Report Descriptorは「1バイトのアイテムヘッダ + 0/1/2/4バイトのデータ」の羅列。ヘッダの上位4bitが種類(タグ)、下位2bitがデータ長。今回の解析で使った着目点:

1. **`85 xx`(Report ID)がセクションの区切り**: `85`が出るたびに「ここから次の`85`(or `End Collection`)までが、そのIDのレポート」という単位で読む。これが無いと、複数の独立した機能が1つのインターフェースに同居してることに気付けない
2. **`05 xx`(Usage Page) + `09 xx`(Usage)の組み合わせが「これは何のcollectionか」の看板**: 例えば`05 01 09 02` = Generic Desktop / Mouse(普通のマウス)、`05 0c 09 01` = Consumer Page / Consumer Control(メディアキー、前回のキーボード解析と全く同じ看板)、`05 01 09 80` = Generic Desktop / System Control(電源管理系)、`06 00 ff`(3バイト版Usage Page) = ベンダー独自ページ(0xFF00)
3. **`a1 01`〜`c0`のペア(Collection Application〜End Collection)が1つの機能のまとまり**。看板(2)と組み合わせて「このIDはこの種類の機能」と確定する
4. **`81 xx`(Input)と`b1 xx`(Feature)は別物**: Inputは普段のポーリングで流れてくるデータ、Featureは`GET_FEATURE`/`SET_FEATURE`で明示的に要求しないと出てこない設定データ。今回のパーサーは`Input`しか見ていないので、`b1`(Feature)の箇所は最初から解析対象外 — つまりベンダー設定(マクロ登録など)はそもそも見えなくて当然、というのも同時にわかる

### 実際の対応付け
| バイト位置(先頭からの通し番号目安) | 内容 | 判断根拠 |
|---|---|---|
| `05 01 09 02 a1 01 85 01 ...` | **Report ID 1: 通常のマウス** | 看板が`Generic Desktop/Mouse`。中で`05 09 19 01 29 05`(Button 1〜5)→`81 02`(Input, 5bit)で5ボタン、`09 30 09 31`(X,Y)を`16 01 80 26 ff 7f`(16bit signed)→`81 06`、`09 38`(Wheel)を8bit→`81 06`。AC Pan(Consumer page)の記述は無し。→ これが実機ログの`buttons=5 wheel=1 pan=0`と完全一致 |
| `05 0c 09 01 a1 01 85 02 ...` | **Report ID 2: Consumer Control** | 看板が`Consumer Page/Consumer Control`(前回のキーボードと同一シグネチャ)。`75 10 95 01`(16bit×1個)、`15 01 26 8c 02`(Logical 1〜0x28C)、`19 01 2a 8c 02`(Usage Min/Max 1〜0x28C)→`81 00`(Input, Array)。0x28C=652まで幅広くカバーしてるのは、Volume(0xE9/0xEA)やAC Forward/Back(0x225/0x224)なども含めて汎用テンプレートにしてあるため、と読める |
| `05 01 09 80 a1 01 85 03 ...` | **Report ID 3: System Control** | 看板が`Generic Desktop/System Control`。Usage 0x81〜0x83(Sleep/Power Down/Wake Up相当)を3bit、+5bitパディング。どのボタンにも紐付いてなさそうで、今回の6機能(Ctrl+C/V, Volume, 進む/戻る)には無関係と思われる |
| `05 01 09 00 a1 01 85 05 06 00 ff ...` | **Report ID 5: ベンダー独自Feature** | `06 00 ff`でベンダーページに切り替え、`b1 02`(**Feature**、Inputではない)で7バイト。マクロ割り当てを設定ソフトが書き込むための領域と思われる。Inputではないので通常のポーリングには一切出てこない |

### 結論
このマウスは1つのUSBインターフェースの中に、Report IDで4つの独立した機能を束ねている(前回解析したキーボードと全く同じ設計パターン)。**Ctrl+C/Vが動くのは、恐らくこのマウスが別途もう1つ本物のBoot Keyboardインターフェースを持っていて、それが完全に独立した経路で動いているから**(前回のcombined testで見えた`Keyboard connected: Boot Protocol`はこれだったと考えられる)。一方、**Volume/進む/戻るが動かないのは、このマウスのReport ID 2(Consumer Control)を今のコードが一切パースしに行っていなかったから** — `handle_driver_connected()`が`dev_params.proto==MOUSE`と判定した時点で`hid_parse_mouse_report_descriptor()`しか呼んでおらず、同じディスクリプタ内の別Report IDにあるConsumer Controlフィールドの存在を確認すらしていなかった。これは**チャンネル枯渇(`No more HCD channels`)とは別の、純粋な実装のギャップ**。

### 実装
`usb_host_task.c`を修正:
- `mouse_device_state_t`に`consumer_report_layout_t consumer_layout`を追加。`is_mouse`分岐で`hid_parse_mouse_report_descriptor()`と**併せて**`hid_parse_consumer_report_descriptor()`も同じディスクリプタに対して呼ぶようにした(パースが独立してるので両方いつでも試してよい)
- 受信側(`hid_host_interface_callback()`)を、`dev_params.proto==MOUSE`の場合でも**incoming reportの先頭バイト(Report ID)を見て**、`consumer_layout`のReport IDと一致すればConsumer Controlとして処理、それ以外は従来通りマウスとして処理、に変更
- `handle_consumer_report()`の引数を`consumer_device_state_t*`から`consumer_report_layout_t*`に変更し、独立したConsumer Controlデバイス(キーボード側)とマウス内蔵のConsumer Control両方で使い回せるようにした

実機の148バイトのDescriptorをそのままネイティブユニットテストに追加し(`test_hid_parser_9button_mouse.c`)、以下を確認済み:
- マウス側: `button_count=5, wheel.present=1, pan.present=0`(実機ログと一致)
- Consumer側: `report_id=2, bit_offset=0, bit_length=16`が正しく検出される
- Report ID 1(マウス)のパケットをConsumer側フィールドとして誤読しないこと

ビルド確認済み(Host role、警告0件)。

### 残っている論点
1. Hub経由での`No more HCD channels`(カーソルすら動かない方)は、今回の修正とは別問題としてまだ残っている(チャンネル予算の話、上の「9ボタンマウスをHubに繋いだ時の`No more HCD channels available`」節を参照)
2. Ctrl+C/Vを送っていると思われる「本物のBoot Keyboardインターフェース」の方はまだ実機Descriptorを見れていない(今回見たのはマウス扱いのインターフェースのみ)
3. 実機で再テストして、Volume/進む/戻るが実際にTarget PC側で反応するか確認してほしい

## Consumer Control実装後の再テストで発覚: 記述子と実挙動の食い違い(2026-08-22 追記)

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

### 次にやること
1. 再度実機で接続し、`SET_PROTOCOL(Report)=...`と`GET_PROTOCOL=...`のログを確認 — 要求が本当に成功しているか、成功してるならデバイス側が本当にReport Protocolだと認識してるか
2. それでも原因がはっきりしなければ、ESP32側だけで粘るより**OS側(Linux)で`evdev`等を使ってこのマウスの生HIDレポートを直接観測する**方が早いかもしれない(`server.py`のevdev周りの仕組みを流用できる可能性がある) — ESP32のUSB Hostスタックの制約なのか、マウス側の挙動なのかを切り分けたい
3. ホイールは今のところこの3バイト経路には情報が無く、別経路が見つからない限り対応できなさそう

## Linux側(`usbhid-dump`)で実機の生バイトを確認(2026-08-22 追記)

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

### 次の一手として検討したこと: `iface->ep_in_mps`をESP32側でも独自に確認
`usb_host_get_active_config_descriptor()` + `usb_parse_endpoint_descriptor_by_address()`(`usb/usb_helpers.h`)を使えば、ESP32側でも標準USBエンドポイント記述子から`wMaxPacketSize`を独自に読み直せる。ただしこれには**独自の`usb_host_client_handle_t`を新規に登録する必要がある**(今の`usb_host_task.c`は`espressif/usb_host_hid`ドライバに全てのクライアント登録を任せていて、生の`usb_host`低レベルAPIを直接呼んでいない) — 見積もっていたより低コストではない。しかも`lsusb`で外部から`wMaxPacketSize=8`は既に確定済みなので、**ESP32側で同じ値を再確認する意義は薄い**(`USB_EP_DESC_GET_MPS()`は単純なフィールド抽出で、ESP-IDF全体で広く使われてる実績のあるコードなので、ここが間違っている可能性は低い)。

そのため、この特定の確認は**費用対効果が見合わないと判断し、見送る**。

## 実装: 観測された長さを信用するフォールバック(2026-08-22 追記)

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

ホイールのみ、Boot Protocol形式にそもそも情報が存在しないため未対応のまま(別途検討)。

### 次にやること
1. ホイール対応の方針を検討(別途)
