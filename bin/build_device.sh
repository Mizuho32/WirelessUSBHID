#!/usr/bin/env bash
# Build esp32-kvm-ip in Device role (default/original behavior: USB HID
# device connected to the Target PC). Uses the project's default build/
# directory. See mds/usb_hid/2026-08-21_usb_host.md for why role is a plain CMake
# variable instead of a Kconfig choice, and bin/build_host.sh for the
# other role.
#
# Usage: bin/build_device.sh [idf.py args...]
#   bin/build_device.sh build flash monitor
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../esp32-kvm-ip"
exec idf.py -D KVM_ROLE=DEVICE "$@"
