# RP2040ブリッジ経由のFPS(体感フレームレート)問題

`mds/usb_hid/2026-08-23_rp2040_host_status.md`の続き。type-c直結のクラッシュ・移動量の問題は解決したが、**type-c・UDPどちらの出力先でも、体感フレームレートが明らかに遅い**。RP2040導入前(素のESP32ネイティブOTG Host + UDP、あるいはWindows PC + server.py + UDP)は気にならなかったので、**RP2040ブリッジ導入が原因**と見てよさそう。(注: 確かに素のネイティブOTG Hostだと滑らかに動く。UDPで。となると、ESP32S3の動作速度・UDP通信ともに問題なく、RP2040との通信か動作速度?でもRP2040も100MHzオーバーと十分速い)
(注: また、ドングルの動作速度説もない。有線マウスで試したから。さらに、無線ドングルでも明らかにFPSが落ちる。)

## 現在のパイプライン

```
[ドングル] --USB FS--> [RP2040 (TinyUSB Host)] --UART 460800bps--> [ESP32] --+-- type-c --USB FS--> PC
                                                                             +-- UDP(WiFi)--> Device role基板 --USB FS--> PC
```

## 要検討: 犯人候補の切り分け(先に測るべき)

要件で挙がってた2つの候補(UART速度・RP2040のポーリングレート)を検討する前に、**まず実測すべき**。理由: 御指摘の通り「RP2040側がボトルネックなら通信方式を変えても意味がない」ため、当てずっぽうで対策する前に切り分けが要る。

### UART帯域の机上計算(参考値、実測ではない)

1フレーム = ヘッダ8バイト(sync/type/addr/idx/proto/len×2/checksum) + payload(マウスレポートなら7〜8バイト程度)≒ 15〜16バイト。

460800bpsは1バイトあたり約21.7μs(8N1、10bit/バイト)。1フレーム ≒ 16 × 21.7μs ≒ **347μs**。

これは1000Hzポーリング(1ms周期)ですら理論上余裕がある帯域(347μs << 1000μs)。**机上計算だけで見ると、UART帯域そのものは通常のマウスポーリングレートに対してボトルネックになりにくい**——ただしこれは「フレームが16バイト程度・詰まらず流れれば」の話で、実際の送受信オーバーヘッド(後述)は含んでいない。

### RP2040側で疑わしい点(未検証、要実測)

- `rp2040_host_bridge.ino`の`send_frame()`は**1バイトずつ`Serial1.write()`を呼んでいる**(ヘッダ6回+ペイロード分だけ個別呼び出し)。Arduino版`HardwareSerial`の実装次第では、1回のバルク書き込み(`Serial1.write(buf, len)`)に比べて呼び出しあたりのオーバーヘッドが効いてくる可能性がある。2秒おきの再アナウンス(MOUNTフレーム、最大512バイトのレポート記述子を1バイトずつ)は特に重そう(ただし頻度は低いので継続的なFPS低下の主因ではなさそう、周期的なカクつきの要因にはなりうる)。
- ワイヤレスドングル自体の**無線リンク側のポーリングレート**(USB側でRP2040がどれだけ速くポーリングしても、マウス⇔ドングル間のRF通信自体に上限がある機種は多い、体感125Hz程度が一般的)。これは**RP2040/ESP32どちらのソフトウェアを直しても解決しない、ハードウェア側の上限**。

### 切り分けのための実測案(次にやるべきこと)

1. **RP2040側**: `tuh_hid_report_received_cb()`が呼ばれる頻度(reports/sec)を計測し、`BRIDGE_DEBUG`経由でSerial2に定期出力。これで「ドングル→RP2040のUSB Host」自体が出してるレートが分かる(候補2の直接測定)。
2. **ESP32側**: `dispatch_report()`(`usb_host_rp2040_bridge.c`)が実際に処理してるフレーム数/秒と、チェックサム不一致で捨ててるフレーム数を計測。RP2040側の計測値と比較すれば、UART伝送で落ちてるかどうか分かる(候補1の直接測定)。
3. **type-c送信側**: `usb_device_typec.c`の`wait_for_ready()`が実際に待たされてる頻度・時間も見ておくと、USB側のバックプレッシャーがどれだけ効いてるか分かる。

この3点を測れば、「RP2040のポーリングレート自体が低い」のか「UART伝送で間引かれてる」のか「送信側(type-c/UDP)で詰まってる」のか、憶測でなく数字で切り分けられる。

