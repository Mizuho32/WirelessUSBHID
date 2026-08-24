#!/usr/bin/env bash
# Capture USB traffic on the bus the Maxxter 9-button mouse dongle is on,
# to compare its enumeration/control-transfer sequence with what ESP32's
# usb_host_task.c does. See mds/usb_hid/2026-08-22_wireless_dongle_short_reports.md.
#
# Uses tcpdump directly against the kernel's usbmon interface - no
# Wireshark GUI or tshark needed to CAPTURE (tcpdump has native support
# for the "USB Linux" linktype). Run enable_usbmon.sh once first.
#
# Procedure:
#   1. Unplug the Maxxter dongle.
#   2. Run this script (it starts capturing immediately).
#   3. Plug the dongle back in, wait a few seconds for enumeration +
#      one or two mouse moves/clicks/wheel turns, then Ctrl-C.
#   4. Analyze: open capture.pcap in Wireshark, or see README.md for a
#      no-GUI option.
#
# Usage: wireshark_9btn_mouse/capture.sh [bus] [output.pcap]
#   bus defaults to 1 (Bus 001 in `lsusb`, where the dongle showed up
#   in this project's testing so far - check `lsusb` if it moved).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
BUS="${1:-1}"
OUT="${2:-capture.pcap}"
echo "Capturing usbmon${BUS} -> ${OUT} (Ctrl-C to stop once the dongle is replugged and exercised)"
sudo tcpdump -i "usbmon${BUS}" -w "/tmp/$OUT"
sudo chown $(whoami):$(whoami) "/tmp/$OUT"
mv -i "/tmp/$OUT" ./
