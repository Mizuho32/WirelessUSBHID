# RP2040 host check

See `mds/usb_hid/2026-08-22_rp2040_host_check.md` for what this is and why.

## Build / flash

Shared with `rp2040_hello_world/` via `bin/build_flash_rp2040.sh` (board =
Raspberry Pi Pico, confirmed to actually boot on this project's
compatible board - see `rp2040_hello_world/`'s bring-up check):

```
bin/build_flash_rp2040.sh rp2040_host_check tinyusb_host build
bin/build_flash_rp2040.sh rp2040_host_check tinyusb_host flash
```

`flash` uploads via Picotool, which needs the board already sitting in
BOOTSEL mode (hold BOOTSEL while plugging in/resetting).

If Picotool can't open the device (`Maybe try 'sudo' or check your
permissions`), run `rp2040_hello_world/udev_setup.sh` once (needs sudo)
to allow non-root access to the RP2040's USB bootloader interface, then
replug and try again - no `sudo` needed after that.

## Debug output: Serial1, not Serial

This sketch has no CDC Serial once it's acting as USB host (RP2040 has
only one native USB peripheral, and it's busy being Host) - all output
goes over `Serial1` (hardware UART, usually GP0=TX/GP1=RX). Wire that to
a USB-serial adapter or FTDI cable to read it.
