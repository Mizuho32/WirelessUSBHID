#!/usr/bin/env python3
"""Upload a new firmware image to a Host-role (KVM_ROLE=HOST) board's
currently-inactive OTA slot over WiFi (POST /api/firmware) - no serial
cable or Boot-mode button needed. See mds/usb_hid/2026-09-10_wifi_ota.md.

Usage:
  bin/upload_firmware.py --host 192.168.0.42
  bin/upload_firmware.py --host esp32-kvm-ip-host.local path/to/esp32-kvm-ip.bin

Wraps a plain HTTP POST (urllib, no extra dependencies - same
no-external-deps approach as server/server.py) - equivalent to
main/webui/index.html's "Update firmware" button, just scriptable. The
board (main/ota_updater.c) writes the whole body straight to whichever
OTA slot (partitions.csv's ota_0/ota_1) isn't currently running and
reboots into it once written. If that new image never confirms itself
(crashes/resets before WiFi reconnects - wifi_manager.c's
esp_ota_mark_app_valid_cancel_rollback() call), the bootloader
automatically reverts to whichever slot was running before this upload
(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) - see the design doc for details.

If FILE is omitted, defaults to this repo's own
esp32-kvm-ip/build.host/esp32-kvm-ip.bin (bin/build_host.sh's own build
output).
"""
import argparse
import os
import sys
import urllib.error
import urllib.request

DEFAULT_IMAGE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                              "esp32-kvm-ip", "build.host", "esp32-kvm-ip.bin")


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("file", nargs="?", default=DEFAULT_IMAGE,
                         help=f"Path to the .bin to upload (default: {DEFAULT_IMAGE})")
    parser.add_argument("--host", required=True, help="Board's IP address or hostname")
    parser.add_argument("--port", type=int, default=80, help="WebUI HTTP port (default: 80)")
    parser.add_argument("--timeout", type=float, default=120,
                         help="Upload timeout in seconds (default: 120 - a slow WiFi link/flash "
                              "write can take a while for a multi-MB image)")
    parser.add_argument("-y", "--yes", action="store_true", help="Skip the confirmation prompt")
    args = parser.parse_args()

    if not os.path.isfile(args.file):
        sys.exit(f"{args.file} not found - build it first (bin/build_host.sh build)")
    with open(args.file, "rb") as f:
        payload = f.read()

    if not args.yes:
        reply = input(f"Flash {args.file} ({len(payload)} bytes) to "
                       f"{args.host}:{args.port} and reboot it? [y/N] ")
        if reply.strip().lower() not in ("y", "yes"):
            sys.exit("Aborted.")

    url = f"http://{args.host}:{args.port}/api/firmware"
    print(f"+ POST {url} ({len(payload)} bytes)")
    req = urllib.request.Request(url, data=payload, method="POST")
    req.add_header("Content-Type", "application/octet-stream")
    try:
        with urllib.request.urlopen(req, timeout=args.timeout) as resp:
            print(resp.read().decode("utf-8", "replace").strip())
    except urllib.error.HTTPError as e:
        sys.exit(f"upload failed: HTTP {e.code} {e.read().decode('utf-8', 'replace').strip()}")
    except urllib.error.URLError as e:
        sys.exit(f"upload failed: {e.reason}")

    print("Board is rebooting into the new firmware - give it a few seconds, "
          "then check the WebUI's \"firmware:\" status line once it's back.")


if __name__ == "__main__":
    main()
