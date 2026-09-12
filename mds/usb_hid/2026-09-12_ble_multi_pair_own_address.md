# マルチペア「スロット切り替え」実戦投入記 - 各スロット固有アドレス化とRPA対応

[[ble_multi_pair]]でスロット切り替え自体(ディレクテッドアドバタイズでの再接続、`ble_pair_switch`/`ble_pair_new`)は動くようになったが、実機で2台(PC=KDE Plasma、Android)を実際に切り替え運用してみると、さらに2つの現象にぶつかった。それぞれ何を試して何が効いたかをまとめる。長い顛末は[[after_timer_dsl]]の追記1〜6に逐次記録してある - ここはその最終結論のダイジェスト。

## 症状1: スロットを切り替えると、外れた側のホストが再接続を試みてチラつく/別スロットの新規ペアリングを奪う

**現象**: slot1(KDE)からslot2(Android)に切り替えると、KDEのBluetoothアイコンが「一瞬繋がって切れる」を定期的に繰り返す。また、slot2を新規ペアリング用に開けると、そこにslot1のデバイス(まだ近くにいて再接続を試みている)が代わりに繋がってしまうことがあった。

**試して失敗したこと**(いずれも[[after_timer_dsl]]に詳細):
- 「`ble_pair_new()`で消したばかりのアドレスからの再接続を拒否する」(`s_avoid_addr`) - 実際には発火せず(アドレス比較が一致しなかった)効果不明のまま複雑さだけ増えた。
- 「pairing中に接続してきた相手が別スロット既存ボンドと一致したら拒否する」(`slot_bonded_to()`) - 今度は「既に別スロットにボンド済みのデバイスを、別のスロットへ意図的に再ペアリングすること」自体を永久に阻害してしまい、実機で「PCがslot1に永久にペアできない(既にslot2にいるから)」という実害の方が大きかった。

**本当の原因**: 全スロットが同じBLEアドレス(このESP32本体の固定MAC)を名乗っていたこと。ディレクテッドアドバタイズは「そのアドレス宛でないと接続はできない」だけで、「そのアドレス宛のADV_DIRECT_INDパケット自体は誰でも受信できる」ため、KDE側は「いつものデバイスがまだ電波を出してる」と認識して再接続を試み続けてしまう。

