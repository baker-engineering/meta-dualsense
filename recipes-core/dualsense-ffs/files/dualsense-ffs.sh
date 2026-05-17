#!/bin/bash
# Set up the configfs USB gadget skeleton for a FunctionFS-backed DualSense.
# This runs before dualsense-ffsd. The daemon itself writes descriptors via
# /dev/ffs0/ep0 and binds the UDC once endpoints are ready.
set -euo pipefail

GADGET=/sys/kernel/config/usb_gadget/dualsense
FFS_MNT=/dev/ffs0

mountpoint -q /sys/kernel/config || mount -t configfs none /sys/kernel/config

# Tear down any prior instance.
if [[ -d "$GADGET" ]]; then
    [[ -f "$GADGET/UDC" ]] && echo "" > "$GADGET/UDC" || true
    find "$GADGET/configs"/*/* -maxdepth 0 -type l -exec rm -f {} + 2>/dev/null || true
    rmdir "$GADGET"/configs/*/strings/* 2>/dev/null || true
    rmdir "$GADGET"/configs/* 2>/dev/null || true
    rmdir "$GADGET"/functions/* 2>/dev/null || true
    rmdir "$GADGET"/strings/* 2>/dev/null || true
    rmdir "$GADGET" 2>/dev/null || true
fi
mountpoint -q "$FFS_MNT" && umount "$FFS_MNT" || true

# Configfs skeleton: VID/PID, strings, config, FFS function link.
mkdir -p "$GADGET"
echo 0x054C > "$GADGET/idVendor"
echo 0x0CE6 > "$GADGET/idProduct"
echo 0x0100 > "$GADGET/bcdDevice"
echo 0x0200 > "$GADGET/bcdUSB"
echo 0x00   > "$GADGET/bDeviceClass"
echo 0x00   > "$GADGET/bDeviceSubClass"
echo 0x00   > "$GADGET/bDeviceProtocol"

mkdir -p "$GADGET/strings/0x409"
echo "Sony Interactive Entertainment" > "$GADGET/strings/0x409/manufacturer"
echo "Wireless Controller"             > "$GADGET/strings/0x409/product"
echo "00000000"                        > "$GADGET/strings/0x409/serialnumber"

mkdir -p "$GADGET/functions/ffs.usb0"

mkdir -p "$GADGET/configs/c.1/strings/0x409"
echo "DualSense USB" > "$GADGET/configs/c.1/strings/0x409/configuration"
echo 500             > "$GADGET/configs/c.1/MaxPower"
echo 0xC0            > "$GADGET/configs/c.1/bmAttributes"

ln -s "$GADGET/functions/ffs.usb0" "$GADGET/configs/c.1/"

# Mount functionfs so the daemon can talk to /dev/ffs0/ep0.
mkdir -p "$FFS_MNT"
mount -t functionfs usb0 "$FFS_MNT"

echo "dualsense-ffs setup ready, daemon will bind UDC after descriptors load."
