# リモートバーチャルHID

## 概要
UDPで送信した信号をサーバーで受けて、さらにサーバーからWebsocket(じゃなくてもいいが)でクライアントにHID信号を送り、クライアントではバーチャルHIDでPCを操作したい。

## 要件
- サーバー: UDPで受け取り、クライアントにWebsocketとかで送信。ここは難しくないのでやるだけ
- バーチャルHID(クライアント)
  - Windows機を想定
  - Websocketとかで受け取ってHID信号をPCに渡す
  - できれば.NETで実装して、Linux・Windows共通ソースにできるとベスト

## 疑問
- Windows用って、ドライバとか管理者権限無しでHIDなりすましバイナリ(てかバイトコード?)実行可能?
- .NETって結構レイヤー上な気がするけど、HIDなりすましできる? (汎用HIDならまぁ流石にできると思ってるが)

## 調査結果

### UDP側(参考実装): `esp32-kvm-ip/server/`
既存の`server.py`は「PCの物理入力をキャプチャしてESP32(本物のUSB HIDデバイス役)にUDPで飛ばす」スクリプトで、今回作りたい「サーバーがUDPを受けてWebsocketでクライアントに転送」とは向きが逆だが、プロトコルはそのまま流用・参考にできる。
- `protocol.py`: 16byte固定・リトルエンディアン。ヘッダ`H magic(0xCAFE) I seq B type xpad`(8byte) + ペイロード8byte
  - mouse: `B buttons, h dx, h dy, b wheel, b pan, xpad`
  - keyboard: `B modifiers, B reserved, 6s keycodes`(USB HID Bootキーボードレポートそのもの)
  - consumer: `H usage_id, 6x pad`
- `udp_sender.py`: 状態が変化した時だけ送信(dirty flag)、送信レートは可変(60〜1000Hz)、ポート4210固定
- そのままこのバイナリ形式をWebsocketのペイロードにも使い回せば、サーバー側の変換ロジックはほぼ不要(受信UDPパケットをそのままWSでブロードキャストするだけ)。

### Windowsでの「HIDなりすまし」方式の比較
「バーチャルHID」と一口に言っても実体は段階がある。

1. **SendInput (user32.dll)** — ドライバ・管理者権限不要。.NETからP/Invoke一発(`[DllImport("user32.dll")]`)で叩ける、実装コストは最小。
   - ただしOSが合成入力に`LLMHF_INJECTED`相当のフラグを立てるため、Raw Input/DirectInputを直接見るアプリやアンチチートは「本物のHIDデバイスからの入力ではない」と区別して無視できる(ゲームなど一部で弾かれる)。
   - UIPIにより、送信元プロセスより権限の低い自プロセスから昇格済みウィンドウへは注入できない(送る側も管理者権限が要る場面がある)。
   - 「なりすまし」ではなく「OS入力キューへの注入」なので、疑問1への回答としては「管理者権限は不要だが、これは真のHIDなりすましではない」が正確。

