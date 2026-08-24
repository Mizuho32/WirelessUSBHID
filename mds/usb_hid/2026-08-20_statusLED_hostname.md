## DHCPホスト名 + 起動完了LED通知(コミット b725b17)

- **ホスト名**: `main/wifi_credentials.h`に`WIFI_HOSTNAME`を追加(デフォルト`esp32-kvm-ip`)。`wifi_manager_init()`がWiFi接続前に`esp_netif_set_hostname()`でセットするので、OpenWRTのDHCPリース一覧にIPだけでなくホスト名で表示されます。
- **LED**: XIAO ESP32S3のオンボードLED(GPIO21、active-low)を使う`status_led`モジュールを追加。起動時は消灯、WiFi接続+IP取得まで含めた初期化完了時に点灯します。

ビルドは警告・エラーなしで成功、変更した6ファイルのみ再コンパイルされることも確認済みです。実機での動作確認(LED点灯、OpenWRT側でのホスト名表示)はまだなので、必要であればそちらもどうぞ。

## 実機WiFi接続トラブルシュート(コミット 4cf5bfc)

実機で`WIFI: Disconnected`を繰り返して繋がらない問題が発生、切断理由コードを見て原因を切り分け。

- **理由コードのログ出力を追加**: 元コードは切断理由を出していなかったため、`wifi_event_sta_disconnected_t.reason`をログに追加。
- **`reason 211` (NO_AP_FOUND_IN_AUTHMODE_THRESHOLD)**: 元の`threshold.authmode = WIFI_AUTH_WPA2_WPA3_PSK`は「これ以上のセキュリティ」を要求する閾値のため、接続先が素のWPA1のAPだと事前フィルタで弾かれていた。`WIFI_AUTH_WPA_PSK`まで下げて解決(WPA2/WPA3のAPはそのまま通る)。
- **`reason 2`/`205` (AUTH_EXPIRE/CONNECTION_FAIL)**: WPA1→WPA2に変えても解消せず。これはWPAネゴシエーションより手前の素の802.11認証段階での失敗のため、暗号方式は無関係と判断。古い/廉価な11nチップセット搭載APとの相性問題を疑い、失敗が続いたら802.11b/g限定にダウングレードして再試行するフォールバックを追加(`esp_wifi_set_protocol`、5回連続失敗で発動、一度ダウングレードしたらセッション中は維持)。
- **最終的な根本原因はESP32側のアンテナ未接続**(物理層の問題)だったが、上記のソフト側修正(WPA_PSK閾値 + b/gフォールバック)も併せて必要だったことをstashして確認済み(スタッシュすると繋がらなくなる)。

ビルドは警告・エラーなしで成功。
