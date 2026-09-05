# WiFi認証情報のUART書き込み化

`main/wifi_credentials.h`にSSID/パスワードを平文で書く方式を廃止。`mrb_script`/`webui_html`と同じ生パーティション方式(4バイト長ヘッダ+データ)で、`bin/upload_wifi_credentials.py`経由でシリアル書き込みするようにした。`wifi_credentials.h`には`KVM_TARGET_HOST`(Hostロールの転送先)のみ残置。

## 実装

- `partitions.csv`: `wifi_cred`パーティション追加(subtype 0x52、**4K**)。中身はプレーンテキスト`SSID\nPASSWORD\nHOSTNAME\n`(hostname行省略可)
- `wifi_manager.c/.h`: `wifi_manager_load_credentials()`新設。パーティションが無い/未書き込みならログを出して空文字列を返す(コンパイル時フォールバックは無し - プレースホルダのSSIDはどのみち繋がらないので意味が無い)
- `main.c`/`main_host.c`: 起動時にこれを呼んでから`wifi_manager_start()`へ渡す形に変更
- `bin/upload_wifi_credentials.py`(新規): `--ssid`/`--hostname`は引数、**パスワードは`getpass`で対話入力**(コマンドライン引数には一切乗らない、シェル履歴/`ps`に残らない)

## ハマりどころ

`wifi_cred`を最初256バイトにしたら`parttool.py write_partition`が`Size of data to erase must be a multiple of 4096`で失敗した。`write_partition`は書き込み前にパーティション全体を消去するため、esptoolの`erase_region`がフラッシュの消去セクタサイズ(4096バイト)の倍数を要求する。`mrb_script`(64K)/`webui_html`(32K)は元々4Kの倍数だったので気づかなかった。**data系の生パーティションは4Kの倍数にする**、が教訓。4Kに変更して解決。

## 使い方

```
idf.py flash   # partition table込みのフルフラッシュが必要(初回・スキーマ変更時)
bin/upload_wifi_credentials.py --port /dev/ttyUSB0 --ssid "My WiFi" --hostname esp32-kvm-ip
# パスワードは対話プロンプトで入力
```

実機確認済み。

## 参考

- 関連する既存パーティション方式: `mds/usb_hid/2026-08-29_mruby_phase1_impl.md`(mrb_script)、`mds/usb_hid/2026-08-30_mruby_phase2_webui.md`(webui_html)
