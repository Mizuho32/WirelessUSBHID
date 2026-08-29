# Host role: WiFi接続完了を待たずにUSB Host/type-cを起動する

## 動機

今までの`main_host.c`は`wifi_manager_init()`(netif初期化+接続試行+**接続完了(IP取得)までブロック**、最大30秒/fast-reconnect時10秒)が返るまで、USB Hostバックエンド選択もtype-c出力起動も一切始まらなかった。ローカルのUSB Host→type-cパススルーはWiFiに何の依存も無いはずなのに、APが遠い/混雑してる/落ちてる等でWiFi接続が遅れると、その間ずっと「マウス/キーボードを繋いでも無反応」になる。「繋いで即操作できるようにしたい」という動機でこれを見直した。

## 気づき: UDP周りの依存は「lwIPスレッドが起きてること」であって「AP接続完了」ではない

`mruby_filter.h`の`mruby_filter_resolve_udp_sinks()`のコメントに元々書いてあった通り、`getaddrinfo()`/`socket()`/`bind()`が本当に必要としているのはlwIPのTCP/IPスレッドが起動していることだけで、これは`wifi_manager_init()`前半(`esp_netif_init()`/`esp_event_loop_create_default()`)の時点で既に立ち上がっている。今までこれらの呼び出しが「`wifi_manager_init()`が完全に返った後」に置かれていたのは、単に関数が「netif初期化+接続試行+待ち」を1個に固めていたからで、機能的な必然ではなかった。

## 変更

`wifi_manager.h/.c`: `wifi_manager_init()`を2つに分割

- `wifi_manager_start(ssid, password, hostname)`: netif/WiFiドライバ起動+接続試行の開始まで。**待たずに即return**。以降の接続試行自体はバックグラウンド(`event_handler`の無限リトライループ)で継続
- `wifi_manager_wait_connected(void)`: 今までの「待つ」部分(fast-reconnectタイムアウト→フルスキャンfallback含む)をそのまま切り出しただけ。分割前と全く同じロジック

`wifi_config_t`はfallback時(`wifi_fallback_connect()`)にも要るので、関数ローカル変数からファイルスコープの`static s_wifi_config`に昇格。

### Device role(`main.c`、pure C)

```c
ESP_ERROR_CHECK(wifi_manager_start(WIFI_SSID, WIFI_PASSWORD, WIFI_HOSTNAME));
esp_err_t wifi_ret = wifi_manager_wait_connected();
if (wifi_ret != ESP_OK) { ... }
```

`start()`+`wait_connected()`を続けて呼ぶだけで、分割前と完全に同じ「接続完了までブロック」の起動シーケンスを再現している。Device roleは全機能がUDP受信前提(ローカル入力が一切無い)なので、ここは今まで通りブロックするのが正しい — 変更する理由が無い。

### Host role(`main_host.c`)

`wifi_manager_start()`だけ呼んで`wait_connected()`は呼ばない。そのままUSB Hostバックエンド選択・type-c起動まで一直線に進む。`mruby_filter_resolve_udp_sinks()`/`mruby_filter_start_net_source()`/`mruby_webui_start()`/`hid_forwarder_init()`は元々の呼び出し順のまま(前述の通りAP接続完了を待つ必要が無いので、そのままで良い)。

結果、WiFi接続がどれだけ遅れても(あるいは失敗し続けても)、ローカルのUSB Host→type-cパススルーは起動シーケンスの最初の方(NVS初期化・mruby VM起動の直後)でほぼ即座に使えるようになる。WiFi接続の成否自体は`wifi_manager.c`の`event_handler`が`IP_EVENT_STA_GOT_IP`で引き続き「WiFi connected」をログするので、実害なく可視性も維持される。

## 既知の注意点: `KVM_TARGET_HOST`をホスト名にしている場合

`wifi_credentials.h`には`KVM_TARGET_HOST`を数値IP(`"192.168.0.2"`、デフォルト)ではなく実ホスト名(`"esp32-kvm-ip"`、コメントアウトされた代替設定)にする選択肢がコメントで用意されている。`hid_forwarder_init()`の`resolve_target()`はこの値を`getaddrinfo()`に渡すが、

- **数値IPの場合**(デフォルト): `getaddrinfo()`は純粋な文字列パースで即座に成功する。ネットワークに一切アクセスしないので、WiFi接続完了前に呼んでも問題ない
- **実ホスト名の場合**: mDNS/DNS解決には実際にネットワークへ参加できていること(最低限IP取得、mDNSなら追加でマルチキャストグループ参加)が要る。WiFi接続完了前にこの経路を通ると`resolve_target()`が失敗し、`hid_forwarder_init()`が`ESP_FAIL`を返して`main_host.c`が5秒後に`esp_restart()`する — WiFiが実際に繋がるまで再起動を繰り返す形になる(今までの「`wifi_manager_init()`が接続完了かタイムアウトまでブロック」設計でも、タイムアウトすれば同様に失敗しうる話ではあるが、今回の変更で「接続完了前に確実に一度は失敗を試みる」形になった分、顕在化しやすくなった)

このプロジェクトの現状のデフォルト(数値IP)では影響無し。もし将来的に`KVM_TARGET_HOST`をホスト名運用にするなら、`hid_forwarder_init()`だけは`wifi_manager_wait_connected()`の後に回す(USB Hostバックエンド/type-c起動より後ろで良い、`hid_forwarder_send_*_to()`は`s_sock < 0`なら黙って何もしないガードが既にあるので、ローカルのtype-c専用パイプラインはそれまでも問題なく動く)よう別途調整が要る。

## 未検証

実機での起動時間短縮の体感確認(WiFi接続が遅い/失敗する環境での、ローカル入力が使えるようになるまでの時間)はユーザー側で実施予定。ビルドはHost/Device両ロールとも確認済み。
