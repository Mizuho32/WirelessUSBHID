#!/usr/bin/env bash
if ! lsmod | grep -q usbmon; then
	sudo modprobe usbmon
	sudo mount -t debugfs none /sys/kernel/debug
fi
sudo ls /sys/kernel/debug/usb/usbmon
