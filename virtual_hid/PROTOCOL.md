# プロトコル

`server/`が受け取るUDPパケットと、`client/`がWebSocket越しに受け取るバイナリは
**同一フォーマット**。`server/`はこのフォーマットを一切解釈せず(先頭2byteの
magic値の確認のみ)、UDPで受けたバイト列をそのままWebSocketクライアント全員へ
ブロードキャストするだけの薄い中継。

`esp32-kvm-ip/server/protocol.py`と同じ16byte固定・リトルエンディアン形式
(そちらとは独立した実装。esp32-kvm-ip/はこのプロジェクトから一切参照・変更しない)。

```
オフセット  サイズ  フィールド
0           2       magic (0xCAFE, uint16 LE)
2           4       sequence (uint32 LE, 未使用でも可)
6           1       type (0x01=mouse, 0x02=keyboard, 0x03=consumer)
7           1       pad
8           8       payload (typeにより異なる)
```

## mouse (type=0x01)
```
8   1   buttons   (bit0=left, bit1=right, bit2=middle, bit3=X1(戻る), bit4=X2(進む))
9   2   dx        (int16 LE)
11  2   dy        (int16 LE)
13  1   wheel     (int8, 縦ホイール)
14  1   pan       (int8, 横ホイール)
15  1   pad
```

## keyboard (type=0x02)
```
8   1   modifiers (USB HID Bootキーボードのmodifierバイトと同じビット配置)
9   1   reserved
10  6   keycodes  (USB HID Usage ID、最大6キー同時押し。未使用は0x00)
```

## consumer (type=0x03)
```
8   2   usage_id  (USB HID Consumer Page usage、uint16 LE)
10  6   pad
```

対応するusage_idはひとまず以下を実装(`client/`のHidKeycodeMap参照):
`0xE9`(Volume Up) `0xEA`(Volume Down) `0xE2`(Mute)
`0xCD`(Play/Pause) `0xB5`(Next Track) `0xB6`(Prev Track) `0xB7`(Stop)

## WebSocket
- `server/`はTLS終端をしない。リバースプロキシ(nginx/caddy等)の裏で平文`ws://`のみ待受け。
- 接続時のクエリパラメータ`?token=...`で認証(`--token`未指定時は無認証、警告ログのみ)。
- メッセージは常にbinary frame。1メッセージ = 1パケット(16byte)。
