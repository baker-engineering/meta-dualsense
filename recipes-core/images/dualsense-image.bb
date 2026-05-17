SUMMARY = "Headless BeagleBone Black DualSense gadget image (FunctionFS)"
LICENSE = "MIT"

inherit core-image

IMAGE_FEATURES += "ssh-server-dropbear tools-debug debug-tweaks"

IMAGE_INSTALL = " \
    packagegroup-core-boot \
    kernel-modules \
    kernel-devicetree \
    \
    dualsense-ffs \
    kbm-mapper-ff \
    kbm-passthrough-setup \
    kbm-passthrough \
    kbm-web \
    kbm-leds \
    \
    bash \
    util-linux \
    evtest \
    \
    ${CORE_IMAGE_EXTRA_INSTALL} \
"

ROOTFS_POSTPROCESS_COMMAND += "set_dualsense_motd; "

set_dualsense_motd() {
    cat > ${IMAGE_ROOTFS}/etc/motd <<'MOTDEOF'

  BeagleBone Black -- DualSense USB HID gadget (FunctionFS)
  ---------------------------------------------------------
  Web UI:   http://<this-board-ip>/
  Mode:     emulation (default), kbm-web toggle switches to passthrough
  Status:   systemctl status dualsense-ffsd kbm-passthrough kbm-web
  Logs:     journalctl -u dualsense-ffsd -u kbm-passthrough -f
  Config:   /etc/kbm-mapper.conf

MOTDEOF
}
