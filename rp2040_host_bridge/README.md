# RP2040 host bridge

See `mds/2026-08-23_rp2040_as_host_bridge_plan.md` for what this is and
why (MAX3421E replacement/alternative - RP2040 does the actual USB Host
role itself and forwards decoded HID events to the ESP32-S3 over UART).

## Build / flash

Shared with `rp2040_hello_world/`/`rp2040_host_check/` via
`bin/build_flash_rp2040.sh` (board = Raspberry Pi Pico):

```
bin/build_flash_rp2040.sh rp2040_host_bridge tinyusb_host build
bin/build_flash_rp2040.sh rp2040_host_bridge tinyusb_host flash
```

`flash` uploads via Picotool, which needs the board already sitting in
BOOTSEL mode (hold BOOTSEL while plugging in/resetting).

## Wiring

- USB Host port: same VBUS-jumper setup as `rp2040_host_check/` (see
  `mds/2026-08-22_rp2040_host_check.md` - the board's own VBUS pin is
  diode-blocked for device-only power, so a separate 5V jumper directly
  to VBUS is required for the dongle to see power).
- ESP32 link: `Serial1` (GP0=TX, GP1=RX by default on most rp2040 boards)
  to the ESP32-S3's bridge UART pins (`BRIDGE_UART_TX_PIN`/
  `BRIDGE_UART_RX_PIN` in `esp32-kvm-ip/main/usb_host_rp2040_bridge.c` -
  cross TX/RX between the two boards, plus a shared GND). Adjust either
  side's pin numbers to match actual wiring - the ones in the ESP32 code
  are placeholders, same as MAX3421E's pins were before that was wired up.

## No human-readable debug output

Unlike `rp2040_host_check/` (which is still the sketch to use for that),
this one uses `Serial1` entirely for the binary bridge protocol - there's
no free UART left for text logs. If you need to debug this sketch in
isolation (without an ESP32 on the other end), use
`rp2040_host_check/` first to confirm the dongle itself behaves, then
come back to this one.
