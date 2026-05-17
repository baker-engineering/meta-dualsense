# meta-dualsense

A Yocto layer that builds a headless BeagleBone Black image which
presents to a host PC as a Sony DualSense controller, driven by a USB
keyboard and mouse plugged into the BBB. A small web UI on the BBB
lets you tune the mouse to right-stick response curve, remap keys
and mouse buttons, save named profiles, switch between modes, and
watch a live controller test widget without rebooting.

The Windows-side half of the pipeline (translates the DualSense HID
into a virtual Xbox 360 controller via ViGEmBus so games like Call of
Duty Warzone see a familiar pad) lives in a separate repository:

    https://github.com/baker-engineering/dualsense-xbridge


## Hardware

The build targets one specific board. With the components listed below
the device boots ready-to-use in about 12 seconds from cold power-on.

| Item                  | Notes                                                  |
| --------------------- | ------------------------------------------------------ |
| BeagleBone Black      | rev. A5C or later. eMMC required (no SD-only).         |
| USB mini-B cable      | BBB peripheral (mini-USB) port to the host PC.         |
| USB hub               | Powered or bus-powered, plugged into the BBB's USB-A.  |
| USB keyboard          | Standard HID boot keyboard.                            |
| USB mouse             | Standard HID boot mouse.                               |
| FTDI 3.3 V serial     | Optional. J1 header on BBB. Recommended for boot logs. |

Wiring:

    +-----------+
    |  Host PC  | <----- USB mini-B --------+
    +-----------+                           |
                                            v
                                  +-------------------+
                                  | BeagleBone Black  |
                                  | (eMMC, 1 GHz A8)  |
                                  +-------------------+
                                            ^
                                            | USB-A
                                            v
                                       +--------+
                                       |  Hub   |
                                       +--------+
                                        |     |
                                  keyboard   mouse


## Build

On any machine with Docker:

    ./build.sh build

`build.sh` is a thin wrapper around `kas-container` pinned to kas 4.6.
On first run it pulls `ghcr.io/siemens/kas/kas:4.6`, fetches poky
scarthgap plus meta-openembedded, meta-arm, meta-ti, and runs the
full bitbake. Output lands in:

    build/tmp/deploy/images/beaglebone-yocto/

The wic is named
`dualsense-image-beaglebone-yocto.rootfs.wic` (about 252 MB).

Useful sub-commands:

    ./build.sh shell                              # interactive kas shell
    ./build.sh shell -c "bitbake -c menuconfig virtual/kernel"


## Flash

Two paths. Use BBBlfs unless you have a reason to do an in-place dd.


### BBBlfs flash (recommended)

The AM335x ROM enters a USB peripheral boot mode when the S2 button on
the board is held during power-on. From that mode the host PC can
serve SPL, u-boot, and a tiny FIT ramdisk over RNDIS + TFTP, and the
ramdisk exposes the BBB's eMMC as a USB mass-storage device that the
host can dd onto.

The BBBlfs project provides the SPL, u-boot, and FIT blobs:

    https://github.com/ungureanuvladvictor/BBBlfs

Build them in a debian:bookworm container:

    docker run --rm -v /tmp/bbblfs:/work debian:bookworm bash -c '
      apt-get update && apt-get install -y git build-essential \
        libusb-1.0-0-dev pkg-config automake autoconf libtool
      cd /work
      git clone https://github.com/ungureanuvladvictor/BBBlfs.git
      cd BBBlfs && ./autogen.sh && ./configure && make
    '

Extract the resulting `bin/spl`, `bin/uboot`, `bin/fit` into a tftp
root. The full helper scripts (dnsmasq + ip-keeper container) are
documented in the repository's runbook.

Once the chain runs end to end the BBB enumerates as
`0525:a4a5 Linux-USB File-backed Storage Gadget` and shows up as
`/dev/sdc` (3.6 GB) on the host. Then:

    dd if=dualsense-image-beaglebone-yocto.rootfs.wic of=/dev/sdc \
       bs=4M conv=fsync status=progress
    sync

Unplug, then power-cycle WITHOUT holding S2 to boot from eMMC.


### In-place dd (when the BBB is already running)

Faster but requires the rootfs to be quiesced first or page-cache
writeback will silently corrupt the freshly written image.

    ssh root@bbb '
      systemctl stop dualsense-ffsd dualsense-ffs kbm-passthrough \
                     kbm-passthrough-setup kbm-leds kbm-web \
                     systemd-journald.service
      mount -o remount,ro /
      sync; sync; sync
      echo 3 > /proc/sys/vm/drop_caches
      wget -q -O /dev/mmcblk1 http://your-server/path/to/wic
      dd if=/dev/mmcblk1 bs=1024 count=<size_KiB> | sha256sum
      sync; sync
      echo b > /proc/sysrq-trigger
    '

Verify the size+hash against the source wic before issuing the
sysrq-b reboot. Without the remount-ro step the page cache trumps the
on-disk content and the next boot wedges with no serial output.


## First boot

After flash the BBB defaults to emulation mode. Plug the mini-USB
cable into a host PC and the gadget enumerates as:

    USB\VID_054C&PID_0CE6\<serial>      Sony Corp. DualSense

On Windows confirm with Device Manager or `pnputil /enum-devices`.
On Linux `lsusb` will list the device.


