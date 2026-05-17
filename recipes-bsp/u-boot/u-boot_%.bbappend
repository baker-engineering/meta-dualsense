# am335x_evm_defconfig does not set CONFIG_BOOTDELAY explicitly, so it
# falls through to the Kconfig default of 2 seconds. On the BBB that
# 2 s is the "press any key to interrupt autoboot" countdown -- an
# operator-typing-at-the-serial-console affordance that has no purpose
# for this gadget profile. Patch the .config produced by
# "make ${UBOOT_MACHINE}" to force CONFIG_BOOTDELAY=0 using scripts/config
# so the value sticks across u-boot's internal olddefconfig pass.

do_configure:append() {
    if [ -x ${S}/scripts/config ] && [ -f ${B}/.config ]; then
        ${S}/scripts/config --file ${B}/.config --set-val CONFIG_BOOTDELAY 0
        oe_runmake -C ${S} O=${B} olddefconfig
    fi
}