2. **カーネルドライバによる真の仮想HIDデバイス** — vmulti([djpnewton/vmulti](https://github.com/djpnewton/vmulti)、[nefarius/vmulti](https://github.com/ViGEm/vmulti)等のfork多数)、Interception、ViGEmBusなど。OSからは実HIDデバイスとして見えるため、Raw Input/DirectInput経由のアプリにも本物として認識される。
   - ドライバのインストール自体は初回に管理者権限+ドライバ署名(テスト署名 or EV証明書署名)が必要。インストール後のアプリ側runtime実行は、デバイスオブジェクトのACL次第で無管理者権限でも動くことが多い。
   - .NET側は、インストール済みデバイスファイルに対して`CreateFile`+`WriteFile`/`HidD_SetOutputReport`でHIDレポートを書き込むだけなので、.NET自体は全く障害にならない(P/Invokeで素直に叩ける)。

3. **2026-08-16、LizardByte(Sunshineのゲームストリーミングプロジェクト)が発表した新方式** — [libvirtualhid](https://github.com/LizardByte/libvirtualhid) + Nefarius製 "Virtual HID Driver"([紹介記事](https://app.lizardbyte.dev/2026-08-16-introducing-libvirtualhid-and-virtual-hid-driver/))。
   - 触れ込みは「ユーザーモードで動作し、アプリ側が独自のカーネルモードドライバを追加する必要がない」。ViGEmBus/vmulti系の「毎回カスタムドライバを書いて署名する」重さを解消する新しい方向性。
   - **キーボード・マウスはライセンス不要・無償**。ゲームパッドのみ有償ライセンス($14.99/年 または $49.99買い切り、1ライセンスで5台まで)。旧来無償だったViGEmBus(Xbox360/DS4互換)も後方互換のため無償のまま残る。
   - Linux側は`inputtino`(uinputベースと推測)に置き換え済みで、こちらはライセンスキー不要。
   - **未確認事項**: 「ユーザーモードで動作」の内実(既に署名済みで常駐する共有ドライバ/サービスをNefariusが配布していて、アプリ側はそこに繋ぎに行くだけ、という構成が濃厚)、インストール自体に管理者権限が要るか、実行時にアプリ側も管理者権限が要るか — このあたりはリポジトリのインストーラ/ドキュメントを直接読まないと断定できない。本プロジェクトで採用するなら要追加調査。
   - `libvirtualhid`は**C++実装で.NET公式バインディングなし**。.NETから使うにはP/Invoke用のCラッパーを自作するか、C++/CLIブリッジが必要。

### Linux側(クライアントを.NETでクロスプラットフォーム化する場合)
- `/dev/uinput`はカーネル組み込みの仮想入力デバイス機構で、追加ドライバ不要。udevルールで対象ユーザー/グループに書き込み権限を与えておけば、**実行時のroot権限は不要**(初回のudevルール設置だけrootが要る)。
- .NETからは`ioctl`をP/Invokeで直接叩くか、`inputtino`/`libvirtualhid`のようなラッパー経由で使う。
- WindowsのVHF相当の仕組みがLinuxカーネルには標準で入っている、という非対称性がそもそもの難しさの根本。

### 疑問への回答まとめ
- **Q1(Windows、ドライバ・管理者権限無しでHIDなりすまし可能か)**: 「真のHIDなりすまし」は不可。無管理者権限・無ドライバで完結するのはSendInputレベルの入力注入までで、これはHID層のなりすましではない。真のHIDなりすましをしたいなら、最低でも一度は管理者権限でのドライバ(署名済み)インストールが必要になる。2026-08時点でNefarius Virtual HID Driverがこの手間を減らす方向で動いているが、詳細な権限要件は要追加調査。
- **Q2(.NETでHIDなりすましできるか)**: .NET自体は制約にならない。Win32 API/デバイスファイルへのP/Invokeは.NET(Framework/Core問わず)から素直にできるので、「.NETだから無理」ということはない。ボトルネックは常にOS側(Windowsには標準の仮想HID作成機構が無い)であり、.NETはそのOS機構(SendInput、既存の仮想HIDドライバのデバイスファイル、Linuxならuinput)を薄く叩く層でしかない。
- 実務上の落とし所は概ね3択: (a) SendInputで妥協(手軽だが一部アプリに無視される)、(b) 署名済み仮想HIDドライバ(vmulti/Interception/ViGEmBus、またはNefarius Virtual HID Driver)を前提にインストーラで導入し.NETはそのデバイスファイルを叩くだけにする、(c) ソフトウェアのみでの仮想HID化を諦め、本プロジェクトの既存資産(ESP32-S3をUSBデバイスとしてクライアントPCに物理接続するdevice role)をそのままWindows機側にも使う。

## 実装試行1
OK。実装して

### 要件詳細
- サーバーには、サーバーローカルネットワーク上機器からUDPで送られてくる
- グローバルからWebsocketとかで接続
- セキュア化はプロキシで包むよくあるやつ
- Virtual HIDはサーバーホストのみ設定項目の超シンプル構成(他にも最低限必要な項目あれば実装)

### 実装結果
`virtual_hid/`(リポジトリ直下、esp32-kvm-ip/とは完全独立)に実装。詳細は`virtual_hid/README.md`。

- **プロトコル**: `virtual_hid/PROTOCOL.md`。esp32-kvm-ip/server/protocol.pyと同じ16byte固定バイナリ形式を、独立実装として踏襲(参照はしたがesp32-kvm-ip/への変更・依存は無し)。マウスのbuttonsビットにback/forward(戻る/進む)を追加定義。
- **server/(Ruby, em-websocket)**: UDPで受けたバイト列をmagic値だけ検証してWS接続中の全クライアントへそのままブロードキャストする薄い中継。プロトコルの中身は解釈しない。TLS終端はしない(リバースプロキシ前提)。`?token=`クエリでの認証機構を追加(グローバル公開時の最低限の要件と判断)。
  - Ruby 3.4では`base64`gemがデフォルトgem外になっており、em-websocketの依存先が要求するため`Gemfile`に明示追加が必要だった。
  - 動作確認: Ruby製UDP送信→relay_server.rb→WebSocketクライアントの実バイト列一致を確認済み。
- **client/(.NET 9, C#)**: 設定はサーバーURLのみ(`--server wss://host/?token=...`)。
  - Windows: `SendInput`で注入。マウス(移動・左右中/戻る進む・縦横ホイール)、キーボード(USB HID usage→VKマッピング、最大6キー同時+8種修飾キー)、consumer(音量/ミュート/再生系)に対応。ドライバ・管理者権限不要。
  - Linux: 未実装スタブ(`NotImplementedException`)。要望があれば`/dev/uinput`で実装(疑問への回答の通りuinputはカーネル組み込みで追加ドライバ不要)。
  - UDPレポートは「現在の押下状態」のスナップショットなので、`SendInput`が要求するdown/upエッジイベントへは`PacketDispatcher`が前回状態との差分を取って変換している。
  - 動作確認: 実際の`Protocol.cs`パーサーを使い、UDP→Ruby中継→.NET ClientWebSocketの実配線でパース結果の一致を確認済み(SendInput自体はWindows実機が無くコード側の実装のみ、実機テストは未実施)。
- **未実施/持ち越し**: Windows実機でのSendInput実地確認、Linux virtual HID実装、リバースプロキシ(TLS)の実設定例。
