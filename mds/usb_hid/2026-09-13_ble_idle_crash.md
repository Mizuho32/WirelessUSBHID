# BLEを長時間放置後、有効化しようとするとクラッシュする

`ble_dynamic true`運用（[[ble_dynamic_enable]]）で`ble_toggle false`にした後、長時間(実測約9.4時間)放置してから`ble_toggle true`すると、稀にクラッシュ→リブートする現象。実機ログ(`data/2026-09-13_ble_idle_crash.log`)で解析できた。

## クラッシュの実際のログ

```
I (33901597) MRBFILT: script: "up before long press"
I (33901598) BLE_INIT: BT controller compile version [51d9dfd]
...
I (33901610) BLE_INIT: Bluetooth MAC: e8:3d:c1:fc:5a:2a
I (33901616) wifi:Coexist!!! Wi-Fi station would only keep waked when available

E (33901621) NimBLE: ble_gattc_init rc=6

assert failed: ble_hs_init ble_hs.c:995 (rc == 0)
```

`rc=6` は`host/ble_hs.h`の`BLE_HS_ENOMEM`。`ble_gattc_init()`内の固定サイズmempool(`ble_gattc_proc_pool`用のバッキングメモリ)の`malloc`相当(`nimble_platform_mem_calloc()`)が失敗している。これは**NimBLE/ESP-IDF側の`ble_hs_init()`が、この初期化に失敗すると即`assert()`で落ちる**箇所で、アプリ側からは失敗を横取りしてエラーとして処理する余地がない - 発生させないことでしか防げない。

BTコントローラ自体の起動(`esp_bt_controller_mem_release`/`init`/`enable`)は今回成功しており(以前セッションで見た`esp_bt_controller_mem_release failed: 259`とは別の失敗モード)、失敗しているのはその先の`esp_nimble_init()`→`ble_hs_init()`のホスト側メモリプール確保。

## 直前の経緯(同じログから)

同じログに、意図的な`ble_toggle`のオン/オフが2往復記録されている:

```
I (45866) MRBFILT: script: "up before long press"   <- 短押しでtoggle off
I (45869) BLE_HID: stopped
...
I (45929) ESP_HID_GAP: disconnect; reason=534
E (45934) NimBLE: error enabling advertisement; rc=30
W (45938) BLE_PAIR: re-advertising slot 1 after disconnect failed: ERROR
I (45948) BLE_HID: BLE HID device stopped
I (45949) MRBFILT: script: "ble false"

I (56601) MRBFILT: script: "up before long press"   <- 長押しでtoggle on (再接続成功)
...
I (62489) MRBFILT: script: "up before long press"   <- 再度toggle off
...
I (62569) MRBFILT: script: "ble false"

(ここで約9.4時間放置)

I (33901597) MRBFILT: script: "up before long press"  <- toggle on -> クラッシュ
```

`rc=30`(`BLE_HS_EDISABLED`)は無害な既知のレース: `ble_toggle false`が`esp_hidd_dev_deinit()`で接続中のピアを切断する際、`ble_pair_slots.c`の(別の)disconnectハンドラがまだstop処理中と知らずに再アドバタイズしようとして、ホストが既にdisable中で弾かれるだけ - クラッシュには寄与していない。

## 根本原因の評価

再現時のヒープ計測ログが無く、確定原因の特定はできなかったが、コード読みで以下が分かった:

1. **`esp_hid_ble_gap_adv_init()`(`esp_hid_gap.c`)に本物のリーク**を発見・修正: `ble_hid_device_start()`のたびに呼ばれるこの関数が、HID Service UUID用の`ble_uuid16_t`を毎回`malloc()`していたが、書き込み先の`fields`(static)の`uuids16`ポインタは前回分を指したまま上書きされるだけで、`free()`されることは一度も無かった。値が変わらない定数なので、`malloc`せず`static const`にして使い回すよう修正(`esp_hid_gap.c`)。1サイクルあたり`sizeof(ble_uuid16_t)`(数バイト)程度なので、今回の(2サイクルで発生した)クラッシュそのものの主因とは考えにくいが、確実なバグではあるので直した。
2. 起動直後の内部RAM空き実測が確認できたのは一度だけ(`internal free=106464 largest block=45056`, 起動2.9秒後)で、ESP32-S3の内部SRAMとしては元々あまり余裕が無い。9時間超の稼働中、BLEと無関係な要因(WiFi/mrubyスクリプト/デバッグストリーム等)を含む断片化・消費が積み重なり、3回目の`ble_hid_device_start()`が要求するNimBLEホスト側の固定プール確保に必要な連続領域が不足した、という説明が最も筋が通る - が、この1回のログだけでは確証できない。

## 対処

上記1のリーク修正に加えて、次回の発生に備えて**診断用のヒープログ**を追加(`ble_hid_device.c`): `ble_hid_device_start()`の入り口と`ble_hid_device_stop()`の出口それぞれで`internal free`/`largest block`を`ESP_LOGI`。動作は変えない。次に同じ現象が起きた際、このログから

- サイクルを重ねるごとに空きが目に見えて減っているか(BLE絡みの累積リークがまだ他にもあるか)
- 単に長時間稼働の間にBLEと無関係に地力で減っているか(BLE非稼働中も含めた全体のヒープ圧迫)

を切り分けられる。

## 未解決

- クラッシュの確定原因(何が内部ヒープを消費しているか)は特定できていない - 次回発生時のヒープログ待ち。
- `ble_gattc_init()`が確保しようとしているプールは本来GATT**クライアント**側(このプロジェクトはBLE HID周辺機器=GATTサーバーとしてしか動かない)のもの - `ble_hs_init()`が無条件にクライアント側も初期化する仕様のようで、アプリ側で不要とマークして節約する手段は確認できていない(未調査)。

### 追記1: 「放置中は`ble_toggle false`だった」という点の意味

今回の実際の再現パターン(この文書冒頭のログ)は、放置に入る**直前**に`ble_toggle false`していて、放置中ずっとBLEスタック自体は完全に落ちていた(`ble_hid_device_stop()`済み)。ここが重要な手がかりで: 放置中BLE/NimBLEは一切動いていないので、放置中にヒープを消費/断片化させ得るのはBLEと無関係などこか(WiFi manager、mrubyスクリプト、debug_stream、NTP同期等)のはず。

追加した`start:`/`stop:`ヒープログはちょうどこの前後を挟む(放置直前の`stop:`ログと、放置後にクラッシュを引き起こした`start:`ログ - 後者は`ble_gattc_init`で落ちるより前に出るので確実に記録される)。次回この2点の実測値に有意な差があれば、「BLEの起動/停止サイクル自体のリーク」ではなく「BLEと無関係などこかが放置中に食っている」という説がほぼ確定する - この場合、今回直したuuid16リーク(BLEが**動いてる**サイクルでしか発生しない)は今回のクラッシュの主因ではなかったことになる。

## 参考

- [[ble_dynamic_enable]] - `ble_dynamic true`/`ble_toggle`の設計
- [[ble_multi_pair]], [[ble_multi_pair_own_address]] - このセッションの直前に解決したマルチペア関連の別issue群
- `esp32-kvm-ip/main/esp_hid_gap.c`の`esp_hid_ble_gap_adv_init()`/`deinit_low_level()`
- `esp32-kvm-ip/main/ble_hid_device.c`の`ble_hid_device_start()`/`ble_hid_device_stop()`
- `data/2026-09-13_ble_idle_crash.log` - 実機ログ全体