## Web UI

The BBB serves a single-page web app on port 80:

    http://<bbb-ip>/

Find the address either by attaching the BBB to a router with DHCP
and looking at the DHCP table, or by reading the serial console
(`systemctl status systemd-networkd` shows the assigned address).

What the UI offers:

  * Live status panel: UDC state, gadget binding, mode, input
    device list, service health.
  * Mode toggle: emulation (DualSense to host) vs passthrough
    (HID keyboard + mouse to host) vs off.
  * Mouse to right-stick tuning sliders -- window_ms, curve_exp,
    anti_deadzone, outer_sat, sens_counts_ms, debt_drain. Edits
    persist through inotify hot-reload, no service restart.
  * Burst-on-hold per action: oscillate any DualSense action while
    the source key is held, at a configurable Hz and duty cycle.
    Useful for recoil control on r2.
  * Key + mouse binding editor with click-to-capture so you don't
    have to type symbolic KEY_* names.
  * Profile system: named snapshots of the full tuning + bindings +
    burst + hotkey set. Activate, rename, duplicate, export to
    .conf, import from .conf.
  * Configurable mode-toggle hotkey: capture a chord of any keys,
    hold them for 1 second to flip the BBB between emulation and
    passthrough.
  * Live controller test widget with both sticks, triggers, the
    D-pad, and all face buttons updating from a Server-Sent Events
    stream of /run/kbm-mapper/state.bin at 20 Hz.


## Tuning parameters

The same parameters are persisted to `/etc/kbm-mapper.conf` and can
be edited directly. The daemon hot-reloads on file change via inotify.

| Key              | Default | Effect                                             |
| ---------------- | ------- | -------------------------------------------------- |
| `window_ms`      | 6       | Sliding velocity window for mouse to right stick.  |
| `curve_exp`      | 2.0     | Response curve. 1.0 = linear, 2.0 = quadratic.     |
| `anti_deadzone`  | 0.10    | Minimum stick magnitude for any non-zero input.    |
| `outer_sat`      | 0.97    | Clamp the maximum stick magnitude.                 |
| `sens_counts_ms` | 8.0     | Mouse counts/ms required for full deflection.      |
| `debt_drain`     | 0.05    | Per-tick decay of the rotation-debt buffer.        |


## Boot performance

The image is heavily tuned for fast cold-boot. From ROM reset to a
USB device that Windows enumerates as a DualSense controller takes
about 12 seconds.

Optimizations applied:

  * Service ordering: gadget, web UI, and LED daemon all anchor on
    `systemd-remount-fs.service` with `WantedBy=sysinit.target`
    rather than waiting for the basic.target chain.
  * /boot mounted nofail,noauto so local-fs.target does not block
    on the FAT partition's by-uuid resolution (a ~18 s wait on this
    image).
  * Audio module chain blacklisted so udev does not auto-load 13
    snd_soc_* modules at boot.
  * systemd-timesyncd and systemd-resolved masked. The BBB has no
    RTC and the web UI is reached by IP, so neither service has a
    use.
  * tools-debug removed from IMAGE_FEATURES (~30 MB rootfs).
  * Kernel image compression switched from gzip to LZ4 (~1.5 s off
    decompression on the Cortex-A8).
  * Kernel subsystem disables: DRM, SOUND, WIRELESS, WLAN, RFKILL,
    BT, MEDIA, CAN. No silicon support for any of these in this
    profile.
  * Kernel built-in cmdline appended via CMDLINE_EXTEND with
    `quiet loglevel=3 cpufreq.default_governor=performance` (the
    governor pin is the bigger contributor).
  * U-Boot CONFIG_BOOTDELAY=0 (~2 s off the SPL to kernel hand-off).

Measured service active time (from kernel monotonic clock 0):

| Service          | Before  | After     |
| ---------------- | ------- | --------- |
| dualsense-ffs    | +30.4 s | **+7.7 s** |
| dualsense-ffsd   | +30.6 s | **+7.8 s** |
| kbm-web          | +31.1 s | **+7.9 s** |
| kbm-leds         | +31.8 s | **+11.1 s** |

Cold-boot wall clock from ROM reset to Windows-visible USB device:
**about 12 s**, down from about 30 s before optimization.


## Repository layout

    meta-dualsense/
    +- conf/layer.conf
    +- kas.yml                          poky scarthgap + meta-oe + meta-arm
    +                                   + meta-ti + this layer
    +- build.sh                         kas-container wrapper
    +- recipes-bsp/u-boot/              bootdelay=0 bbappend
    +- recipes-kernel/linux/            USB gadget cfg, MUSB patch, boot opts
    +- recipes-core/
    |   +- dualsense-ffs/               configfs DualSense gadget builder
    |   +- kbm-passthrough-setup/       configfs HID kbd+mouse gadget builder
    |   +- images/dualsense-image.bb    image recipe
    +- recipes-apps/
        +- kbm-mapper-ff/               DualSense userspace daemon (C)
        +- kbm-passthrough/             USB-host kbd/mouse forwarder (C)
        +- kbm-leds/                    Animated USR-LED indicator (C)
        +- kbm-web/                     Web configurator (Go + go:embed UI)


## License

MIT. See `LICENSE`.
