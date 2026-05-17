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

ROOTFS_POSTPROCESS_COMMAND += "set_dualsense_motd; install_module_blacklist; patch_fstab_boot_nofail; mask_unused_systemd_services; install_static_resolv_conf; "

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

patch_fstab_boot_nofail() {
    sed -i 's|\(/boot[[:space:]]\+vfat[[:space:]]\+\)defaults|\1nofail,noauto|' \
        ${IMAGE_ROOTFS}/etc/fstab
}

# Mask systemd services that this profile does not use:
#   systemd-timesyncd -- the BBB has no battery-backed RTC, wall clock is
#     wrong on every boot anyway, and the daemons use CLOCK_MONOTONIC for
#     all timing.
#   systemd-resolved  -- the web UI is reached by IP, the daemons make no
#     outbound HTTP/HTTPS calls, and /etc/hosts handles localhost via NSS.
mask_unused_systemd_services() {
    ln -sf /dev/null ${IMAGE_ROOTFS}${systemd_unitdir}/system/systemd-timesyncd.service
    ln -sf /dev/null ${IMAGE_ROOTFS}${systemd_unitdir}/system/systemd-resolved.service
}

# When systemd-resolved is masked, /etc/resolv.conf (a symlink chain into
# /run/systemd/resolve) goes stale. Ship an empty resolv.conf so any libc
# resolver call falls back to "no nameservers configured" instead of
# tripping on a missing file.
install_static_resolv_conf() {
    rm -f ${IMAGE_ROOTFS}/etc/resolv.conf ${IMAGE_ROOTFS}/etc/resolv-conf.systemd
    cat > ${IMAGE_ROOTFS}/etc/resolv.conf <<'RESOLVEOF'
# Empty resolv.conf. DNS is not used in this image. systemd-resolved
# is masked. /etc/hosts handles localhost lookups via NSS.
RESOLVEOF
}

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
