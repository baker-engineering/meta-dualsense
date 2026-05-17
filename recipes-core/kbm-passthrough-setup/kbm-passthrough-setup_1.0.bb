SUMMARY = "USB gadget setup for passthrough mode (HID keyboard + mouse)"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://kbm-passthrough-setup.sh \
    file://kbm-passthrough-setup.service \
"

S = "${WORKDIR}"

inherit systemd allarch

RDEPENDS:${PN} = "bash"

# Not auto-enabled: the user picks emulation (default) or passthrough
# at runtime via the kbm-web mode toggle.
SYSTEMD_SERVICE:${PN} = "kbm-passthrough-setup.service"
SYSTEMD_AUTO_ENABLE:${PN} = "disable"

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${WORKDIR}/kbm-passthrough-setup.sh ${D}${bindir}/kbm-passthrough-setup

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/kbm-passthrough-setup.service ${D}${systemd_system_unitdir}/kbm-passthrough-setup.service
}

FILES:${PN} += " \
    ${bindir}/kbm-passthrough-setup \
    ${systemd_system_unitdir}/kbm-passthrough-setup.service \
"
