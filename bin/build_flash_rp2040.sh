#!/usr/bin/env bash
# Build (and optionally flash) an RP2040 Arduino sketch - shared by all
# rp2040_*/ sketches (rp2040_hello_world/, rp2040_host_check/, ...). See
# mds/usb_hid/2026-08-22_rp2040_host_check.md.
#
# Board = "Raspberry Pi Pico" (confirmed to actually boot on this
# project's compatible board - a different board id, e.g. "Generic
# RP2040", was tried first and never actually ran despite building
# cleanly). Builds directly into /tmp via --build-path instead of
# --export-binaries - an exported copy in the (NFS-mounted) project
# directory didn't match a fresh build from the same sketch+FQBN in at
# least one observed case. Upload method is Picotool, which talks
# directly to a device already sitting in BOOTSEL mode over USB - no
# serial port needed (useful since rp2040_host_check's sketch has no CDC
# Serial once it's acting as USB host) - so the board must already be in
# BOOTSEL mode (hold BOOTSEL while plugging in/resetting) before `flash`
# will find it.
#
# Usage:
#   bin/build_flash_rp2040.sh <sketch-dir> <usbstack> [build|flash]
#
#   bin/build_flash_rp2040.sh rp2040_hello_world picosdk build
#   bin/build_flash_rp2040.sh rp2040_host_check tinyusb_host flash
#
# <usbstack> is the rp2040:rp2040 core's USB Stack board option
# (picosdk, tinyusb, tinyusb_host, or nousb).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

SKETCH_DIR="${1:?Usage: $0 <sketch-dir> <usbstack> [build|flash]}"
USBSTACK="${2:?Usage: $0 <sketch-dir> <usbstack> [build|flash]}"
ACTION="${3:-build}"
SKETCH_NAME="$(basename "$SKETCH_DIR")"

if command -v arduino-cli >/dev/null 2>&1; then
    CLI=arduino-cli
else
    CLI=$(find /var/lib/flatpak/app/cc.arduino.IDE2 -type f -name arduino-cli 2>/dev/null | head -1)
    if [ -z "$CLI" ]; then
        echo "arduino-cli not found (checked PATH and the flatpak Arduino IDE 2 install)" >&2
        exit 1
    fi
fi

FQBN="rp2040:rp2040:rpipico:usbstack=${USBSTACK},uploadmethod=picotool"
BUILD_DIR="/tmp/arduino_build_${SKETCH_NAME}"
UF2_NAME="${SKETCH_NAME}.ino.uf2"

"$CLI" compile --fqbn "$FQBN" --build-path "$BUILD_DIR" "$SKETCH_DIR"

case "$ACTION" in
    build)
        ;;
    flash)
        "$CLI" upload --fqbn "$FQBN" -i "$BUILD_DIR/$UF2_NAME"
        ;;
    *)
        echo "Usage: $0 <sketch-dir> <usbstack> [build|flash]" >&2
        exit 1
        ;;
esac
