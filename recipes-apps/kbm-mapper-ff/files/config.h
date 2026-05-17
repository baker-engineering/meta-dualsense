#ifndef KBM_CONFIG_H
#define KBM_CONFIG_H
#include <stdint.h>
#include "report.h"

/*
 * Parsed /etc/kbm-mapper.conf.
 *
 * Mapping arrays are indexed by Linux input KEY_* / BTN_* codes; the value
 * is the DualSense action that key triggers (DS_ACT_NONE = unmapped).
 *
 * KEY_MAX in <linux/input-event-codes.h> is 0x2FF; we size the table to
 * 0x300 (768) so any keyboard code fits.
 */
#define KBM_KEYTABLE_SIZE 0x300

struct config {
    /* mouse -> right stick tuning */
    double  window_ms;        /* sliding velocity window length */
    double  curve_exp;        /* response curve exponent */
    double  anti_deadzone;    /* normalized [0, ~0.3] */
    double  outer_sat;        /* clamp magnitude (default 0.97) */
    double  sens_counts_ms;   /* mouse counts/ms that produces full deflection */
    double  debt_drain;       /* counts/tick drained from rotation-debt buffer */

    uint8_t key_action[KBM_KEYTABLE_SIZE];   /* keyboard codes */
    uint8_t btn_action[KBM_KEYTABLE_SIZE];   /* mouse button codes (BTN_LEFT..) */

    /* Burst-on-hold: while an action's source input is held, oscillate the
     * reported action at burst_hz[a] Hz with burst_duty[a] high-time fraction.
     * burst_hz <= 0 disables burst for that action (the default — hold acts
     * like a normal sustained press). Indexed by DS action id. */
    double burst_hz[DS_ACT_MAX];
    double burst_duty[DS_ACT_MAX];

    /* Mode-toggle hotkey: list of Linux KEY_* codes that must all be held
     * simultaneously for >=1s to trigger a switch between emulation and
     * passthrough. Defaults to LEFTCTRL + ESC. Max 8 keys; hotkey_n == 0
     * disables the feature. */
    int hotkey_codes[8];
    int hotkey_n;
};

void config_init_defaults(struct config *c);
int  config_load(struct config *c, const char *path);

/* Resolve a name like "cross"/"r2"/"lstick_up" to an action id, or 0. */
enum ds_action config_action_from_name(const char *name);

/* Resolve "W"/"SPACE"/"LEFTSHIFT" etc. to a Linux KEY_* code, or -1. */
int config_key_from_name(const char *name);

/* Resolve "LEFT"/"RIGHT"/"MIDDLE"/"SIDE"/"EXTRA" to a BTN_* code, or -1. */
int config_btn_from_name(const char *name);

#endif
