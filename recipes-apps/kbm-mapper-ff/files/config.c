#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <linux/input-event-codes.h>
#include "config.h"

struct named { const char *name; int code; };

/* Subset of Linux input KEY_* codes useful for keyboard mapping. */
static const struct named key_names[] = {
    {"A",KEY_A},{"B",KEY_B},{"C",KEY_C},{"D",KEY_D},{"E",KEY_E},{"F",KEY_F},
    {"G",KEY_G},{"H",KEY_H},{"I",KEY_I},{"J",KEY_J},{"K",KEY_K},{"L",KEY_L},
    {"M",KEY_M},{"N",KEY_N},{"O",KEY_O},{"P",KEY_P},{"Q",KEY_Q},{"R",KEY_R},
    {"S",KEY_S},{"T",KEY_T},{"U",KEY_U},{"V",KEY_V},{"W",KEY_W},{"X",KEY_X},
    {"Y",KEY_Y},{"Z",KEY_Z},
    {"0",KEY_0},{"1",KEY_1},{"2",KEY_2},{"3",KEY_3},{"4",KEY_4},
    {"5",KEY_5},{"6",KEY_6},{"7",KEY_7},{"8",KEY_8},{"9",KEY_9},
    {"SPACE",KEY_SPACE},{"ENTER",KEY_ENTER},{"ESC",KEY_ESC},{"TAB",KEY_TAB},
    {"LEFTSHIFT",KEY_LEFTSHIFT},{"RIGHTSHIFT",KEY_RIGHTSHIFT},
    {"LEFTCTRL",KEY_LEFTCTRL},{"RIGHTCTRL",KEY_RIGHTCTRL},
    {"LEFTALT",KEY_LEFTALT},{"RIGHTALT",KEY_RIGHTALT},
    {"BACKSPACE",KEY_BACKSPACE},{"GRAVE",KEY_GRAVE},
    {"UP",KEY_UP},{"DOWN",KEY_DOWN},{"LEFT",KEY_LEFT},{"RIGHT",KEY_RIGHT},
    {"F1",KEY_F1},{"F2",KEY_F2},{"F3",KEY_F3},{"F4",KEY_F4},
    {"F5",KEY_F5},{"F6",KEY_F6},{"F7",KEY_F7},{"F8",KEY_F8},
    {"F9",KEY_F9},{"F10",KEY_F10},{"F11",KEY_F11},{"F12",KEY_F12},
    {NULL,0}
};

static const struct named btn_names[] = {
    {"LEFT",BTN_LEFT},{"RIGHT",BTN_RIGHT},{"MIDDLE",BTN_MIDDLE},
    {"SIDE",BTN_SIDE},{"EXTRA",BTN_EXTRA},{NULL,0}
};

static const struct named action_names[] = {
    {"lstick_up",DS_ACT_LSTICK_UP},{"lstick_down",DS_ACT_LSTICK_DOWN},
    {"lstick_left",DS_ACT_LSTICK_LEFT},{"lstick_right",DS_ACT_LSTICK_RIGHT},
    {"rstick_up",DS_ACT_RSTICK_UP},{"rstick_down",DS_ACT_RSTICK_DOWN},
    {"rstick_left",DS_ACT_RSTICK_LEFT},{"rstick_right",DS_ACT_RSTICK_RIGHT},
    {"dpad_up",DS_ACT_DPAD_UP},{"dpad_down",DS_ACT_DPAD_DOWN},
    {"dpad_left",DS_ACT_DPAD_LEFT},{"dpad_right",DS_ACT_DPAD_RIGHT},
    {"cross",DS_ACT_CROSS},{"circle",DS_ACT_CIRCLE},
    {"square",DS_ACT_SQUARE},{"triangle",DS_ACT_TRIANGLE},
    {"l1",DS_ACT_L1},{"r1",DS_ACT_R1},{"l2",DS_ACT_L2},{"r2",DS_ACT_R2},
    {"l3",DS_ACT_L3},{"r3",DS_ACT_R3},
    {"create",DS_ACT_CREATE},{"options",DS_ACT_OPTIONS},
    {"ps",DS_ACT_PS},{"touchpad",DS_ACT_TOUCHPAD},{"mute",DS_ACT_MUTE},
    {NULL,0}
};

