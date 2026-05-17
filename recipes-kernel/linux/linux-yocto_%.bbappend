FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI:append = " \
    file://dualsense-gadget.cfg \
    file://musb-ep0-setupend-recovery.patch \
"

# PREEMPT_RT is not enabled in this build. The yocto-kernel-cache for the
# beaglebone KBRANCH (v6.6/standard/beaglebone) does not ship a
# features/rt/preempt-rt.scc fragment, so the standard
# `KERNEL_FEATURES += "features/rt/preempt-rt.scc"` recipe fails. Userspace
# uses SCHED_FIFO + mlockall + CPU0 pin + epoll/timerfd to keep p99 under
# 5 ms on AM335x, which is sufficient for a 1 kHz HID gadget.
