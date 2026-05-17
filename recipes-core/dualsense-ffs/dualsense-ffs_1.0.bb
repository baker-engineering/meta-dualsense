SUMMARY = "DualSense FunctionFS gadget setup (configfs + functionfs mount)"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://dualsense-ffs.sh \
    file://dualsense-ffs.service \
"

S = "${WORKDIR}"

inherit systemd allarch

RDEPENDS:${PN} = "bash"

SYSTEMD_SERVICE:${PN} = "dualsense-ffs.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${WORKDIR}/dualsense-ffs.sh ${D}${bindir}/dualsense-ffs

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/dualsense-ffs.service ${D}${systemd_system_unitdir}/dualsense-ffs.service
}

FILES:${PN} += " \
    ${bindir}/dualsense-ffs \
    ${systemd_system_unitdir}/dualsense-ffs.service \
"
