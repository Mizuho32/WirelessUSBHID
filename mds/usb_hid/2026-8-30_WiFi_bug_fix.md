# WiFi reason 201 (NO_AP_FOUND) ループから復旧しないバグ修正

## 概要

`wifi_manager.c`の自動再接続ロジックに、一度接続した後にfast-reconnect用のキャッシュ(BSSID/channel)が古くなると、二度と復旧しなくなるバグがあった。[USB suspend/resume電源管理機能](2026-8-30_Sleep.md)の実機テスト中に発見したが、その機能自体とは無関係で、`wifi_manager.c`に元からあった問題。

## 発生状況

実機でWiFiが`reason 201`(`WIFI_REASON_NO_AP_FOUND`)で切断されたまま、何度再接続を試みても同じ理由で失敗し続け、自己復旧しなかった。

## 原因

`wifi_manager_start()`のfast-reconnectパス(NVSキャッシュに保存したBSSID+channelを`s_wifi_config.sta.bssid_set = true`で固定)で一度接続した後、`event_handler`の`WIFI_EVENT_STA_DISCONNECTED`ハンドラは切断のたびに**同じ固定BSSID/channelのまま**`esp_wifi_connect()`を無限リトライするだけだった。BSSID/channelをクリアしてフルスキャンにフォールバックする`wifi_fallback_connect()`は、起動時の`wifi_manager_wait_connected()`経路からしか呼ばれておらず、実行時の自動再接続には無かった。

つまり、一度接続した後にルーター側でチャンネルが変わる(自動チャンネル選択・DFS切替・ルーター再起動でBSSID変化等)と、そのAPはもう「そのチャンネルにいない」ので毎回reason 201(NO_AP_FOUND)で失敗し、永遠にそのまま復旧しない。

## 修正

`nvs_clear_cache()`+`restore_dhcp()`+BSSID/channelクリアのロジックを`wifi_reset_to_full_scan()`という共通関数として切り出し、`event_handler`が`disc->reason == WIFI_REASON_NO_AP_FOUND && s_wifi_config.sta.bssid_set`を検知した時点で即座にこれを呼ぶように変更した。以降は通常の`esp_wifi_connect()`がフルスキャンとして働く。起動時の`wifi_fallback_connect()`もこの共通関数を使うよう統合した。

Host/Device両ロールともビルド確認済み、実機で修正確認済み(esp32-kvm-ip commit `3ef5fe3`)。
