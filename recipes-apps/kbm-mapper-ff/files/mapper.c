#define _GNU_SOURCE
#include <string.h>
#include <math.h>
#include <linux/input.h>
#include <linux/input-event-codes.h>
#include "mapper.h"

/* `input_event_sec`/`_usec` are member-name macros in modern kernel
 * headers (linux/input.h): under _TIME_BITS=64 they expand to `__sec`/
 * `__usec`, otherwise to `time.tv_sec`/`time.tv_usec`. Use `e->member`. */
#ifndef input_event_sec
#define input_event_sec  time.tv_sec
#define input_event_usec time.tv_usec
#endif

static inline void set_action(struct mapper *m, enum ds_action a, bool down) {
    if (a == DS_ACT_NONE || a >= DS_ACT_MAX) return;
    uint64_t bit = 1ull << (a % 64);
    uint64_t *w  = (a < 64) ? &m->pressed_lo : &m->pressed_hi;
    if (down) *w |= bit;
    else      *w &= ~bit;
}

static inline bool is_pressed(const struct mapper *m, enum ds_action a) {
    if (a == DS_ACT_NONE || a >= DS_ACT_MAX) return false;
    uint64_t bit = 1ull << (a % 64);
    return ((a < 64) ? m->pressed_lo : m->pressed_hi) & bit;
}

/* Update pressed_since_ns timestamps from the current pressed bitmap. Called
 * once per report build so burst phase is referenced to the most recent
 * not-pressed -> pressed transition. */
static void burst_tick(struct mapper *m, int64_t now_ns) {
    for (int a = 1; a < DS_ACT_MAX; a++) {
        bool now = is_pressed(m, (enum ds_action)a);
        if (now && m->pressed_since_ns[a] == 0)        m->pressed_since_ns[a] = now_ns;
        else if (!now && m->pressed_since_ns[a] != 0)  m->pressed_since_ns[a] = 0;
    }
}

/* Deterministic per-cycle uniform in [0, 1). Same (cycle, salt) always
 * produces the same value, so is_active stays a pure function of
 * (action, now_ns). xorshift-style mixing is enough — we are sampling a
 * distribution to look human, not generating cryptographic randomness. */
static double burst_cycle_uniform(int64_t cycle_idx, uint64_t salt) {
    uint64_t x = (uint64_t)cycle_idx * 0x9e3779b97f4a7c15ULL ^ salt;
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return (double)((x >> 11) & ((1ULL<<53)-1)) / (double)(1ULL<<53);
}

/* Standard normal sample via Box-Muller from two independent uniforms,
 * each derived from a distinct salt so the two uniforms are uncorrelated. */
