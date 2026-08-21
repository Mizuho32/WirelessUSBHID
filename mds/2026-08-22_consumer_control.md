# Host role: メディアキー(Consumer Control)対応

## 実際の接続構成(訂正)
これまでのテストでは「キーボード直結 + Hub(マウス+キーボード)」という構成で書いていたが、実運用の構成は違う:

- ボードのType-Cポートは1個だけなので、**直結できるのはHubだけ**
- Hub配下に **キーボード1台 + マウス2台**

チャンネル予算的には Hub自身(1) + キーボードBoot IF(1) + マウス2台(各1、Report Protocol) = 4 で、8チャンネル中まだ余裕がある。

## 質問: 「チャンネル枯渇が原因なら、9ボタンマウス/メディアキー対応は解決不可能?」

いいえ、どちらも解決不可能ではない。整理すると:

### チャンネル枯渇はバグの結果であり、ハード上限そのものではない
`mds/2026-08-22_multi_device.md`で直した「使わないインターフェースまでopenしていた」バグの話であって、**実際に使うインターフェースだけを数えれば**上記の通り8チャンネルに全然余裕がある。メディアキー用インターフェースを1つ追加で開いても5で収まる。

### メディアキー未対応は「未実装」であって「不可能」ではなかった
`handle_driver_connected()`は`proto == HID_PROTOCOL_NONE`(キーボードのConsumer Controlインターフェースはまさにこれ)のインターフェースを、判定コストすらかけずに`ignoring, not opened`としていた。チャンネル云々ではなく、単に「対応するコードが無かった」だけ。

しかも調べたところ、**Device側・protocol.h・server側は最初からConsumer Control対応が完備されていた**(元々サーバーPC側でローカルキーボードのメディアキーを拾ってTarget PCに転送する機能として実装済みだったもの):
- `protocol.h`: `EVENT_TYPE_CONSUMER`、`usage_id`(0=release)
- `usb_descriptors.c`: `ITF_NUM_CONSUMER`、`TUD_HID_REPORT_DESC_CONSUMER()`
- `hid_task.c`: `EVENT_TYPE_CONSUMER`を受けてそのまま`tud_hid_n_report()`
- `server/protocol.py`, `hid_keymap.py`, `evdev_keymap.py`: Usage IDテーブル、pack/unpack

つまりHost側でやるべきことは「物理キーボードのConsumer Controlインターフェースを読んで、同じ`EVENT_TYPE_CONSUMER`パケットとして送る」だけで、**Device側・protocol.h側は無変更**。

### 9ボタンマウスの方はそもそも無関係
チャンネル枯渇はマウス1台=1チャンネルという話なので、ボタン数は関係ない。実際の制限はHost側パーサーの`HID_MAX_BUTTONS=8`とDevice側`usb_descriptors.c`の5ボタン固定記述子で、こちらは別タスク(`mds/2026-08-22_multi_device.md`参照、今回は未着手)。

## 実装上の制約: Report Descriptorはopenしないと読めない
マウスと違い、Consumer Controlインターフェースは`proto`/`sub_class`だけでは「これがConsumer Controlか、それとも別の未対応ベンダー独自インターフェースか」を判別できない。判別にはReport Descriptorの中身(Usage Page Consumerのフィールドがあるか)を見るしかない。

ところが`espressif/usb_host_hid`の実装を見ると:

```c
// hid_class_request_report_descriptor() 内
HID_RETURN_ON_FALSE((HID_INTERFACE_STATE_READY == iface->state) ||
                    (HID_INTERFACE_STATE_ACTIVE == iface->state),
                    ESP_ERR_INVALID_STATE,
                    "Unable to request report descriptor. Interface is not ready");
```

**Report Descriptorの取得は`hid_host_device_open()`済み(READY/ACTIVE状態)でないとできない** — つまり「本当に使うものだけopenする」という前回のチャンネル節約方針そのままでは、Consumer Controlかどうかの判定ができない。

### 対処
`proto == HID_PROTOCOL_NONE`のインターフェースは「Consumer Controlかもしれない候補」として扱い、一旦`open()`してReport Descriptorを覗く。パースできれば(`consumer_report_layout_t.selector.present`)使う側として登録して`start()`、パースできなければ(=本当に無関係なインターフェースだった)`close()`してチャンネルを返す。無関係なインターフェースについては前回までの「そもそもopenしない」よりわずかにコストが増える(一瞬openしてすぐcloseする分)が、恒久的にチャンネルを保持するわけではないので8チャンネル予算上は無視できる。

## 実装内容

