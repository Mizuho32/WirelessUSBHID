## 9ボタンマウスをHubに繋いだ時の`No more HCD channels available`

### 9ボタンマウスについて
3ボタンは通常と同じ。残り6ボタンにカスタム機能を割り当てられ、現在(ルートプロジェクト コミット59f531b時付近)はCtrl C/V, Volume, 進む/戻る の機能を割り当てている。
Hub経由でHostに繋ぐと、Ctrl C/Vだけが動作し、残り7ボタンは動作しない。カーソルも動かない。

### 構成
Hub配下: キーボード(メディアキーあり、`mds/usb_hid/2026-08-22_consumer_control.md`で対応済み) + マウス2台。1台目は5ボタンマウス(問題なし)、2台目として9ボタンマウスを挿すと`No more HCD channels available`が発生。

### チャンネル予算の見積もり
定常状態(9ボタンマウス自体に追加のUSBインターフェースが無いと仮定した場合)で持続的にopenされるのは:

- Hub自身: 1
- キーボード Boot Interface: 1
- キーボード Consumer Control Interface: 1(認識成功、`mds/usb_hid/2026-08-22_consumer_control.md`)
- マウス1台目(5ボタン、Report Protocol): 1
- マウス2台目(9ボタン)のメインインターフェース: 1

合計 **5/8**。ボタン数そのものはチャンネル消費に無関係(HID_MAX_BUTTONSやDevice側5ボタン固定は純粋にソフトウェア側のフィールド解釈の話)なので、9ボタンマウスの「メイン」インターフェース1つが5ボタンマウスと同じく1チャンネルしか使わないなら、5/8で全然余裕があるはず。

キーボードのベンダー独自インターフェース(0xFF60、`mds/usb_hid/2026-08-22_consumer_control.md`参照)は「Consumer Controlかもしれない候補」として一時的にopen→即座にclose(認識できなかったので)されるが、これは`usb_host_app_task`が1イベントずつ直列に処理しているので、他のトランジェントなopenと重なることはない(次のCONNECTEDイベントを処理する前に必ずclose済み)。

### つまり
**見積もり上は5/8で全然余裕があるはずなのに枯渇する**、というのが今の理解。これは:

1. 見積もりが外れている(9ボタンマウス自体が、メインのマウスインターフェース以外に**独自のベンダー向けインターフェース(マクロ/DPI設定/RGB制御用など、ゲーミングマウスによくある)を1つ以上持っていて**、それぞれが「Consumer Controlかもしれない候補」としてopen/close(または最悪、誤ってConsumer Controlとして認識されて永続open)される
2. または、closeしたはずのチャンネルが実際にはハードウェア側で即座に解放されていない(タイミング/非同期の問題)
3. または、そもそも見積もりに入れていない何か(Hub自身が新規デバイスのenumeration時に一時的に追加のチャンネルを使う、など)がある

のどれかだと思うが、これは**推測の域を出ない** — 前回、Hub対応やチャンネル枯渇そのものについても最初の仮説が2回とも外れた(`mds/usb_hid/2026-08-22_multi_device.md`の「有力な仮説」節、「ハードウェア制約」節を参照)経緯があるので、今回も実機ログを見ないと確信は持てない。

対策の方向性としては、確認が取れれば以下が有力候補(実装はまだしていない):
- Consumer Control候補のprobeを「同一アドレスに本物のキーボードIF(proto=KEYBOARD, Boot Interface)が既にある場合だけ」に絞る(Consumer Controlはキーボード寄りの機能なので、マウス由来のベンダーIFを毎回probeする必要はそもそも無い)
- チャンネル解放のタイミング/同期を`hid_host.c`のソースで確認する

### 次にやること
1. 9ボタンマウス単体で(キーボード無し、または他のマウス無しで)Hub経由で接続した場合に同じエラーが出るか確認 → 出なければ「合わせ技」の問題、出れば9ボタンマウス単体の構成の問題
2. `mds/usb_hid/2026-08-22_consumer_control.md`でやったのと同じ要領で、9ボタンマウスの各インターフェースのReport Descriptorを取得し、実際に何個・どんなインターフェースを持っているか確認

### デバッグログについて
`ESP_LOGD`にしてみようとしたが、このプロジェクトの`sdkconfig`は`CONFIG_LOG_MAXIMUM_LEVEL=INFO`なので、`ESP_LOGD`は実行時フィルタではなく**コンパイル時に呼び出しごと消える**(`esp_log_level_set()`では救えない)。`CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y`にすると直るが、`sdkconfig.h`が変わるので今回だけ大きめの再ビルドが必要になる — デバッグ目的だけでそれをやるのは重いのでやめ、**`ESP_LOGI`のまま残して、使わない時は手動でコメントアウトする**方針にした(`usb_host_task.c`の`handle_consumer_report()`と`handle_driver_connected()`のproto0分岐)。

### 9ボタンマウスの挙動から見えてきたこと(重要な手がかり)
Hub経由で接続すると **Ctrl C/Vだけ動作し、残り7ボタンは無反応、カーソルも動かない**。これは大きなヒント:

- Ctrl+C/Vが動く ⇒ このマウスは**マクロキー用に別途「キーボードとして振る舞うインターフェース」**を持っていて(Ctrl+C/Vは生のマウスボタンでは表現できないので当然)、それが`is_keyboard`判定(`proto==KEYBOARD && sub_class==BOOT_INTERFACE`)を通って正常にopen/startされている、ということ
- **カーソルすら動かない** ⇒ 本来一番重要なはずのメインのマウスインターフェース(X/Y移動)自体が`open()`に失敗している(チャンネル枯渇のタイミングで運悪く後回しにされた)可能性が高い
- Volume/進む/戻るが動かない ⇒ Volumeは恐らくConsumer Controlインターフェース経由、進む/戻るはメインマウスの追加ボタン経由だが、いずれもチャンネルが足りずopenできていないと考えれば筋が通る

つまりこのマウスは実質「メインマウス + マクロ用キーボードIF + (恐らく)Volume用Consumer Control IF + 場合によってはさらにベンダーIF」という**複数インターフェースを持つ複合デバイス**である可能性が高く、それが`mds/usb_hid/2026-08-22_multi_device.md`で立てた仮説(9ボタンマウス自体が追加インターフェースを持っている)を裏付ける形になっている。実機のReport Descriptorダンプで確定させたい。

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

## Report ID解釈のミスマッチ・ホイール未対応の詳細調査は別ファイルへ

Consumer Control実装後の実機再テストで、「記述子と実際に届くバイト列が噛み合わない」という別の問題が見つかった(このマウス=ワイヤレスドングルが、`GET_PROTOCOL`ではReport Protocolのつもりと答えるのに、実際にはずっとBoot Protocol風の短いレポートしか送ってこない、という話)。カーソル移動・クリック・進む/戻る・Volumeはこの問題を踏まえたフォールバック実装で解決済みだが、**ホイールだけは未解決**(Boot Protocol形式にそもそもホイールの情報が無いため)。

この一連の調査(Linux実機の`usbhid-dump`との比較、`wMaxPacketSize`理論の検証と否定、観測された長さを信用するフォールバックの実装、Wiresharkでの追加調査方針)は`mds/usb_hid/2026-08-22_wireless_dongle_short_reports.md`にまとめてある。
