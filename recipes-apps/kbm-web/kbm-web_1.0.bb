SUMMARY = "kbm-web: BBB DualSense web configurator UI"
DESCRIPTION = "Single-process Go HTTP server. Reads/writes \
/etc/kbm-mapper.conf, controls dualsense-gadget and kbm-mapper services, \
and streams the live 64-byte HID report state for a browser-based \
controller-test widget."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://main.go \
    file://profiles.go \
    file://capture.go \
    file://go.mod \
    file://kbm-web.service \
    file://ui/index.html \
    file://ui/app.css \
    file://ui/app.js \
"

S = "${WORKDIR}"

DEPENDS = "go-native"

inherit systemd

SYSTEMD_SERVICE:${PN} = "kbm-web.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

do_compile() {
    cd ${S}
    export HOME="${WORKDIR}"
    export GOCACHE="${WORKDIR}/.gocache"
    export GOMODCACHE="${WORKDIR}/.gomodcache"
    export CGO_ENABLED=0
    export GOOS=linux
    export GOARCH=arm
    export GOARM=7
    export GOFLAGS="-mod=mod"
    # Yocto's do_package handles stripping itself; don't pre-strip with -s -w
    # or QA fails with [already-stripped].
    go build -trimpath -ldflags "-X main.buildVersion=${PV}" -o kbm-web .
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${S}/kbm-web ${D}${bindir}/kbm-web

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${S}/kbm-web.service ${D}${systemd_system_unitdir}/kbm-web.service
}

FILES:${PN} += " \
    ${bindir}/kbm-web \
    ${systemd_system_unitdir}/kbm-web.service \
"
