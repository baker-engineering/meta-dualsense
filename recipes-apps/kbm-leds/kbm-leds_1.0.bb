SUMMARY = "Animated status indicator on the BBB's 4 green user LEDs"
DESCRIPTION = "Tiny userspace daemon that drives /dev/gpiochip0 lines 21..24 \
(BBB USR0..USR3) in unison via Linux GPIO uAPI v2 ioctls. Renders smooth \
breathing/pulse patterns whose shape encodes system state: emulation mode, \
passthrough mode, host disconnected, error, or idle. Software-PWM at \
1 kHz × 16 levels for >60 Hz visual refresh."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://CMakeLists.txt \
    file://main.c \
    file://kbm-leds.service \
"

S = "${WORKDIR}"

inherit cmake systemd

SYSTEMD_SERVICE:${PN} = "kbm-leds.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

do_install:append() {
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/kbm-leds.service ${D}${systemd_system_unitdir}/kbm-leds.service
}

FILES:${PN} += " \
    ${systemd_system_unitdir}/kbm-leds.service \
"