static int lookup_ci(const struct named *t, const char *s) {
    for (; t->name; t++)
        if (strcasecmp(t->name, s) == 0) return t->code;
    return -1;
}

void config_init_defaults(struct config *c) {
    memset(c, 0, sizeof(*c));
    c->window_ms      = 6.0;
    c->curve_exp      = 2.0;
    c->anti_deadzone  = 0.10;
    c->outer_sat      = 0.97;
    c->sens_counts_ms = 8.0;  /* 8 counts/ms ≈ typical desktop sens at 800 dpi */
    c->debt_drain     = 0.05; /* normalized: 5% of full deflection paid per tick */

    /* Default mapping per the project brief. */
    c->key_action[KEY_W] = DS_ACT_LSTICK_UP;
    c->key_action[KEY_S] = DS_ACT_LSTICK_DOWN;
    c->key_action[KEY_A] = DS_ACT_LSTICK_LEFT;
    c->key_action[KEY_D] = DS_ACT_LSTICK_RIGHT;
    c->key_action[KEY_SPACE]     = DS_ACT_CROSS;
    c->key_action[KEY_LEFTSHIFT] = DS_ACT_CIRCLE;
    c->btn_action[BTN_LEFT  & 0xFF] = DS_ACT_R2;
    c->btn_action[BTN_RIGHT & 0xFF] = DS_ACT_L2;

    /* Default mode-toggle hotkey: LEFTCTRL + ESC. */
    c->hotkey_codes[0] = KEY_LEFTCTRL;
    c->hotkey_codes[1] = KEY_ESC;
    c->hotkey_n        = 2;

    /* Recoil + sensitivity scaling: default to off (action = NONE means
     * the feature is inert regardless of x/y or scale values). The
     * recoil.action and ads.action keys in the conf file enable them. */
    c->recoil_action   = DS_ACT_NONE;
    c->recoil_x        = 0.0;
    c->recoil_y        = 0.0;
    c->ads_action      = DS_ACT_NONE;
    c->ads_sens_scale  = 1.0;
    c->fire_sens_scale = 1.0;
}

enum ds_action config_action_from_name(const char *n) {
    int v = lookup_ci(action_names, n);
    return v < 0 ? DS_ACT_NONE : (enum ds_action)v;
}
int config_key_from_name(const char *n) { return lookup_ci(key_names, n); }
int config_btn_from_name(const char *n) { return lookup_ci(btn_names, n); }

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