## 「もし通信方式を変えても意味がない場合」の検討

ご指摘の通り、**RP2040自体の動作速度(ドングルへのポーリングレート)がボトルネックなら、UART高速化やSPI化は無意味**。この場合の選択肢:

### 選択肢A: ESP32/RP2040のどちらかにUSB Host/Deviceをハード実装する(ブリッジそのものを無くす)

これは実質「RP2040ブリッジ導入前の構成に戻す」に等しい:
- ESP32ネイティブOTG Hostを使う → 例の3バイート切り詰めバグが再発(そもそもRP2040ブリッジを作った理由)。
- MAX3421Eを使う → 実機で不安定・故障の疑い(`mds/usb_hid/2026-08-23_filter_conv_router_with_max3421.md`)。

なので「ブリッジを無くす」こと自体は、**FPS改善と引き換えに、既に解決済みだった別の問題を再び背負うトレードオフ**になる。かつ、ご自身が懸念している通り、**ドングルの無線リンク側のポーリングレートが上限なら、ブリッジを無くしてもそのレート自体は変わらない**——ブリッジ(RP2040)がボトルネックだった場合にのみ効く対策。

### 選択肢B: RP2040側ソフトウェアの最適化(ブリッジは維持)

上の実測でRP2040側(USB Host処理 or `Serial1.write()`のオーバーヘッド)がボトルネックと分かった場合:
- `send_frame()`をバイトごとの`Serial1.write()`からバッファ1個にまとめた一括`write(buf, len)`に変更。
- 2秒おきの再アナウンス(MOUNT再送)の頻度を下げる、またはディスクリプタ送信を非同期化。
- Arduinoラッパー(`Adafruit_TinyUSB`)ではなく生のpico-sdk + TinyUSBに書き直す(オーバーヘッド削減、ただし手間大)。

### 選択肢C: UART高速化 or SPI化(ブリッジは維持)

机上計算では現状のUART帯域は主要因ではなさそうだが、実測でチェックサム不一致・取りこぼしが多ければ検討: ボーレートを上げる(RP2040/ESP32双方とも数Mbpsまで対応可能)、またはSPIに変更(MAX3421と同じ「複数バイトのタイミングがシビアな転送」に戻るので、皮肉にもMAX3421で悩んだのと同種の問題を持ち込むリスクがある点は要注意)。

### 選択肢D: 何もしない(ドングル自体の上限と判明した場合)

無線リンク側のポーリングレートが上限だった場合、ソフトウェア側でできることはない。この場合は「今のドングルではこれが限界」と割り切るか、別のワイヤレスマウス/レシーバーに変える、という話になる。

## 実測結果(2026-08-24)

上記の切り分け案を実装し、順に潰していった記録。

### 1. type-c送信側の`tud_hid_n_report()`失敗は0

`usb_device_typec.c`の`wait_for_ready()`に加えて、`tud_hid_n_report()`自体が(`wait_for_ready()`成功後でも)`false`を返すケースをカウントする`submit failures/sec`を追加。結果は常に0——「待った後に実は送信自体が失敗して無音に消えている」説は却下。

```
USBDEV_TYPEC: [rate] wait_for_ready: 102 calls/sec, 79 blocked/sec, 79 ms blocked/sec, 0 submit failures/sec
```

`blocked`が呼び出しの大半を占める点も、フルスピードUSBの1msフレーム境界に対する平均的な待ち(理論上避けられないオーバーヘッド)として説明がつき、それ自体は異常ではないと判断。

### 2. 決定的な手がかり: バースト配信

`usb_host_rp2040_bridge.c`に「連続する2フレームの最短/最長間隔」を追加したところ:

```
USBHOST_RP2040BRIDGE: [rate] 102 reports/sec, 0 checksum failures/sec, 0 queue drops/sec, min interval 13us, max interval 49992us
```

平均は~100Hzで健全に見えるのに、**最短間隔が13μs**(460800bpsの1バイト転送時間21.7μsより短い)——「均等に~10ms間隔で届く」のではなく、**しばらく詰まって溜まった分を一気に処理している**ことの直接証拠。人間の目には「静止→まとめてジャンプ」の繰り返しに見え、静止期間の長さの逆数が体感フレームレートになる(50ms周期の静止なら~20Hz、という具合に、当初の「10, 20Hz前後」という見立てとも整合する)。

### 3. WiFi省電力モードを疑ったが外れ

