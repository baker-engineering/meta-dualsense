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

ROOTFS_POSTPROCESS_COMMAND += "set_dualsense_motd; install_module_blacklist; patch_fstab_boot_nofail; "

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

# Wic image build auto-adds /boot to /etc/fstab as
# "UUID=<...> /boot vfat defaults 0 0". With defaults, systemd
# auto-generates a boot.mount unit pulled into local-fs.target. On this
# BBB the /dev/disk/by-uuid/<...> symlink for the FAT boot partition
# takes ~18 s to appear, which blocks local-fs.target for that entire
# window. /boot is read by u-boot at the SPL/u-boot stage (loads zImage
# + dtb) BEFORE Linux is even running, so we don't need it mounted at
# runtime. nofail,noauto says "don't block boot if it's not there, and
# don't try to mount it automatically".
patch_fstab_boot_nofail() {
    sed -i 's|\(/boot[[:space:]]\+vfat[[:space:]]\+\)defaults|\1nofail,noauto|' \
        ${IMAGE_ROOTFS}/etc/fstab
}

# The BBB's HDMI audio path is unused in this profile. The device tree's
# McASP + HDMI-codec nodes match a chain of 13 snd_soc_* modules via
# modalias and udev auto-loads all of them at boot. Blacklist the chain
# so none of them load.
install_module_blacklist() {
    install -d ${IMAGE_ROOTFS}/etc/modprobe.d
    cat > ${IMAGE_ROOTFS}/etc/modprobe.d/blacklist-unused.conf <<'BLEOF'
# Audio chain -- BBB HDMI audio / McASP, unused in the DualSense gadget profile.
blacklist snd_soc_simple_card
blacklist snd_soc_simple_card_utils
blacklist snd_soc_davinci_mcasp
blacklist snd_soc_ti_udma
blacklist snd_soc_ti_edma
blacklist snd_soc_hdmi_codec
blacklist snd_soc_ti_sdma
blacklist snd_soc_core
blacklist snd_pcm_dmaengine
blacklist snd_pcm
blacklist snd_timer
blacklist snd
blacklist soundcore
BLEOF
}
