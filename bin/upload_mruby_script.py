#!/usr/bin/env python3
"""Upload a file to a Host-role (KVM_ROLE=HOST) board's raw storage
partition over serial, without rebuilding/reflashing the app image - see
mds/usb_hid/2026-08-29_mruby_phase1_impl.md.

Usage:
  bin/upload_mruby_script.py --port /dev/ttyUSB0 path/to/script.rb
  bin/upload_mruby_script.py --port /dev/ttyUSB0 --partition webui_html path/to/index.html

Wraps ESP-IDF's parttool.py (write_partition): prepends the 4-byte
little-endian length header this project's raw-partition readers expect
(main/mruby_filter.c's load_uploaded_script()/mruby_filter_read_script(),
main/mruby_webui.c's read_webui_html_partition()), then writes the result
to the named partition (esp32-kvm-ip/partitions.csv, default:
mrb_script). The max size is looked up from partitions.csv itself (its
size minus the 4-byte header), not hardcoded here, so it stays in sync
if that file changes.

--partition mrb_script (default): an mruby script. The board picks it up
on its next boot/reset - mruby_filter_init() reads mrb_script before
falling back to the embedded main/mruby_scripts/default.rb.

--partition webui_html: the WebUI's own frontend page (see
mds/usb_hid/2026-08-30_mruby_phase2_webui.md). Takes effect immediately,
no reset needed - main/mruby_webui.c's GET / handler reads this
partition fresh on every request.

Requires ESP-IDF's environment to be sourced first (parttool.py needs
$IDF_PATH) - same prerequisite as bin/build_host.sh:
  export ESP_IDF=/opt/esp-idf && source "$ESP_IDF/export.sh"
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile

HEADER_SIZE = 4  # 4-byte little-endian length, see module docstring


def parse_size(text):
    """Parses partitions.csv-style size fields ('64K', '3M', '4096')."""
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
    parser.add_argument("file", help="Path to the file to upload (.rb script or .html page)")
    parser.add_argument("--port", required=True, help="Serial port (e.g. /dev/ttyUSB0)")
    parser.add_argument("--baud", default="460800", help="Baud rate (default: 460800)")
    parser.add_argument("--partition", default="mrb_script",
                         help="Target partition name (default: mrb_script; see esp32-kvm-ip/partitions.csv)")
    args = parser.parse_args()

    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        sys.exit("IDF_PATH not set - source ESP-IDF's export.sh first (see bin/build_host.sh)")
    parttool = os.path.join(idf_path, "components", "partition_table", "parttool.py")
    if not os.path.isfile(parttool):
        sys.exit(f"parttool.py not found at {parttool} - check IDF_PATH")

    csv_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "esp32-kvm-ip", "partitions.csv")
    part_size = partition_size(csv_path, args.partition)
    if part_size is None:
        sys.exit(f"partition '{args.partition}' not found in {csv_path}")
    max_size = part_size - HEADER_SIZE

    with open(args.file, "rb") as f:
        file_bytes = f.read()

    if len(file_bytes) > max_size:
        sys.exit(f"{args.file} is {len(file_bytes)} bytes, max is {max_size} "
                  f"('{args.partition}' partition is {part_size} bytes in partitions.csv - "
                  "enlarge it there if you need more)")

    with tempfile.NamedTemporaryFile(suffix=".bin") as tmp:
        tmp.write(struct.pack("<I", len(file_bytes)))
        tmp.write(file_bytes)
        tmp.flush()

        cmd = [
            sys.executable, parttool,
            "--port", args.port,
            "--baud", args.baud,
            "write_partition",
            "--partition-name", args.partition,
            "--input", tmp.name,
        ]
        print("+", " ".join(cmd))
        subprocess.run(cmd, check=True)

    print(f"Uploaded {args.file} ({len(file_bytes)} bytes) to {args.partition}.")
    if args.partition == "mrb_script":
        print("Reset/power-cycle the board to load it (read at boot in mruby_filter_init()).")
    elif args.partition == "webui_html":
        print("No reset needed - main/mruby_webui.c reads this partition fresh on every request.")


if __name__ == "__main__":
    main()
