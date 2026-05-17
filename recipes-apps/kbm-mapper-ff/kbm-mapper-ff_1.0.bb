SUMMARY = "DualSense FunctionFS userspace daemon (descriptors + IN/OUT/control)"
DESCRIPTION = "Replaces the configfs HID kbm-mapper combo. One process owns \
/dev/ffs0/ep0..2: configures USB descriptors, binds UDC, handles HID class \
control transfers (feature reports, report-descriptor), writes 64 byte \
DualSense input reports to ep1 at 1 kHz driven by evdev keyboard and mouse, \
and consumes host-to-device output reports on ep2."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://CMakeLists.txt \
    file://main.c \
    file://mapper.c \
    file://mapper.h \
    file://config.c \
    file://config.h \
    file://report.h \
    file://ffs_descriptors.h \
    file://feature_reports.c \
    file://feature_reports.h \
    file://dualsense-ffsd.service \
    file://kbm-mapper.conf \
    file://99-kbm-input.rules \
"

S = "${WORKDIR}"

inherit cmake systemd

SYSTEMD_SERVICE:${PN} = "dualsense-ffsd.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

do_install:append() {
    install -d ${D}${sysconfdir}
    install -m 0644 ${WORKDIR}/kbm-mapper.conf ${D}${sysconfdir}/kbm-mapper.conf

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/dualsense-ffsd.service ${D}${systemd_system_unitdir}/dualsense-ffsd.service

    install -d ${D}${nonarch_base_libdir}/udev/rules.d
    install -m 0644 ${WORKDIR}/99-kbm-input.rules ${D}${nonarch_base_libdir}/udev/rules.d/99-kbm-input.rules
}

FILES:${PN} += " \
    ${sysconfdir}/kbm-mapper.conf \
    ${systemd_system_unitdir}/dualsense-ffsd.service \
    ${nonarch_base_libdir}/udev/rules.d/99-kbm-input.rules \
"
