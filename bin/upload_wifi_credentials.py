#!/usr/bin/env python3
"""Upload WiFi credentials to a board's wifi_cred partition over serial,
without rebuilding/reflashing the app image and without ever putting them
in esp32-kvm-ip/main/wifi_credentials.h (that file no longer holds real
WiFi credentials at all - see wifi_manager.h's
wifi_manager_load_credentials()).

Usage:
  bin/upload_wifi_credentials.py --port /dev/ttyUSB0 --ssid "My WiFi"
  bin/upload_wifi_credentials.py --port /dev/ttyUSB0 --ssid "My WiFi" --hostname esp32-kvm-ip

The password is never accepted as a command-line argument - it would sit
in shell history and be visible to any other process on the machine via
`ps`. This always prompts for it interactively with echo disabled
(getpass), same as `passwd`/`ssh-keygen` etc.

Wraps ESP-IDF's parttool.py (write_partition), same scheme as
bin/upload_mruby_script.py: a 4-byte little-endian length header, then
plain UTF-8 "SSID\\nPASSWORD\\nHOSTNAME\\n" (hostname line omitted if not
given), written to the wifi_cred partition
(esp32-kvm-ip/partitions.csv). Read at boot by wifi_manager.c's
wifi_manager_load_credentials() - takes effect on next boot/reset.

Requires ESP-IDF's environment to be sourced first (parttool.py needs
$IDF_PATH) - same prerequisite as bin/build_host.sh:
  export ESP_IDF=/opt/esp-idf && source "$ESP_IDF/export.sh"
"""
import argparse
import getpass
import os
import re
import struct
import subprocess
import sys
import tempfile

HEADER_SIZE = 4  # 4-byte little-endian length, see module docstring
PARTITION_NAME = "wifi_cred"


def parse_size(text):
    """Parses partitions.csv-style size fields ('64K', '3M', '256')."""
    text = text.strip()
    m = re.match(r"^(0x[0-9a-fA-F]+|\d+)([kKmM]?)$", text)
    if not m:
        raise ValueError(f"unrecognized size '{text}'")
    value = int(m.group(1), 0)
    suffix = m.group(2).lower()
    if suffix == "k":
        value *= 1024
    elif suffix == "m":
        value *= 1024 * 1024
    return value


def partition_size(csv_path, name):
    """Returns the named partition's size (bytes) from partitions.csv, or None."""
    with open(csv_path) as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            fields = [x.strip() for x in line.split(",")]
            if fields[0] == name:
                return parse_size(fields[4])
    return None


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", required=True, help="Serial port (e.g. /dev/ttyUSB0)")
    parser.add_argument("--baud", default="460800", help="Baud rate (default: 460800)")
    parser.add_argument("--ssid", required=True, help="WiFi network name")
    parser.add_argument("--hostname", default="",
                         help="Optional DHCP/netif hostname (default: leave the chip's own default alone)")
    args = parser.parse_args()

    if "\n" in args.ssid or "\n" in args.hostname:
        sys.exit("--ssid/--hostname must not contain newlines")

    password = getpass.getpass("WiFi password (leave empty for an open network): ")
    if "\n" in password:
        sys.exit("password must not contain newlines")

    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        sys.exit("IDF_PATH not set - source ESP-IDF's export.sh first (see bin/build_host.sh)")
    parttool = os.path.join(idf_path, "components", "partition_table", "parttool.py")
    if not os.path.isfile(parttool):
        sys.exit(f"parttool.py not found at {parttool} - check IDF_PATH")

    csv_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "esp32-kvm-ip", "partitions.csv")
    part_size = partition_size(csv_path, PARTITION_NAME)
    if part_size is None:
        sys.exit(f"partition '{PARTITION_NAME}' not found in {csv_path}")
    max_size = part_size - HEADER_SIZE

    payload = args.ssid.encode("utf-8") + b"\n" + password.encode("utf-8") + b"\n"
    if args.hostname:
        payload += args.hostname.encode("utf-8") + b"\n"

    if len(payload) > max_size:
        sys.exit(f"credentials are {len(payload)} bytes, max is {max_size} "
                  f"('{PARTITION_NAME}' partition is {part_size} bytes in partitions.csv - "
                  "enlarge it there if you need more)")

    with tempfile.NamedTemporaryFile(suffix=".bin") as tmp:
        tmp.write(struct.pack("<I", len(payload)))
        tmp.write(payload)
        tmp.flush()

        cmd = [
            sys.executable, parttool,
            "--port", args.port,
            "--baud", args.baud,
            "write_partition",
            "--partition-name", PARTITION_NAME,
            "--input", tmp.name,
        ]
        print("+", " ".join(cmd))  # nothing sensitive here - the password never touches argv/cmd
        subprocess.run(cmd, check=True)

    print(f"Uploaded WiFi credentials (SSID '{args.ssid}') to {PARTITION_NAME}.")
    print("Reset/power-cycle the board to use them (read at boot by wifi_manager_load_credentials()).")


if __name__ == "__main__":
    main()