有力容疑者として、ESP32 STAのデフォルト省電力モード(`WIFI_PS_MIN_MODEM`、AP のDTIM間隔ごとにしかスリープから起きず、起床時にWiFiドライバの高優先度タスクが数msCPUを占有しうる)を疑い、`wifi_manager.c`に`esp_wifi_set_ps(WIFI_PS_NONE)`を追加。

→ **効果なし**。同じ`max interval ~50ms`が再現した。

### 4. RP2040自体は無罪と確定

RP2040の`Serial2`デバッグ出力を(ESP32を介さず)直接確認したところ:

```
[rp2040-rate] 100 reports/sec, min interval 9996us, max interval 10003us
```

ジッター0.1%未満の完璧な~10ms周期。**RP2040のTinyUSB Host処理・マウス・ドングルはすべて無罪**——バーストはESP32側で発生していることが確定した。(のちにこの数値をESP32のコンソールにも出せるよう、`BRIDGE_MSG_STATS`という新しいフレーム種別を追加し、RP2040がSerial1経由で自分の統計値もESP32に転送するようにした——専用のUSBシリアルアダプタなしでも同じログで両方見えるようにするため。)

### 5. `bridge_task`自身のスケジューリング欠落を直接証明

`bridge_task`の`while(1)`ループ1周ごとの実時間ギャップを計測したところ:

```
USBHOST_RP2040BRIDGE: [rate] 78 reports/sec, 0 checksum failures/sec, 0 queue drops/sec, min interval 13us, max interval 49964us, max loop gap 50033us
```

`max loop gap`(50033μs)と`max interval`(49964μs)がほぼ完全に一致——**`bridge_task`が約50ms、実行可能な状態のままCPUを貰えていない**ことが直接証明された。RP2040が10msごとに律儀にデータを送ってきている以上、これは「データが来ていない」のではなく「タスクが動けなかった」ことを意味する。WiFi省電力は無関係と分かった後なので、真の犯人(WiFi/lwIPの別の内部処理、USB割り込み負荷、その他)はまだ特定できていない。

### 6. FreeRTOSランタイム統計(1回目、累積値): 手がかりなし

`sdkconfig`で`CONFIG_FREERTOS_USE_TRACE_FACILITY`/`CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS`を有効化し、`bridge_task`のループギャップが5msを超えた時だけ`vTaskGetRunTimeStats()`の出力をログに追加。結果:

```
usb_host_rp2040 705264          4%
IDLE1           16643409                95%
IDLE0           16413964                93%
tcpip           15309           <1%
wifi            272614          1%
TinyUSB         195335          1%
```

`IDLE0`/`IDLE1`が93-95%で、他は全部<1%——起動からの**累積時間**で見ているせいで、17秒の稼働時間の中の50-100ms程度のスパイクは1%未満に埋もれて他タスクのノイズと区別がつかない。この方式では解像度不足と判明。

### 7. FreeRTOSランタイム統計(2回目、差分値): それでも犯人不在

`uxTaskGetSystemState()`を毎回スナップショットし、直前との**差分**(直近~1秒間の消費時間)を見るよう変更。結果:

```
loop gap 103134us - per-task CPU delta over last ~1s:
  usb_host_rp2040  57864us
  usb_host_rp2040  6140us
  IDLE1            1014030us
  IDLE0            956462us
  tcpip            410us
  esp_timer        139us
  wifi             2168us
  TinyUSB          2226us
```

`IDLE0`/`IDLE1`が合計で2秒間(2コア分)近くを占有——**この1秒間、CPUはほぼ完全にアイドルだった**。つまり「他のタスクがCPUを奪っている」という説自体がここで崩れる。何も動いていないのに`bridge_task`だけが起きられていない。

(この段階で診断ログの出力量が増えるほど`max loop gap`の値も大きくなっている(~50ms→~103ms)ことに気づき、「測定ログ自体が犯人では」と疑って`BRIDGE_RATE_MONITOR`/`USB_DEVICE_TYPEC_RATE_MONITOR`を全部OFFにして試したが、**症状は変わらず**——ログ自体が原因という説もここで却下。)

### 8. 最小構成での切り分け: WiFi/type-c/dispatch_taskを全部消しても再現