**実際に効いた対処**: 市販マルチペア機器やESP32のマルチホストBLEキーボード実装([`markusg1234/ESPHome-espidf_ble_keyboard`](https://github.com/markusg1234/ESPHome-espidf_ble_keyboard)、[`olegos76/nimble_kbdhid_example`](https://github.com/olegos76/nimble_kbdhid_example))を調べたところ、"**each host slot uses a unique BLE address**"(スロットごとに別々のBLEアドレスを使う)というのが定石だと判明。これを`ble_pair_slots.c`に実装:

- slot1: 今まで通りこの機体の実MAC(public address)。
- slot2, slot3: それぞれ固定のランダムスタティックアドレス(`C0:00:00:00:00:02`, `C0:00:00:00:00:03` - `host/ble_hs_id.h`の`ble_hs_id_set_rnd()`でセット、上位2bit`11`のstatic random要件を満たす値を選定)。

これで、slot2に切り替わってる間、KDEからは「見たことのない別のデバイス」にしか見えなくなり、干渉しなくなった。

副作用として、スロットが名乗るアドレス自体が変わるため、**この変更を入れた時点で既にslot2/3にボンド済みだった端末は再ペアリングが必要**だった。

## 症状2: Androidだけディレクテッドアドバタイズで繋がらない(タイムアウト)

アドレス分離後、KDE(slot1)は問題なく繋がるようになったが、Android(slot2)だけディレクテッドアドバタイズで一切繋がらなくなった(手動接続もタイムアウト)。

**原因**: `own_addr_type`に`BLE_OWN_ADDR_PUBLIC`/`BLE_OWN_ADDR_RANDOM`(固定アドレスそのまま)を使っていた。KDEは固定アドレスで動くBLEスタックなので問題ないが、**Androidは自分の側でLEプライバシー(ローテーションするresolvable private address = RPA)を使うのが一般的**。ディレクテッドアドバタイズはボンド時に記録した固定の識別アドレス宛に出すが、Androidは今そのアドレスでは電波を受信しておらず(ローテーション後の別アドレスになっている)ため、こちらの識別アドレス宛のADV_DIRECT_INDがAndroidの現在のアドレスに正しく解決されない限り届かない。

[`espressif/esp-nimble` issue #10](https://github.com/espressif/esp-nimble/issues/10)などの調査で、これは「自分(ペリフェラル)側の`own_addr_type`をRPA対応にして、コントローラのresolving listにボンド済みピアの鍵を使わせる」ことで解決するパターンだと分かった。

**対処**: `own_addr_type`を`BLE_OWN_ADDR_RPA_PUBLIC_DEFAULT`/`BLE_OWN_ADDR_RPA_RANDOM_DEFAULT`(スロット1/スロット2以降にそれぞれ対応)に変更。ボンド済みの相手は既にこちらのIRKをペアリング時に受け取ってる(`sm_our_key_dist`に`BLE_SM_PAIR_KEY_DIST_ID`が含まれてる)ので、こちらのアドレスがどう変わっても相手側は変わらず解決できる - 再ペアリングは不要だった。

これで実機確認、Android/KDEどちらの切り替えも成功。

## 副次的に見つかった、DSLスクリプト側のバグ

この一連の実機テスト中に、`wheel_to_udp_only3.rb`(ユーザーの実運用スクリプト)側にも2つの実バグが見つかり、あわせて直した:

- Shift+Metaコンボの判定に「押しっぱなし中の複数レポート」に対するエッジトリガーガードが無く、キーボードが押下中にレポートを繰り返し送る機種だと`ble_pair_new`/`ble_pair_switch`/`system_control(:sleep)`が1回の押下で何度も呼ばれていた。特に`after()`は呼ぶたびに新規タイマーを消費するため、`MRB_DSL_MAX_TIMERS`(4)をすぐ使い切ってしまっていた。→ `state[:ble][:active]`で「今どのアクションが押されっぱなしか」を追跡し、変化した時だけ発火するように修正。
- `ble_pair_slot`(現在アクティブなスロット番号を返すDSLメソッド)はidle時(unpair直後など)に`nil`を返す仕様だが、スクリプト側がこれをそのまま`ble_pair_new(slot)`に渡していたため、idle時に長押しすると`ble_pair_new(nil)`が呼ばれて内部で静かに失敗し、何も起きないまま終わっていた。→ `ble_pair_slot || 1`でnilガード。

## 未解決・今後の課題

- **Androidへの接続が体感で遅い**(KDEに比べ)。動作はする。ユーザー案: 接続自体は張りっぱなしにしておいて、別レイヤ(mrubyのDSL側?)で実際にどちらの入力を転送するか切り替える、というアプローチなら遅延を回避できるかもしれない - 未実装、今後の検討事項。
- ランダムスタティックアドレス(slot2/3)はコード内に固定でハードコードしてある(`BLE_PAIR_SLOT_COUNT`が変わったら手動で追記が必要)。

## 参考

- [[ble_multi_pair]] - スロット設計そのもの(ディレクテッドアドバタイズ、NVS構造)
- [[after_timer_dsl]] - 追記1〜6に、ここに至るまでの試行錯誤(revertしたものも含む)の詳細ログ
- [markusg1234/ESPHome-espidf_ble_keyboard](https://github.com/markusg1234/ESPHome-espidf_ble_keyboard) - スロットごとに別アドレスを使うマルチホスト実装例
- [olegos76/nimble_kbdhid_example](https://github.com/olegos76/nimble_kbdhid_example) - ESP32 NimBLE HIDキーボードの基礎実装例
- [espressif/esp-nimble#10](https://github.com/espressif/esp-nimble/issues/10) - RPA使用ピアとのボンド後再接続に関する既知の問題
- `esp32-kvm-ip/main/ble_pair_slots.c`の`s_slot_own_addr`/`own_addr_for_slot()`
- `esp32-kvm-ip/main/esp_hid_gap.c`の`esp_hid_ble_gap_adv_start()`
- `data/mruby_scripts/examples/wheel_to_udp_only3.rb`
