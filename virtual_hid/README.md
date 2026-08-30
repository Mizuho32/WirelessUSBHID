# virtual_hid

リモートバーチャルHID。ローカルネットワーク上の機器からUDPで届くHID風パケットを
サーバーがWebSocketでグローバルに中継し、クライアント(Windows/Linux)がOSに対して
バーチャルHIDとして入力を注入する。

設計の経緯・技術調査は [`mds/virtual_hid/overview.md`](../mds/virtual_hid/overview.md)、
バイナリプロトコルの詳細は [`PROTOCOL.md`](PROTOCOL.md)を参照。

`esp32-kvm-ip/`とは完全に独立(このディレクトリの実装はesp32-kvm-ip/を一切参照・変更しない)。

## 構成

```
virtual_hid/
  PROTOCOL.md      UDP/WebSocket共通のバイナリプロトコル定義
  server/           UDP→WebSocket中継サーバー (Ruby)
  client/           Virtual HIDクライアント (.NET, Windows/Linux)
```

## server/ (Ruby, em-websocket)

UDPで受けたバイト列を検証(先頭2byteのmagic値のみ)して、WebSocket接続中の
全クライアントへそのままブロードキャストするだけの薄い中継。プロトコルの中身は
一切解釈しない。

TLS終端は行わない。グローバル公開時はリバースプロキシ(nginx/caddy等)で
`wss://`に包む前提。

```bash
cd server
bundle install
VHID_TOKEN=<認証トークン> bundle exec ruby relay_server.rb --udp-port 4210 --ws-port 8765
```

主なオプション: `--udp-host` `--udp-port` `--ws-host` `--ws-port` `--token`
(`--token`省略時は`VHID_TOKEN`環境変数、それも無ければ無認証で起動し警告を出す)

デフォルトのUDPポート4210は`esp32-kvm-ip/main/protocol.h`の`UDP_PORT`と同じ値で、
パケット形式もバイト単位で一致している。そのためesp32-kvm-ip Host role基板の
`wifi_credentials.h`にある`KVM_TARGET_HOST`をこのリレーサーバーのIPに向ければ、
変換無しでそのままvirtual_hidに流せる(`UDP_PORT`はesp32-kvm-ip側のコンパイル時
定数で固定なので、ポートを変える場合はesp32-kvm-ip側の再ビルドが必要)。

## client/ (.NET 9, C#)

設定はサーバーURLのみ(トークンが要る場合はURLのクエリに含める)。

ビルド方法は2通り: 実行先に.NETランタイムを入れる前提の**フレームワーク依存**か、
ランタイム同梱で単体exeにする**自己完結**か。配布して他人のWindows機で動かすなら
自己完結が楽(コピーするだけで動く)。

#### フレームワーク依存(要: 実行先に.NET 9ランタイム)

```bash
cd client
dotnet build -c Release

# 実行(要: 実行先にdotnetランタイム)
dotnet bin/Release/net9.0/VirtualHidClient.dll --server "wss://vhid.example.com/?token=SECRET"
```

#### 自己完結・単一exe(実行先にランタイム不要、コピーだけで動く)

```bash
cd client

# Windows向け
dotnet publish -c Release -r win-x64 --self-contained true \
  -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true
# -> bin/Release/net9.0/win-x64/publish/VirtualHidClient.exe をコピーして実行

# Linux向け (未実装、起動時にNotImplementedExceptionで即座に落ちる)
dotnet publish -c Release -r linux-x64 --self-contained true \
  -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true
# -> bin/Release/net9.0/linux-x64/publish/VirtualHidClient をコピーして実行
```

サイズが気になる場合は`-p:PublishTrimmed=true`(未使用コード削減。このプロジェクトは
リフレクション不使用なので安全)、起動を速くしたい場合は`-p:PublishReadyToRun=true`
を追加できる。

### 実装状況
- **Windows**: `user32.dll!SendInput`でOS入力キューに注入(ドライバ・管理者権限不要)。
  マウス(移動・左右中/戻る進む・縦横ホイール)、キーボード(USB HID usage→VK変換、
  最大6キー同時押し+8種の左右修飾キー)、consumer(音量/ミュート/再生系の主要キー)に対応。
  ゲームなど`Raw Input`/`DirectInput`を直接見るアプリでは合成入力として無視される
  ことがある点は`mds/virtual_hid/overview.md`の調査結果を参照。
- **Linux**: `LinuxHidInjector`は未実装スタブ(`NotImplementedException`)。
  実装する場合は`/dev/uinput`への ioctl P/Invoke を想定。

### 受信パケットの適用ロジック
UDPレポートは「現在押されている状態」のスナップショット(USB HID Bootレポートと同じ)。
`SendInput`はdown/upのエッジイベントを要求するため、`PacketDispatcher`が前回状態との
差分を取り、変化したキー/ボタンだけをイベントとして注入する。
