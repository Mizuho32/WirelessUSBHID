# WiFi fast-reconnectのstatic IP適用をopt-inに (2026-09-06)

## バグ

OpenWrt側でESP32のホスト名解決ができない(digが通らない、`/tmp/dhcp.leases`
にもいない)のに、IPへのpingは通る、という報告から発覚。

`wifi_manager_start()`のfast-reconnect(前回接続時のBSSID/channel/IP/GW/
netmaskをNVSにキャッシュし、次回起動を高速化する機能)は、キャッシュが
あると`apply_static_ip()`を呼び、**DHCPクライアントを止めて前回のIPを
直接static設定**していた。つまり初回起動以降は毎回、ルータと実際の
DHCPパケットのやり取りを一切せずに前回のIPへ"自己申告"で居座るだけに
なっていた。

- pingが通る: L2/L3的には正しいIPを使えているだけなので当然
- `/tmp/dhcp.leases`にない/digが通らない: dnsmasqはDHCPトランザクション
  でしかリース台帳(≒ホスト名解決)を作らないので、初回のリースが期限
  切れで消えた後は二度と載らない

## 修正

BSSID/channelの再利用(AP選定・アソシエーション高速化)と、IP割り当ての
static化は別物として分離。`apply_static_ip()`は
`mruby_filter_wifi_fast_reconnect_static_ip_enabled()`が`true`を返す時
だけ呼ぶようにした。デフォルトは`false`= 毎回本物のDHCPハンドシェイクを
行う(ホスト名/DNS解決が常に正しく機能する)。

- mruby DSL: `wifi_fast_reconnect_static_ip true` で明示的にopt-inできる
  (起動時間をわずかに削りたい場合向け。ルータのホスト名解決を諦める
  トレードオフを理解した上で)。
- `main/mruby_filter.c`/`.h`: 他のトグル(`usb_suspend_wifi_sleep`等)と
  同じ、weak-symbol経由でのDevice roleフォールバック(false)パターン。
- `main/wifi_manager.c`: `wifi_fast_reconnect_static_ip_enabled()`ヘル
  パーを追加し、`apply_static_ip()`呼び出しをこれで囲んだだけ。
  BSSID/channelキャッシュ自体は無条件で引き続き使う。