WiFi・`hid_forwarder`(UDPソケット)・type-c USB Device出力・`dispatch_task`を`#ifdef`で全部無効化し、`bridge_task`(UART解析+カウントのみ)だけが動く構成でテスト(`HOST_MINIMAL_TEST`/`BRIDGE_MINIMAL_TEST`)。バイナリサイズが995KB→440KBまで縮み、実際にそれらのサブシステムが除外されていることを確認した上で:

```
[rate] 103 reports/sec, 0 checksum failures/sec, 103 queue drops/sec, min interval 9us, max interval 49980us, max loop gap 56299us
  usb_host_rp2040  39897us
  IDLE1            1021000us
  IDLE0            990106us
```

**それでも同じ規模のバーストが再現**。WiFi・lwIP・TinyUSB Device・`dispatch_task`、全部無罪。残るはアプリケーション層より下——`uart_read_bytes()`自体の実装か、割り込みサービスの遅延(タスク単位の統計には出てこない領域)。

### 9. 根本原因確定: `uart_read_bytes()`のタイムアウト値(20ms→1ms)

`bridge_task`の`uart_read_bytes(..., pdMS_TO_TICKS(20))`のタイムアウトを**1msに縮めるだけ**でテストしたところ:

```
[rate] 100 reports/sec, 0 checksum failures/sec, 101 queue drops/sec, min interval 9000us, max interval 11000us, max loop gap 4623us
```

**別次元の改善**。`min/max interval`が9000-11000usという、RP2040自身の測定値(9995-10004us)とほぼ同じ健全な~10ms周期に戻り、`max loop gap`も4623usまで縮小。**タイムアウト値を長く(20ms)指定すると、その待ち時間が数倍(最大で5倍以上)に膨らむ何らかの挙動が、ESP-IDFの`uart_read_bytes()`(内部的にはFreeRTOSのリングバッファ+セマフォ待ち)にあった**、というのが最終的な根本原因。WiFi省電力オフ・ログ削減・タスク分離など、それまで試した対策が軒並み効かなかったのは、そもそも見当違いの層(アプリケーション層のタスクスケジューリング)を疑っていたため。

最小構成の`HOST_MINIMAL_TEST`/`BRIDGE_MINIMAL_TEST`を元に戻し(WiFi・UDP・type-c・`dispatch_task`を全部復活)、1msタイムアウトの修正だけ残した状態で実機テスト → **体感でも直った**。

### 10. 副次的な問題: 2秒周期の詰まり(解決)

上記の根本修正後、新たに「ちょうど2秒おきに詰まる」症状が出た。原因は`dispatch_mount()`——RP2040側の2秒おきの再アナウンス(`REANNOUNCE_INTERVAL_MS`、ESP32だけ再起動してもマウント状態を見失わないための仕組み)が届くたびに、**毎回無条件で**`ESP_LOGI("HID mounted...")` + レポート記述子のhex dump全部をコンソールに出力していたのが原因。20-100ms級の大きなバグに隠れて見えていなかったが、パイプラインが滑らかになったことで単独で目立つようになった。

`find_mouse_device()`/`find_consumer_device()`で「本当に初回マウントか、再アナウンスか」を区別し、初回のみログを出すよう修正。あわせて今回の調査で追加した診断ログ(`BRIDGE_RATE_MONITOR`/`USB_DEVICE_TYPEC_RATE_MONITOR`/RP2040側`RATE_MONITOR`)も全部OFFにしたクリーンな状態に戻した。

## 結論: 解決済み(2026-08-24)

**根本原因は`usb_host_rp2040_bridge.c`の`bridge_task`が`uart_read_bytes()`に渡していたタイムアウト値(20ms)**——RP2040やマウス/ドングル、WiFi、UDP、type-c USB Device、ログ出力量、いずれも無罪だった。20msという(それ自体は一見妥当に見える)タイムアウト指定が、実際の待ち時間を最大100ms級まで膨らませ、その間にRP2040からの~10ms周期のデータがUART受信バッファに溜まり、再開時に一気に処理される(バースト配信)——これが「100Hz流れているのに体感10-20Hzでしか動いて見えない」の正体だった。

**修正**: タイムアウトを1msに短縮。副次的に見えていた「2秒周期の詰まり」は、`dispatch_mount()`が再アナウンス毎に無条件でログ(hex dump込み)を出していたのが原因で、初回マウント時のみログを出すよう修正して解決。

選択肢A〜D(通信方式変更・ハード実装など)はいずれも的外れだった——ブリッジ(RP2040・UART)は最初から無罪で、問題はESP32側のワンライナー(タイムアウト値)だった。
