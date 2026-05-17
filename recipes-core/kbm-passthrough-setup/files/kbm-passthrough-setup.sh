#!/bin/bash
# Set up the configfs USB gadget for passthrough mode: a composite device
# that exposes a HID keyboard (hidg0) and a HID mouse (hidg1).
#
# Runs before kbm-passthrough, which reads the BBB's USB host-port keyboard
# and mouse via evdev and writes corresponding HID reports to the hidg
# devices, so the Windows host sees a real keyboard + mouse plugged in.
set -euo pipefail

GADGET=/sys/kernel/config/usb_gadget/kbm

mountpoint -q /sys/kernel/config || mount -t configfs none /sys/kernel/config

# Tear down any prior instance (ours or a leftover dualsense one).
for g in /sys/kernel/config/usb_gadget/*; do
    [[ -d "$g" ]] || continue
    [[ -f "$g/UDC" ]] && echo "" > "$g/UDC" 2>/dev/null || true
    find "$g/configs"/*/* -maxdepth 0 -type l -exec rm -f {} + 2>/dev/null || true
    rmdir "$g"/configs/*/strings/* 2>/dev/null || true
    rmdir "$g"/configs/* 2>/dev/null || true
    rmdir "$g"/functions/* 2>/dev/null || true
    rmdir "$g"/strings/* 2>/dev/null || true
    rmdir "$g" 2>/dev/null || true
done

# pid.codes Test PID range — explicitly fictional, not a real product.
mkdir -p "$GADGET"
echo 0x1209 > "$GADGET/idVendor"
echo 0x0001 > "$GADGET/idProduct"
echo 0x0100 > "$GADGET/bcdDevice"
echo 0x0200 > "$GADGET/bcdUSB"
echo 0x00   > "$GADGET/bDeviceClass"
echo 0x00   > "$GADGET/bDeviceSubClass"
echo 0x00   > "$GADGET/bDeviceProtocol"

mkdir -p "$GADGET/strings/0x409"
echo "Baker Engineering"           > "$GADGET/strings/0x409/manufacturer"
echo "DualSense Keyboard and Mouse" > "$GADGET/strings/0x409/product"
echo "00000000"                    > "$GADGET/strings/0x409/serialnumber"

# Boot keyboard: 8 byte report (1 modifiers, 1 reserved, 6 keys).
mkdir -p "$GADGET/functions/hid.usb0"
echo 1 > "$GADGET/functions/hid.usb0/protocol"   # 1 = keyboard
echo 1 > "$GADGET/functions/hid.usb0/subclass"   # 1 = boot interface
echo 8 > "$GADGET/functions/hid.usb0/report_length"
# Standard boot keyboard report descriptor (USB HID 1.11, App. B.1).
printf '\x05\x01\x09\x06\xa1\x01\x05\x07\x19\xe0\x29\xe7\x15\x00\x25\x01\x75\x01\x95\x08\x81\x02\x95\x01\x75\x08\x81\x03\x95\x05\x75\x01\x05\x08\x19\x01\x29\x05\x91\x02\x95\x01\x75\x03\x91\x03\x95\x06\x75\x08\x15\x00\x25\x65\x05\x07\x19\x00\x29\x65\x81\x00\xc0' \
    > "$GADGET/functions/hid.usb0/report_desc"

# Mouse: standard 4-byte boot-protocol report (3 buttons + signed int8 X/Y/wheel).
# This descriptor is the textbook one from USB HID 1.11 Appendix B.2 with the
# addition of a wheel axis sharing the same int8 size. Windows mouse class
# driver accepts this without quirks. A higher-resolution (int16) variant was
# tried first and triggered CM_PROB_FAILED_START on the mouse interface.
mkdir -p "$GADGET/functions/hid.usb1"
echo 2 > "$GADGET/functions/hid.usb1/protocol"   # 2 = mouse
echo 1 > "$GADGET/functions/hid.usb1/subclass"   # 1 = boot interface
echo 4 > "$GADGET/functions/hid.usb1/report_length"
printf '\x05\x01\x09\x02\xa1\x01\x09\x01\xa1\x00\x05\x09\x19\x01\x29\x03\x15\x00\x25\x01\x95\x03\x75\x01\x81\x02\x95\x01\x75\x05\x81\x03\x05\x01\x09\x30\x09\x31\x09\x38\x15\x81\x25\x7f\x75\x08\x95\x03\x81\x06\xc0\xc0' \
    > "$GADGET/functions/hid.usb1/report_desc"

mkdir -p "$GADGET/configs/c.1/strings/0x409"
echo "Keyboard and Mouse" > "$GADGET/configs/c.1/strings/0x409/configuration"
echo 250              > "$GADGET/configs/c.1/MaxPower"
echo 0xC0             > "$GADGET/configs/c.1/bmAttributes"

ln -s "$GADGET/functions/hid.usb0" "$GADGET/configs/c.1/"
ln -s "$GADGET/functions/hid.usb1" "$GADGET/configs/c.1/"

# Bind UDC. Unlike the FunctionFS path, configfs HID is ready immediately
# after the function links are in place.
UDC=$(ls /sys/class/udc | head -n1)
if [[ -z "$UDC" ]]; then
    echo "no UDC available" >&2
    exit 1
fi
echo "$UDC" > "$GADGET/UDC"

echo "kbm-passthrough gadget bound to $UDC; hidg0=keyboard hidg1=mouse"
