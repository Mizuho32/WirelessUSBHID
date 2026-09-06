# RP2040/RP2350 単体 + PIO Device 化についての雑談メモ (2026-09-06)

実装予定なし、単なる雑談の記録。別プロジェクトとしてやるならの話。

## 発端

ここまでのRP2040ブリッジ + ESP32-S3という2チップ構成に対して、「RP2040(2350)
1個 + USBコネクタ増設だけで同じことできない?」という疑問。

## RP2040/RP2350のUSB Host+Device同時可否

- RP2040: ネイティブUSBペリフェラルは1個のみ。Host/Deviceどちらのロールにも
  使えるが、同時には無理(このプロジェクトの既存コード内コメントで既に
  言及済み: 「RP2040 has a single native USB peripheral, so once it's
  acting as Host, the same port can't also be a USB-CDC Serial port」と
  同じ制約)。
- RP2350も同様: 公式Product Briefに "1 × USB 1.1 controller and PHY,
  with host and device support" とあり、デュアルロール対応だが1コント
  ローラのみ = 同時不可は変わらず。

## PIOでもう1ポート増やす案

`sekigon-gonnoc/Pico-PIO-USB` というライブラリが実在し、PIOステートマシン+
予備GPIOで2つ目のUSB Host/Deviceポートをソフト(ビットバンク)で提供できる。
`Adafruit_TinyUSB_Arduino`(このプロジェクトが既に依存)と統合済み。実例:
`RP2350-USB-A`ボード。ただし検索で見つけたRP2350-USB-Aの実例記事による
と、PIO-USBをHostとして使う際の信頼性問題(動かない/直した話)が実際に
報告されている。

HIDのような低速デバイスなら帯域的には問題なさそうで、電力は多少余計に
食う見込み(PIO自体+関連クロックが常時稼働するため)。

## PIO Device側でのsuspend検出 + light/dormant sleepの可否

このプロジェクトが実際にESP32/RP2040双方で実装・実機確認済みの
「USB busのsuspendを検出してsleepに入る」を、PIOベースのDevice実装でも
再現できるか、というのが最後の疑問。結論は「怪しい」:

- TinyUSBの`tud_suspend_cb()`/`tud_resume_cb()`自体は転送方式に依存しない
  一般的な仕組みで、`tud_task()`のポーリングループから呼ばれる。コール
  バックの入り口自体はPIO-USBでも当然存在する。
- ただし「SOFが3ms途絶えたらsuspend」の判定は、ネイティブUSBコントローラ
  なら専用ハードウェア(SIE)割り込みが検出してくれるのに対し、PIO-USBは
  ビットバンクなのでソフトウェアで経過時間を測るしかないはず。
  Pico-PIO-USB側がその検出ロジックを実際にどこまで作り込んでいるかは、
  検索した範囲では確認が取れなかった(データ転送周りが主眼のライブラリ
  という印象で、suspend検出の作り込みは未検証寄り)。
- さらに構造的な問題として、PIOのステートマシンはクロックが動いてないと
  存在できない。つまりdormant(クロック完全停止)に入るには、そのPIO
  ポート自体を止める必要があり、「PIOで動いているHost側入力を維持し続け
  たまま同時にdormantに入る」ことと本質的に両立しない。今回のRP2040
  ブリッジがdormant中は本当にUART受信すら止めて、専用GPIO割り込み
  (dormant IRQ)だけで起こす設計([[2026-09-02_rp2040_sleep_impl]]参照)
  になっているのと同じ理屈で、PIO自身にはdormant中の「起こし役」は
  果たせない。

つまり: コールバックの仕組み自体は転送方式非依存で存在するが、(1)
PIO-USBの suspend 検出実装の信頼性が未確認、(2) 仮に検出できても
PIOポート自体はdormant中維持できないという二重の理由で、今回やったのと
同等のsuspend検出→sleep構成をPIO Device単体で再現するのは怪しい、という
のがこの雑談の結論。

## 参考

- [How to deal with USB suspend? - Raspberry Pi Forums](https://forums.raspberrypi.com/viewtopic.php?t=375853)
- [tud_suspend_cb() - TinyUSB source reference](https://sourcevu.sysprogs.com/rp2040/lib/tinyusb/symbols/tud_suspend_cb)
- Raspberry Pi Pico 2 W / RP2350 Product Brief (公式PDF)
- `sekigon-gonnoc/Pico-PIO-USB` (GitHub)