int config_load(struct config *c, const char *path) {
    config_init_defaults(c);
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "kbm-mapper: %s not found, using defaults\n", path);
        return 0;
    }
    char line[512];
    int lineno = 0;
    while (fgets(line, sizeof line, fp)) {
        lineno++;
        char *p = trim(line);
        if (*p == '\0' || *p == '#') continue;
        char *eq = strchr(p, '=');
        if (!eq) { fprintf(stderr, "kbm-mapper: %s:%d: missing '='\n", path, lineno); continue; }
        *eq = 0;
        char *k = trim(p), *v = trim(eq + 1);

        if      (strcmp(k,"window_ms")==0)      c->window_ms      = atof(v);
        else if (strcmp(k,"curve_exp")==0)      c->curve_exp      = atof(v);
        else if (strcmp(k,"anti_deadzone")==0)  c->anti_deadzone  = atof(v);
        else if (strcmp(k,"outer_sat")==0)      c->outer_sat      = atof(v);
        else if (strcmp(k,"sens_counts_ms")==0) c->sens_counts_ms = atof(v);
        else if (strcmp(k,"debt_drain")==0)     c->debt_drain     = atof(v);
        else if (strncmp(k,"key.",4)==0) {
            int code = config_key_from_name(k + 4);
            int act  = config_action_from_name(v);
            if (code < 0 || act == 0)
                fprintf(stderr, "kbm-mapper: %s:%d: bad mapping '%s=%s'\n", path, lineno, k, v);
            else
                c->key_action[code] = (uint8_t)act;
        }
        else if (strncmp(k,"mouse.",6)==0) {
            int code = config_btn_from_name(k + 6);
            int act  = config_action_from_name(v);
            if (code < 0 || act == 0)
                fprintf(stderr, "kbm-mapper: %s:%d: bad mapping '%s=%s'\n", path, lineno, k, v);
            else
                c->btn_action[code & 0xFF] = (uint8_t)act;
        }
        else if (strcmp(k,"hotkey.mode_toggle")==0) {
            /* Comma- or plus-separated list of KEY_* names, e.g.
             * "LEFTCTRL+ESC" or "LEFTSHIFT,LEFTALT,F12". Empty value disables. */
            c->hotkey_n = 0;
            char *cur = v;
            while (*cur && c->hotkey_n < (int)(sizeof c->hotkey_codes / sizeof c->hotkey_codes[0])) {
                char *sep = cur;
                while (*sep && *sep != '+' && *sep != ',') sep++;
                int had_sep = (*sep != 0);
                if (had_sep) *sep = 0;
                char *name = trim(cur);
                if (*name) {
                    int code = config_key_from_name(name);
                    if (code >= 0) c->hotkey_codes[c->hotkey_n++] = code;
                    else fprintf(stderr, "kbm-mapper: %s:%d: unknown hotkey key '%s'\n", path, lineno, name);
                }
                if (!had_sep) break;
                cur = sep + 1;
            }
        }
        else if (strncmp(k,"burst.",6)==0) {
            /* "burst.<action>.{hz,duty,jitter}" */
            char *dot = strrchr(k + 6, '.');
            if (!dot) {
                fprintf(stderr, "kbm-mapper: %s:%d: bad burst key '%s'\n", path, lineno, k);
            } else {
                *dot = 0;
                int act = config_action_from_name(k + 6);
                const char *field = dot + 1;
                if (act <= 0 || act >= DS_ACT_MAX) {
                    fprintf(stderr, "kbm-mapper: %s:%d: unknown action in '%s'\n", path, lineno, k);
                } else if (strcmp(field,"hz")==0) {
                    c->burst_hz[act] = atof(v);
                } else if (strcmp(field,"duty")==0) {
                    c->burst_duty[act] = atof(v);
                } else if (strcmp(field,"jitter")==0) {
                    double j = atof(v);
                    if (j < 0) j = 0;
                    if (j > 1) j = 1;
                    c->burst_jitter[act] = j;
                } else if (strcmp(field,"duty_jitter")==0) {
                    double j = atof(v);
                    if (j < 0) j = 0;
                    if (j > 1) j = 1;
                    c->burst_duty_jitter[act] = j;
                } else if (strcmp(field,"skip_prob")==0) {
                    double s = atof(v);
                    if (s < 0) s = 0;
                    if (s > 1) s = 1;
                    c->burst_skip_prob[act] = s;
                } else {
                    fprintf(stderr, "kbm-mapper: %s:%d: unknown burst field '%s'\n", path, lineno, field);
                }
            }
        }
        else if (strcmp(k,"recoil.action")==0) {
            int act = config_action_from_name(v);
            if (act == DS_ACT_NONE)
                fprintf(stderr, "kbm-mapper: %s:%d: unknown action '%s'\n", path, lineno, v);
            else
                c->recoil_action = act;
        }
        else if (strcmp(k,"recoil.x")==0) {
            double r = atof(v);
            if (r < -1) r = -1;
            if (r >  1) r =  1;
            c->recoil_x = r;
        }
        else if (strcmp(k,"recoil.y")==0) {
            double r = atof(v);
            if (r < -1) r = -1;
            if (r >  1) r =  1;
            c->recoil_y = r;
        }
        else if (strcmp(k,"ads.action")==0) {
            int act = config_action_from_name(v);
            if (act == DS_ACT_NONE)
                fprintf(stderr, "kbm-mapper: %s:%d: unknown action '%s'\n", path, lineno, v);
            else
                c->ads_action = act;
        }
        else if (strcmp(k,"ads.sens_scale")==0) {
            double s = atof(v);
            if (s < 0.01) s = 0.01;
            if (s > 4.0)  s = 4.0;
            c->ads_sens_scale = s;
        }
        else if (strcmp(k,"fire.sens_scale")==0) {
            double s = atof(v);
            if (s < 0.01) s = 0.01;
            if (s > 4.0)  s = 4.0;
            c->fire_sens_scale = s;
        }
        else {
            fprintf(stderr, "kbm-mapper: %s:%d: unknown key '%s'\n", path, lineno, k);
        }
    }
    fclose(fp);
    return 0;
}
