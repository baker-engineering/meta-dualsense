#ifndef KBM_REPORT_H
#define KBM_REPORT_H
#include <stdint.h>

/*
 * DualSense USB input report layout (mode 0x01). 64 bytes including the
 * leading report-ID byte. Field offsets and bit assignments derived from
 * drivers/hid/hid-playstation.c in the kernel.
 *
 * We only populate the fields kbm-mapper actually drives (sticks,
 * triggers, buttons, sequence number). IMU, touchpad, battery, audio
 * remain zero — the host's hid-playstation driver tolerates that fine.
 */

#define DS_REPORT_LEN   64
#define DS_REPORT_ID    0x01

#define DS_OFF_LX       1
#define DS_OFF_LY       2
#define DS_OFF_RX       3
#define DS_OFF_RY       4
#define DS_OFF_L2       5
#define DS_OFF_R2       6
#define DS_OFF_SEQ      7
#define DS_OFF_BTN0     8    /* nibble 0-3: dpad hat; bits 4-7: square/cross/circle/triangle */
#define DS_OFF_BTN1     9    /* l1 r1 l2 r2 create options l3 r3 */
#define DS_OFF_BTN2    10    /* ps touchpad-click mute (bits 3-7 reserved) */
#define DS_OFF_TS      11    /* uint32 LE timestamp in 333us units */

#define DS_STICK_CENTER 128

#define DS_DPAD_N      0
#define DS_DPAD_NE     1
#define DS_DPAD_E      2
#define DS_DPAD_SE     3
#define DS_DPAD_S      4
#define DS_DPAD_SW     5
#define DS_DPAD_W      6
#define DS_DPAD_NW     7
#define DS_DPAD_NONE   8

#define DS_BTN0_SQUARE    (1u<<4)
#define DS_BTN0_CROSS     (1u<<5)
#define DS_BTN0_CIRCLE    (1u<<6)
#define DS_BTN0_TRIANGLE  (1u<<7)

#define DS_BTN1_L1        (1u<<0)
#define DS_BTN1_R1        (1u<<1)
#define DS_BTN1_L2        (1u<<2)
#define DS_BTN1_R2        (1u<<3)
#define DS_BTN1_CREATE    (1u<<4)
#define DS_BTN1_OPTIONS   (1u<<5)
#define DS_BTN1_L3        (1u<<6)
#define DS_BTN1_R3        (1u<<7)

#define DS_BTN2_PS        (1u<<0)
#define DS_BTN2_TOUCHPAD  (1u<<1)
#define DS_BTN2_MUTE      (1u<<2)

/* Logical action IDs used by config and mapper. Stable: do not renumber. */
enum ds_action {
    DS_ACT_NONE = 0,

    DS_ACT_LSTICK_UP, DS_ACT_LSTICK_DOWN, DS_ACT_LSTICK_LEFT, DS_ACT_LSTICK_RIGHT,
    DS_ACT_RSTICK_UP, DS_ACT_RSTICK_DOWN, DS_ACT_RSTICK_LEFT, DS_ACT_RSTICK_RIGHT,

    DS_ACT_DPAD_UP, DS_ACT_DPAD_DOWN, DS_ACT_DPAD_LEFT, DS_ACT_DPAD_RIGHT,

    DS_ACT_CROSS, DS_ACT_CIRCLE, DS_ACT_SQUARE, DS_ACT_TRIANGLE,
    DS_ACT_L1, DS_ACT_R1, DS_ACT_L2, DS_ACT_R2,
    DS_ACT_L3, DS_ACT_R3,
    DS_ACT_CREATE, DS_ACT_OPTIONS, DS_ACT_PS, DS_ACT_TOUCHPAD, DS_ACT_MUTE,

    DS_ACT_MAX
};

#endif
