#!/usr/bin/env bash
# Build esp32-kvm-ip in Host role (USB Host: reads a physical
# keyboard/mouse and forwards it over WiFi/UDP to a Device-role board -
# see mds/usb_hid/2026-08-21_usb_host.md). Uses its own build.host/ directory so
# switching back and forth with bin/build_device.sh never forces a
# full rebuild (each role keeps its own ninja/ccache state).
#
# Usage: bin/build_host.sh [idf.py args...]
#   bin/build_host.sh build flash monitor
#
# `flash -p <target>` is special-cased: if <target> looks like a serial
# device (/dev/..., COM<N>), this is passed straight through to idf.py
# as always (esptool over the cable). If it *doesn't* - anything else,
# e.g. an IP address or a .local hostname - this is treated as a WiFi OTA
# upload instead (mds/usb_hid/2026-09-10_wifi_ota.md): `flash` is dropped
# and bin/upload_firmware.py --host <target> is run against whatever's
# already sitting in build.host/esp32-kvm-ip.bin. No Boot-mode button, no
# cable - the board just needs to already be reachable on the network.
#
# Unlike serial flashing, this does NOT run idf.py build first unless you
# ask for it - `flash -p <wifi-host>` alone re-uploads the existing
# build.host/esp32-kvm-ip.bin as-is (as fast as calling upload_firmware.py
# directly), while `build flash -p <wifi-host>` rebuilds first, same as
# typing both idf.py targets explicitly.

if ! command -v idf.py; then
 source "${ESP_IDF}/export.sh"
fi

set -euo pipefail
bin_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$bin_dir/../esp32-kvm-ip"

args=("$@")
filtered=()
wifi_host=""
want_flash=false
want_build=false
i=0
while [ $i -lt ${#args[@]} ]; do
    a="${args[$i]}"
    case "$a" in
        flash)
            want_flash=true
            filtered+=("$a")
            ;;
        build)
            want_build=true
            filtered+=("$a")
            ;;
        -p|--port)
            i=$((i + 1))
            val="${args[$i]:-}"
            if [[ "$val" == /dev/* || "$val" == COM* ]]; then
                filtered+=("$a" "$val")
            else
                wifi_host="$val"
            fi
            ;;
        *)
            filtered+=("$a")
            ;;
    esac
    i=$((i + 1))
done

if [ "$want_flash" = true ] && [ -n "$wifi_host" ]; then
    echo "WiFi OTA mode: uploading to $wifi_host instead of a serial flash (see this script's own comment)"
    if [ "$want_build" = true ]; then
        build_only=()
        for a in "${filtered[@]}"; do
            [ "$a" = "flash" ] || build_only+=("$a")
        done
        idf.py -B build.host -D KVM_ROLE=HOST "${build_only[@]}"
    fi
    exec "$bin_dir/upload_firmware.py" --host "$wifi_host" build.host/esp32-kvm-ip.bin
fi

exec idf.py -B build.host -D KVM_ROLE=HOST "${filtered[@]}"
