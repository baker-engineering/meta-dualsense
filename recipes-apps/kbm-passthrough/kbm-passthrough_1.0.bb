SUMMARY = "USB-host keyboard+mouse passthrough to a configfs HID gadget"
DESCRIPTION = "Daemon that reads /dev/input/event* keyboard/mouse, hot-plug \
aware via inotify, and forwards them as standard HID reports to /dev/hidg0 \
(boot keyboard) and /dev/hidg1 (mouse). Mutually exclusive with \
dualsense-ffsd via systemd Conflicts=; pair it with kbm-passthrough-setup \
which configures the configfs gadget."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://CMakeLists.txt \
    file://main.c \
    file://kbm-passthrough.service \
"

S = "${WORKDIR}"

inherit cmake systemd

RDEPENDS:${PN} = "kbm-passthrough-setup"

SYSTEMD_SERVICE:${PN} = "kbm-passthrough.service"
SYSTEMD_AUTO_ENABLE:${PN} = "disable"

do_install:append() {
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/kbm-passthrough.service ${D}${systemd_system_unitdir}/kbm-passthrough.service
}

FILES:${PN} += " \
    ${systemd_system_unitdir}/kbm-passthrough.service \
"
