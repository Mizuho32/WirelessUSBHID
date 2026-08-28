#!/usr/bin/env python3
"""Upload an mruby script to a Host-role (KVM_ROLE=HOST) board's
mrb_script partition over serial, without rebuilding/reflashing the app
image - see mds/usb_hid/2026-08-29_mruby_phase1_impl.md.

Usage:
  bin/upload_mruby_script.py --port /dev/ttyUSB0 path/to/script.rb

Wraps ESP-IDF's parttool.py (write_partition): prepends the 4-byte
little-endian length header main/mruby_filter.c's load_uploaded_script()
expects, then writes the result to the mrb_script partition
(esp32-kvm-ip/partitions.csv). The board picks it up on its next
boot/reset - mruby_filter_init() reads mrb_script before falling back to
the embedded main/mruby_scripts/default.rb.

Requires ESP-IDF's environment to be sourced first (parttool.py needs
$IDF_PATH) - same prerequisite as bin/build_host.sh:
  export ESP_IDF=/opt/esp-idf && source "$ESP_IDF/export.sh"
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

# Must match partitions.csv's mrb_script size (64K) minus the 4-byte
# length header load_uploaded_script() (main/mruby_filter.c) expects.
MAX_SCRIPT_SIZE = 64 * 1024 - 4


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("script", help="Path to the .rb file to upload")
    parser.add_argument("--port", required=True, help="Serial port (e.g. /dev/ttyUSB0)")
    parser.add_argument("--baud", default="460800", help="Baud rate (default: 460800)")
    args = parser.parse_args()

    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        sys.exit("IDF_PATH not set - source ESP-IDF's export.sh first (see bin/build_host.sh)")
    parttool = os.path.join(idf_path, "components", "partition_table", "parttool.py")
    if not os.path.isfile(parttool):
        sys.exit(f"parttool.py not found at {parttool} - check IDF_PATH")

    with open(args.script, "rb") as f:
        script_bytes = f.read()

    if len(script_bytes) > MAX_SCRIPT_SIZE:
        sys.exit(f"{args.script} is {len(script_bytes)} bytes, max is {MAX_SCRIPT_SIZE} "
                 "(mrb_script partition is 64K in partitions.csv - enlarge it there if you need more)")

    with tempfile.NamedTemporaryFile(suffix=".bin") as tmp:
        tmp.write(struct.pack("<I", len(script_bytes)))
        tmp.write(script_bytes)
        tmp.flush()

        cmd = [
            sys.executable, parttool,
            "--port", args.port,
            "--baud", args.baud,
            "write_partition",
            "--partition-name", "mrb_script",
            "--input", tmp.name,
        ]
        print("+", " ".join(cmd))
        subprocess.run(cmd, check=True)

    print(f"Uploaded {args.script} ({len(script_bytes)} bytes) to mrb_script.")
    print("Reset/power-cycle the board to load it (read at boot in mruby_filter_init()).")


if __name__ == "__main__":
    main()
