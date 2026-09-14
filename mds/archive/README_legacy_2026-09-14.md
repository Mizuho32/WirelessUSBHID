# [Archive] トップREADME.md - 旧版(esp32-kvm-ip中心のmruby DSLアーキテクチャ図刷新前)

2026-09-14時点でトップのREADME.mdを刷新した際の退避。

---

# ワイヤレスUSB HID

USB HID機器(複数、HUB経由)をワイヤレス化したい。

## 機能

- **Device role** (`esp32-kvm-ip/`, `KVM_ROLE=DEVICE`): ESP32-S3がUSB HIDデバイスとしてTarget PCに接続し、WiFi/UDPで受けたキーボード・マウス入力を中継
- **Host role** (`KVM_ROLE=HOST`): 別のESP32-S3が物理USBキーボード/マウス(ワイヤレスドングル含む)をUSB Hostとして読み取り、Device role基板へUDP転送
  - USB Hostバックエンドは2系統: ESP32-S3ネイティブOTG、またはMAX3421E(SPI外付けチップ。ネイティブOTGが一部ドングルで報告を3バイトに切り詰めるバグの回避策)
  - Hub経由の複数デバイス、9ボタンマウス、Consumer Control(メディアキー)に対応
  - `filter_rules.h`でキー/ボタンの変換・フィルタ・ルーティングをカスタマイズ可能
- **server.py** (`esp32-kvm-ip/server/`): Windows/LinuxのPCから直接Device role基板へUDP送信する代替入力元。クリップボード貼り付け機能つき
- 調査・検証用サブプロジェクト: `rp2040_host_check/`(RP2040によるUSB Hostクロステスト)、`wireshark_9btn_mouse/`(USBキャプチャ解析)
- **virtual_hid/**: 上記esp32-kvm-ip系とは独立した別プロジェクト。UDPで届くHIDパケットをサーバー(Ruby)がWebSocketでグローバルに中継し、クライアント(.NET, Windows/Linux)がOSに対してバーチャルHIDとして入力する。詳細は`virtual_hid/README.md`、設計背景は`mds/virtual_hid/overview.md`

## filter/conv/routeの編集場所(Host role)

デフォルト(`CONFIG_MRUBY_FILTER_ROUTE_ENABLE=y`)ではmrubyスクリプトのsource/sink/pipeline DSLが本流。旧来のC版(`filter_rules.h`/`route_rules.h`)はビルド時にmrubyを切った場合、またはスクリプトが2段階(アップロード済み→埋め込み`default.rb`)とも読み込みに失敗した場合のフォールバックとしてのみ使われる。

- **編集場所**: `esp32-kvm-ip/main/mruby_scripts/default.rb`(埋め込みデフォルト、要リビルド)か、`mrb_script`パーティションにアップロードしたスクリプト(こちらが優先、リビルド不要)
- **アップロード方法**(どちらもリビルド不要、書き込み後は自動/手動でリセットが要る):
  - WebUI: WiFi接続後、基板のIP/hostnameへブラウザでアクセス(`http://<hostname または IP>/`) → textareaで編集 → Save(自動で再起動して反映)
  - シリアル: `bin/upload_mruby_script.py --port /dev/ttyACM0 path/to/script.rb`(ネイティブUSB-Cポート、外付けUART`/dev/ttyUSB0`ではない点に注意)
- **DSL**: `source`/`sink`/`pipeline`/`from`/`to`/`branch`で入出力を名前付き部品として繋ぐ。`to`ブロックが変換付き接続(旧`filter_*`相当)、`branch`が条件付き無加工接続(旧`route_*_also_udp`相当)。イベントはSymbolキーのHash(`ev[:wheel]`等) - `:keyboard`/`:mouse`/`:consumer`ごとのフィールド一覧は`esp32-kvm-ip/README.md`の「mruby DSL: Event (ev) Reference」参照
- 呼び出し元は`hid_forwarder.c`一箇所(USB Hostバックエンドがrp2040_bridge/max3421/native OTGどれでも共通)。どの物理USB Hostバックエンドを使うかもスクリプト側の`usb_host_backends(*syms)`で制御する
- Cフォールバック版(`filter_rules.h`/`route_rules.h`、gitignore、`.example`をコピーして編集、どちらも未作成なら`filter_rules_default.h`/`route_rules_default.h`で素通し)は非常時の保険としてコード上は残っているが、通常の編集対象ではない
- 詳細: `mds/usb_hid/2026-08-28_mruby_filter_route.md`(設計)、`mds/usb_hid/2026-08-29_mruby_phase1_impl.md`(実装・DSLリファレンス)、`mds/usb_hid/2026-08-30_mruby_phase2_webui.md`(WebUI)。旧C版の経緯は`mds/usb_hid/2026-08-21_filter_conv_route.md`, `mds/usb_hid/2026-08-23_filter_conv_router_with_max3421.md`

## ファームウェア更新(Host role、WiFi OTA)

スクリプト/WebUI frontendだけでなく、ビルドしたアプリイメージ本体もケーブル無しで書き換えられる(`ota_0`/`ota_1`の2枠構成 + ロールバック - 新イメージが起動確認前にクラッシュ/リセットすると前のスロットへ自動で戻る)。

- WebUI: 「Update firmware」でファイル選択(`build.host/esp32-kvm-ip.bin`)→アップロード(自動で再起動して反映)
- CLI: `bin/upload_firmware.py --host <IPまたはhostname> [path/to/esp32-kvm-ip.bin]`(省略時`esp32-kvm-ip/build.host/esp32-kvm-ip.bin`)
- `bin/build_host.sh flash -p <値>`は`<値>`がシリアルデバイス風(`/dev/...`, `COM<N>`)ならこれまで通りシリアル書き込み、そうでなければ(IP/hostname)自動的にWiFi OTAへ切り替わる(`bin/upload_firmware.py`を内部で呼ぶ)
- 詳細: `mds/usb_hid/2026-09-10_wifi_ota.md`

## debug print類の場所

- `esp32-kvm-ip/main/usb_host_rp2040_bridge.c`: `BRIDGE_RATE_MONITOR`(受信rate/interval統計)、`BRIDGE_MINIMAL_TEST`(WiFi/type-c/dispatch_task抜きの最小構成ビルド)
- `esp32-kvm-ip/main/usb_device_typec.c`: `USB_DEVICE_TYPEC_DEBUG`(送信毎ログ)、`USB_DEVICE_TYPEC_RATE_MONITOR`(wait_for_ready blocking統計)
- `esp32-kvm-ip/main/main_host.c`: `HOST_MINIMAL_TEST`(WiFi/type-c/hid_forwarder抜きの最小構成ビルド)
- `rp2040_host_bridge/rp2040_host_bridge.ino`: `BRIDGE_DEBUG`、`RATE_MONITOR`、`POLL_CEILING_TEST`
- `rp2040_host_check/rp2040_host_check.ino`: `RAW_DUMP`、`RATE_MONITOR`
- いずれも該当ファイル冒頭付近の`#define ... 0`を`1`にして有効化


