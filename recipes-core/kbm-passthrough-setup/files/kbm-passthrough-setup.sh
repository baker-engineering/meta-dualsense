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

# Clone the device-level identity from the BBB-attached source mouse.
# Rationale: a composite USB gadget exposes ONE device-level VID/PID and
# one set of strings; the two HID interfaces (keyboard + mouse) sit under
# it. We pick the mouse for the device descriptor because the host sees
# the composite as "one mouse-like device that also has a keyboard
# interface" — exactly what plenty of Logitech / Razer composite
# receivers look like — and because the burst-on-hold use case is mouse-
# side. The host never sees the source keyboard's VID/PID at the device
# level even in passthrough; both source devices' inputs are forwarded
# through interfaces under one device descriptor regardless of policy.
#
# Falls back to the pid.codes Test PID range with generic strings if no
# source mouse is enumerated when this runs (e.g. a first boot before
# any host-side USB device has been plugged in). pid.codes is the
# canonical fictional-VID range for hobbyist USB devices.
#
# Note: bDeviceClass stays 0 (composite, use interface descriptors) even
# when the source mouse is HID-class — declaring this device HID-only at
# the top level would conflict with us having two HID interfaces under
# one composite config.

find_usb_hid_parent() {
    # $1 = HID protocol byte: 1 = keyboard, 2 = mouse.
    # Walks USB interface entries (paths containing ':') and returns the
    # parent device path of the first matching interface. Accepts any
    # subclass so gaming mice without a boot interface still match.
    local proto="$1" iface parent
    for iface in /sys/bus/usb/devices/*:*; do
        [ -e "$iface/bInterfaceClass"    ] || continue
        [ -e "$iface/bInterfaceProtocol" ] || continue
        [ "$(cat "$iface/bInterfaceClass")"    = "03"    ] || continue
        [ "$(cat "$iface/bInterfaceProtocol")" = "0$proto" ] || continue
        parent=$(readlink -f "$iface/..") || continue
        [ -e "$parent/idVendor" ] || continue
        echo "$parent"
        return 0
    done
    return 1
}

# Strip the trailing newline from a sysfs string file. Returns empty if
# the file is absent.
read_sysfs_str() {
    [ -f "$1" ] || return 0
    tr -d '\n' < "$1"
}

VID=0x1209
PID=0x0001
BCDDEV=0x0100
MFG="Generic"
PROD="USB Composite Device"
SERIAL="00000000"

src_mouse=$(find_usb_hid_parent 2 || true)
# STRINGS_SOURCE marks which identity branch the gadget ended up using. It
# is consumed by kbm-web's /api/status (gadget_descriptor.strings_source) so
# strict-API callers can verify identity-clone success without eyeballing
# the host's usbview.
STRINGS_SOURCE="fallback-generic"
if [ -n "${src_mouse:-}" ]; then
    vid_hex=$(read_sysfs_str "$src_mouse/idVendor")
    pid_hex=$(read_sysfs_str "$src_mouse/idProduct")
    bcd_hex=$(read_sysfs_str "$src_mouse/bcdDevice")
    [ -n "$vid_hex" ] && VID="0x$vid_hex"
    [ -n "$pid_hex" ] && PID="0x$pid_hex"
    [ -n "$bcd_hex" ] && BCDDEV="0x$bcd_hex"
    mfg=$(read_sysfs_str "$src_mouse/manufacturer")
    [ -n "$mfg" ]    && MFG="$mfg"
    prod=$(read_sysfs_str "$src_mouse/product")
    [ -n "$prod" ]   && PROD="$prod"
    serial=$(read_sysfs_str "$src_mouse/serial")
    [ -n "$serial" ] && SERIAL="$serial"
    STRINGS_SOURCE="cloned-from-mouse"
    echo "kbm-passthrough-setup: cloned identity from $src_mouse ($VID:$PID '$MFG' / '$PROD')"
else
    echo "kbm-passthrough-setup: no source mouse enumerated, using fictional identity ($VID:$PID)"
fi
echo "$STRINGS_SOURCE" > /run/kbm-passthrough-gadget-strings-source

mkdir -p "$GADGET"
echo "$VID"    > "$GADGET/idVendor"
echo "$PID"    > "$GADGET/idProduct"
echo "$BCDDEV" > "$GADGET/bcdDevice"
echo 0x0200    > "$GADGET/bcdUSB"
echo 0x00      > "$GADGET/bDeviceClass"
echo 0x00      > "$GADGET/bDeviceSubClass"
echo 0x00      > "$GADGET/bDeviceProtocol"

mkdir -p "$GADGET/strings/0x409"
echo "$MFG"    > "$GADGET/strings/0x409/manufacturer"
echo "$PROD"   > "$GADGET/strings/0x409/product"
echo "$SERIAL" > "$GADGET/strings/0x409/serialnumber"

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