### `hid_report_parser.h`/`.c`
- マウス用パーサーとConsumer Control用パーサーで共通の記述子走査ロジック(`walk_report_descriptor()`)を切り出し、コールバック(`mouse_input_slot_cb`/`consumer_input_slot_cb`)で用途ごとの記録先を分岐する形にリファクタ。
- `hid_parse_consumer_report_descriptor()`を追加。一般的なキーボードのメディアキー実装(Consumer Page上の1フィールド、値そのものが現在押されているUsage ID、0=リリース、大抵16bit)を認識する。1キー1bitの「ビットマップ」型記述子(古い/一部の機種)には非対応 — 見つかった最初の8bit以上のConsumer Pageフィールドを採用する方式なので、そちらは対象外。
- ネイティブ(gcc)ユニットテストで検証済み(Report IDあり/なし、値抽出、Report ID不一致時に0を返すこと、空記述子でも`present=false`のまま失敗しないこと)。

### `usb_host_task.c`
- `consumer_device_state_t` / `find_consumer_device()` / `register_consumer_device()` / `unregister_consumer_device()`(`MAX_CONSUMER_DEVICES=4`)をマウス用と同じパターンで追加。
- `send_consumer_report(usage_id)`、`handle_consumer_report()`を追加。フィルタ(`filter_rules.h`)は今回は通していない(パススルーのみ) — 今のところメディアキーを加工したい要望が無いため、必要になったら`filter_keyboard_report`/`filter_mouse_report`と同じパターンで追加できる。
- `handle_driver_connected()`を上記の通り再構成: `is_mouse` / `is_keyboard` / `maybe_consumer`(`proto == HID_PROTOCOL_NONE`)を判定し、`maybe_consumer`のみ「open→Report Descriptor取得→パース→ダメならclose」という流れを通す。

実機ビルド確認済み(Host/Device両方、警告0件)。

## 実機テストで発覚したバグ: AC Panを誤ってセレクタと誤認

診断用ログ(Report Descriptor hexダンプ + 抽出結果ログ)を仕込んで実機テストしたところ、`Consumer Control device connected`は出るがメディアキーが一切反応しない事象が発生。取得できた実機のReport Descriptorを解読して原因判明。

### キーボードの構成
このキーボードはHID的に3インターフェース構成だった:
1. Boot Keyboardインターフェース(そのまま動作)
2. Vendor Page(0xFF60)の32バイト独自インターフェース → Consumer Pageではないので正しく`not a recognized Consumer Control layout`として`close`
3. **1つのインターフェースにReport IDで3つのcollectionを束ねた複合レポート**(123バイト):
   - Report ID 2: `Usage Page(Generic Desktop)/Usage(Mouse)` — 8ボタン + X/Y/wheel/**AC Pan**(Consumer Page上のUsage)。恐らくキーボード上の音量ホイール等をマウス風スクロールとして実装したもの
   - Report ID 3: System Control(電源/スリープ等)
   - Report ID 4: 本物のConsumer Control(メディアキー) — `Usage Minimum(1)`〜`Usage Maximum(0x02A0)`、Report Size 16、Report Count 1

### 原因
`hid_parse_consumer_report_descriptor()`は「Consumer Page上で見つかった最初の8bit以上のフィールド」を機械的にセレクタとして採用する実装だった。ところがこの実機ではDescriptor内の出現順が Report ID 2 の **AC Pan(Consumer Page上のUsageだが実体はスクロール用、8bit)** → Report ID 4 の本物のセレクタ(16bit)、という順で並んでいたため、**先に出てくるAC Panを誤ってセレクタとして採用してしまっていた**(実機ログ: `bit_offset=32 bit_length=8 report_id=2`)。AC Panは音量ホイールを回さない限り値が変化しないので、メディアキーを押しても常に`usage_id=0x0000`のままだった。

AC Pan自体はConsumer Pageの正規のUsage(0x0238)だが「今押されているキーのセレクタ」とは全く別の意味を持つため、フィールド幅だけで判定するのは不十分だった。

### 修正
`consumer_input_slot_cb()`にAC Pan(`USAGE_CONSUMER_AC_PAN`)を名前で明示的に除外するガードを追加。もらった実機のReport Descriptorのバイト列をそのままリグレッションテストとして追加し(`test_hid_parser_consumer.c`)、Report ID 2のAC Panではなく Report ID 4(`bit_offset=0 bit_length=16`)が正しく選ばれることをネイティブテストで確認済み。実機ビルドも確認済み(Host role)。

## 実機確認完了
修正版で再テストし、メディアキーが正常にTarget PC側で反応することを確認済み。デバッグ用ログ(`Consumer report: usage_id=...`、Report Descriptor hexダンプ)は役目を終えたので削除した。Host/Device両ロールとも再ビルド確認済み(警告0件)。

## 次にやること
1. もしキーボードの記述子が「1キー1bitビットマップ」型で認識されない場合(ログが`not a recognized Consumer Control layout`になる)、実機ログの記述子ダンプを見て個別対応を検討
2. 9ボタンマウスの5→8ボタン拡張(別タスク、`mds/2026-08-22_multi_device.md`参照)