static double burst_cycle_normal(int64_t cycle_idx, uint64_t salt) {
    double u1 = burst_cycle_uniform(cycle_idx, salt);
    double u2 = burst_cycle_uniform(cycle_idx, salt ^ 0xb7e151628aed2a6bULL);
    if (u1 < 1e-12) u1 = 1e-12;
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

/* is_active is is_pressed gated by burst-on-hold: if burst_hz[a] > 0 and the
 * action is currently held, this returns true only during the high portion of
 * each oscillator cycle. burst_hz <= 0 is treated as a normal sustained press.
 *
 * The cadence model is three independent realism knobs (see config.h for
 * the full rationale). All are off by default and the function takes a
 * fast clockwork path when nothing is enabled:
 *
 *   burst_jitter[a]      lognormal sigma on cycle interval: a clockwork
 *                        period of 1/hz becomes (1/hz) * exp(sigma * N(0,1)),
 *                        producing a long-tailed click-interval distribution
 *                        instead of a uniform window around the mean.
 *   burst_duty_jitter[a] lognormal sigma on the duty fraction per cycle.
 *   burst_skip_prob[a]   per-cycle probability of dropping the click
 *                        (cycle becomes a silent gap).
 *
 * Used only for digital actions (buttons + analog triggers reported digitally).
 * Stick directions go through is_pressed because oscillating a stick axis
 * would introduce camera/movement jitter rather than recoil control. */
static bool is_active(const struct mapper *m, enum ds_action a, int64_t now_ns) {
    if (!is_pressed(m, a)) return false;
    double hz = m->cfg->burst_hz[a];
    if (hz <= 0) return true;
    double duty = m->cfg->burst_duty[a];
    if (duty <= 0) duty = 0.5;
    if (duty >  1) duty = 1;
    int64_t base_cycle_ns = (int64_t)(1e9 / hz);
    if (base_cycle_ns <= 0) return true;
    int64_t since = now_ns - m->pressed_since_ns[a];
    if (since < 0) since = 0;

    double jitter      = m->cfg->burst_jitter[a];
    double duty_jitter = m->cfg->burst_duty_jitter[a];
    double skip_prob   = m->cfg->burst_skip_prob[a];

    if (jitter <= 0 && duty_jitter <= 0 && skip_prob <= 0) {
        /* Clockwork fast path. */
        int64_t phase = since % base_cycle_ns;
        return phase < (int64_t)(base_cycle_ns * duty);
    }

    /* Walk forward through cycles, accumulating per-cycle widths. The
     * action id is salted in so different actions with the same hz get
     * independent patterns. Each field gets its own salt so duty noise
     * is uncorrelated with interval noise and the skip lottery.
     * Bound the loop to keep cost predictable even on a multi-minute
     * trigger hold; after 4096 cycles fall back to nominal phase. */
    uint64_t salt_iv = (uint64_t)a * 0x100000001b3ULL;
    uint64_t salt_dt = salt_iv ^ 0xa5a5a5a5a5a5a5a5ULL;
    uint64_t salt_sk = salt_iv ^ 0x5a5a5a5a5a5a5a5aULL;
    int64_t cum = 0;
    for (int64_t ci = 0; ci < 4096; ci++) {
        /* Lognormal interval multiplier, clamped so a tail draw doesn't
         * silently produce a multi-second gap that looks worse than a
         * clockwork burst would. */
        double mult = 1.0;
        if (jitter > 0) {
            double n = burst_cycle_normal(ci, salt_iv);
            mult = exp(jitter * n);
            if (mult < 0.25) mult = 0.25;
            if (mult > 4.0)  mult = 4.0;
        }
        int64_t this_cycle = (int64_t)(base_cycle_ns * mult);
        if (this_cycle <= 0) this_cycle = 1;
        int64_t next_cum = cum + this_cycle;
        if (since < next_cum) {
            /* Drop the whole cycle with skip_prob — a silent miss. */
            if (skip_prob > 0 &&
                burst_cycle_uniform(ci, salt_sk) < skip_prob) {
                return false;
            }
            double effective_duty = duty;
            if (duty_jitter > 0) {
                double n = burst_cycle_normal(ci, salt_dt);
                effective_duty = duty * exp(duty_jitter * n);
            }
            if (effective_duty < 0.05) effective_duty = 0.05;
            if (effective_duty > 0.95) effective_duty = 0.95;
            int64_t on_time = (int64_t)(this_cycle * effective_duty);
            return (since - cum) < on_time;
        }
        cum = next_cum;
    }
    int64_t phase = since % base_cycle_ns;
    return phase < (int64_t)(base_cycle_ns * duty);
}

void mapper_init(struct mapper *m, const struct config *cfg) {
    memset(m, 0, sizeof(*m));
    m->cfg = cfg;
}

void mapper_handle_event(struct mapper *m, const struct input_event *e) {
    switch (e->type) {
    case EV_KEY: {
        bool down = (e->value != 0);  /* 1 = down, 2 = autorepeat (treat as down) */
        enum ds_action a;
        if (e->code >= KBM_KEYTABLE_SIZE) return;

        /* Mouse buttons live in BTN_* range (0x110+). We dispatch them */
        /* via btn_action[code & 0xFF] to keep the table small.        */
        if (e->code >= BTN_MOUSE && e->code < BTN_MOUSE + 16) {
            a = (enum ds_action)m->cfg->btn_action[e->code & 0xFF];
        } else {
            a = (enum ds_action)m->cfg->key_action[e->code];
        }
        set_action(m, a, down);
        break;
    }
    case EV_REL:
        if (e->code == REL_X) m->pending_dx += e->value;
        else if (e->code == REL_Y) m->pending_dy += e->value;
        break;
    case EV_SYN:
        if (m->pending_dx || m->pending_dy) {
            int64_t t_ns = (int64_t)e->input_event_sec  * 1000000000ll
                         + (int64_t)e->input_event_usec * 1000ll;
            int idx = m->ring_head;
            m->ring[idx].t_ns = t_ns;
            m->ring[idx].dx   = m->pending_dx;
            m->ring[idx].dy   = m->pending_dy;
            m->ring_head = (idx + 1) % KBM_SAMPLE_RING_CAP;
            if (m->ring_count < KBM_SAMPLE_RING_CAP) m->ring_count++;
            m->pending_dx = m->pending_dy = 0;
        }
        break;
    }
}

/* Sum mouse counts within the velocity window ending at now_ns.
 * Returns (dx, dy) summed and the actual covered time span in ms. */
static void window_sum(const struct mapper *m, int64_t now_ns,
                       double *out_dx, double *out_dy, double *out_dt_ms) {
    double win_ns = m->cfg->window_ms * 1e6;
    int64_t cutoff = now_ns - (int64_t)win_ns;
    double dx = 0, dy = 0;
    int64_t oldest = now_ns;

    for (int i = 0; i < m->ring_count; i++) {
        int idx = (m->ring_head - 1 - i + KBM_SAMPLE_RING_CAP) % KBM_SAMPLE_RING_CAP;
        if (m->ring[idx].t_ns < cutoff) break;
        dx += m->ring[idx].dx;
        dy += m->ring[idx].dy;
        oldest = m->ring[idx].t_ns;
    }
    *out_dx    = dx;
    *out_dy    = dy;
    *out_dt_ms = (now_ns - oldest) / 1e6;
    if (*out_dt_ms < 1.0) *out_dt_ms = m->cfg->window_ms;
}

/* Map (vx, vy) counts/ms through the response chain and write right-stick
 * bytes. Updates rotation-debt accumulators. */
static void mouse_to_stick(struct mapper *m, double vx, double vy,
                           uint8_t *rx_out, uint8_t *ry_out) {
    const struct config *c = m->cfg;
    double sens = c->sens_counts_ms > 0 ? c->sens_counts_ms : 1.0;

    /* Add debt-drain contribution and decay the debt. */
    double nx = vx / sens + m->debt_x;
    double ny = vy / sens + m->debt_y;

    double mag = sqrt(nx*nx + ny*ny);
    if (mag <= 1e-9) {
        m->debt_x = m->debt_y = 0;
        *rx_out = *ry_out = DS_STICK_CENTER;
        return;
    }
    double dirx = nx / mag;
    double diry = ny / mag;

    /* Saturation overflow goes to debt; under-saturated input drains debt. */
    if (mag > c->outer_sat) {
        double excess = mag - c->outer_sat;
        m->debt_x = dirx * excess;
        m->debt_y = diry * excess;
        mag = c->outer_sat;
    } else {
        double drain = c->debt_drain;
        m->debt_x *= (1.0 - drain);
        m->debt_y *= (1.0 - drain);
    }

    /* Curve + anti-deadzone, then clamp. */
    double curved = pow(mag, c->curve_exp);
    double adz = c->anti_deadzone;
    double out_mag = adz + (c->outer_sat - adz) * curved;
    if (out_mag > c->outer_sat) out_mag = c->outer_sat;
    if (out_mag < 0)            out_mag = 0;

    double ox = dirx * out_mag;
    double oy = diry * out_mag;

    int rx = (int)lrint(DS_STICK_CENTER + ox * 127.0);
    int ry = (int)lrint(DS_STICK_CENTER + oy * 127.0);
    if (rx < 0) rx = 0; else if (rx > 255) rx = 255;
    if (ry < 0) ry = 0; else if (ry > 255) ry = 255;
    *rx_out = (uint8_t)rx;
    *ry_out = (uint8_t)ry;
}

static uint8_t left_stick_axis(const struct mapper *m, enum ds_action neg, enum ds_action pos) {
    int v = 0;
    if (is_pressed(m, neg)) v -= 127;
    if (is_pressed(m, pos)) v += 127;
    /* Diagonal normalize so up+right doesn't exceed magnitude 1.    */
    /* (Done by the caller for the (x,y) pair via stick_clamp_pair.) */
    return (uint8_t)(DS_STICK_CENTER + v);
}

static void stick_clamp_pair(uint8_t *x, uint8_t *y) {
    int dx = (int)*x - DS_STICK_CENTER;
    int dy = (int)*y - DS_STICK_CENTER;
    double mag = sqrt((double)(dx*dx + dy*dy));
    if (mag > 127.0) {
        double s = 127.0 / mag;
        *x = (uint8_t)(DS_STICK_CENTER + (int)lrint(dx * s));
        *y = (uint8_t)(DS_STICK_CENTER + (int)lrint(dy * s));
    }
}

static uint8_t dpad_from_state(const struct mapper *m) {
    bool u = is_pressed(m, DS_ACT_DPAD_UP);
    bool d = is_pressed(m, DS_ACT_DPAD_DOWN);
    bool l = is_pressed(m, DS_ACT_DPAD_LEFT);
    bool r = is_pressed(m, DS_ACT_DPAD_RIGHT);
    if (u && r) return DS_DPAD_NE;
    if (u && l) return DS_DPAD_NW;
    if (d && r) return DS_DPAD_SE;
    if (d && l) return DS_DPAD_SW;
    if (u)      return DS_DPAD_N;
    if (d)      return DS_DPAD_S;
    if (l)      return DS_DPAD_W;
    if (r)      return DS_DPAD_E;
    return DS_DPAD_NONE;
}

void mapper_build_report(struct mapper *m, uint8_t *buf, size_t len, int64_t now_ns) {
    if (len < DS_REPORT_LEN) return;
    memset(buf, 0, len);
    buf[0] = DS_REPORT_ID;

    /* Refresh per-action press timestamps for burst-on-hold phase reference. */
    burst_tick(m, now_ns);

    /* Left stick from keyboard. Stick axes are NOT burst-gated — oscillating
     * a movement direction at e.g. 10 Hz produces visible camera/move jitter,
     * which is the opposite of the recoil-control use case. */
    uint8_t lx = left_stick_axis(m, DS_ACT_LSTICK_LEFT, DS_ACT_LSTICK_RIGHT);
    uint8_t ly = left_stick_axis(m, DS_ACT_LSTICK_UP,   DS_ACT_LSTICK_DOWN);
    stick_clamp_pair(&lx, &ly);
    buf[DS_OFF_LX] = lx;
    buf[DS_OFF_LY] = ly;

    /* Right stick from mouse velocity window. */
    double dx, dy, dt_ms;
    window_sum(m, now_ns, &dx, &dy, &dt_ms);
    double vx = dx / dt_ms;
    double vy = dy / dt_ms;
    mouse_to_stick(m, vx, vy, &buf[DS_OFF_RX], &buf[DS_OFF_RY]);

    /* Apply sensitivity scaling tied to action state. ADS scaling (smaller
     * stick deflection while aiming) is the standard console behavior; fire
     * scaling makes spray control less twitchy by dampening manual mouse
     * compensation overshoot. Composes multiplicatively when both actions
     * are held. Operates in stick-center-relative space so scaling does
     * not introduce a centering bias. */
    double sens_scale = 1.0;
    if (m->cfg->ads_action  != DS_ACT_NONE && is_pressed(m, m->cfg->ads_action))
        sens_scale *= m->cfg->ads_sens_scale;
    if (m->cfg->recoil_action != DS_ACT_NONE && is_pressed(m, m->cfg->recoil_action))
        sens_scale *= m->cfg->fire_sens_scale;
    if (sens_scale != 1.0) {
        int rx = (int)buf[DS_OFF_RX] - 128;
        int ry = (int)buf[DS_OFF_RY] - 128;
        rx = (int)(rx * sens_scale);
        ry = (int)(ry * sens_scale);
        if (rx < -128) rx = -128; if (rx > 127) rx = 127;
        if (ry < -128) ry = -128; if (ry > 127) ry = 127;
        buf[DS_OFF_RX] = (uint8_t)(rx + 128);
        buf[DS_OFF_RY] = (uint8_t)(ry + 128);
    }

    /* Recoil compensation: while the configured recoil_action is logically
     * held (is_pressed, not is_active, so the bias is steady over a burst
     * rather than pulsing with it) push the right stick by (recoil_x,
     * recoil_y) fractions of full deflection. Y positive pushes down to
     * counter games' vertical recoil. X is for weapons with a consistent
     * left/right drift; 0 for the random-recoil case. Bias is additive on
     * top of the player's own mouse-driven counter motion. */
    if (m->cfg->recoil_action != DS_ACT_NONE &&
        (m->cfg->recoil_x != 0.0 || m->cfg->recoil_y != 0.0) &&
        is_pressed(m, m->cfg->recoil_action)) {
        int bx = (int)(m->cfg->recoil_x * 127.0);
        int by = (int)(m->cfg->recoil_y * 127.0);
        int rx = (int)buf[DS_OFF_RX] + bx;
        int ry = (int)buf[DS_OFF_RY] + by;
        if (rx <   0) rx =   0; if (rx > 255) rx = 255;
        if (ry <   0) ry =   0; if (ry > 255) ry = 255;
        buf[DS_OFF_RX] = (uint8_t)rx;
        buf[DS_OFF_RY] = (uint8_t)ry;
    }

    /* Triggers: analog when active. */
    buf[DS_OFF_L2] = is_active(m, DS_ACT_L2, now_ns) ? 255 : 0;
    buf[DS_OFF_R2] = is_active(m, DS_ACT_R2, now_ns) ? 255 : 0;

    buf[DS_OFF_SEQ] = m->seq++;

    /* Buttons + dpad. */
    uint8_t btn0 = dpad_from_state(m);
    if (is_active(m, DS_ACT_SQUARE,   now_ns)) btn0 |= DS_BTN0_SQUARE;
    if (is_active(m, DS_ACT_CROSS,    now_ns)) btn0 |= DS_BTN0_CROSS;
    if (is_active(m, DS_ACT_CIRCLE,   now_ns)) btn0 |= DS_BTN0_CIRCLE;
    if (is_active(m, DS_ACT_TRIANGLE, now_ns)) btn0 |= DS_BTN0_TRIANGLE;
    buf[DS_OFF_BTN0] = btn0;

    uint8_t btn1 = 0;
    if (is_active(m, DS_ACT_L1,      now_ns)) btn1 |= DS_BTN1_L1;
    if (is_active(m, DS_ACT_R1,      now_ns)) btn1 |= DS_BTN1_R1;
    if (is_active(m, DS_ACT_L2,      now_ns)) btn1 |= DS_BTN1_L2;
    if (is_active(m, DS_ACT_R2,      now_ns)) btn1 |= DS_BTN1_R2;
    if (is_active(m, DS_ACT_CREATE,  now_ns)) btn1 |= DS_BTN1_CREATE;
    if (is_active(m, DS_ACT_OPTIONS, now_ns)) btn1 |= DS_BTN1_OPTIONS;
    if (is_active(m, DS_ACT_L3,      now_ns)) btn1 |= DS_BTN1_L3;
    if (is_active(m, DS_ACT_R3,      now_ns)) btn1 |= DS_BTN1_R3;
    buf[DS_OFF_BTN1] = btn1;

    uint8_t btn2 = 0;
    if (is_active(m, DS_ACT_PS,       now_ns)) btn2 |= DS_BTN2_PS;
    if (is_active(m, DS_ACT_TOUCHPAD, now_ns)) btn2 |= DS_BTN2_TOUCHPAD;
    if (is_active(m, DS_ACT_MUTE,     now_ns)) btn2 |= DS_BTN2_MUTE;
    buf[DS_OFF_BTN2] = btn2;

    /* Timestamp: 32-bit LE, 333us units, taken from now_ns. */
    uint32_t ts = (uint32_t)((now_ns / 1000) / 333);
    buf[DS_OFF_TS+0] = (uint8_t)(ts);
    buf[DS_OFF_TS+1] = (uint8_t)(ts >> 8);
    buf[DS_OFF_TS+2] = (uint8_t)(ts >> 16);
    buf[DS_OFF_TS+3] = (uint8_t)(ts >> 24);
}
