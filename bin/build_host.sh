#!/usr/bin/env bash
# Build esp32-kvm-ip in Host role (USB Host: reads a physical
# keyboard/mouse and forwards it over WiFi/UDP to a Device-role board -
# see mds/usb_hid/2026-08-21_usb_host.md). Uses its own build.host/ directory so
# switching back and forth with bin/build_device.sh never forces a
# full rebuild (each role keeps its own ninja/ccache state).
#
# Usage: bin/build_host.sh [idf.py args...]
#   bin/build_host.sh build flash monitor

if ! command -v idf.py; then
 source "${ESP_IDF}/export.sh"
fi

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../esp32-kvm-ip"
exec idf.py -B build.host -D KVM_ROLE=HOST "$@"
