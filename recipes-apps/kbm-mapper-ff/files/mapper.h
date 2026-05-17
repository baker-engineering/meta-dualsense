#ifndef KBM_MAPPER_H
#define KBM_MAPPER_H
#include <stdint.h>
#include <stdbool.h>
#include <linux/input.h>
#include "report.h"
#include "config.h"

#define KBM_SAMPLE_RING_CAP 256

struct mouse_sample {
    int64_t  t_ns;
    int32_t  dx;
    int32_t  dy;
};

struct mapper {
    const struct config *cfg;

    /* per-action pressed bitmap (1 bit per action) */
    uint64_t pressed_lo, pressed_hi;

    /* per-mouse-button accumulator for current EV_SYN frame */
    int32_t pending_dx, pending_dy;

    /* sliding window of mouse motion samples */
    struct mouse_sample ring[KBM_SAMPLE_RING_CAP];
    int ring_head;   /* next write slot */
    int ring_count;  /* fill level */

    /* rotation-debt buffer: residual mouse counts not yet expressed by */
    /* the (saturated) stick. Drained over subsequent ticks.            */
    double debt_x, debt_y;

    /* Per-action timestamp (monotonic ns) of the most recent
     * not-pressed -> pressed transition; 0 when the action is currently
     * released. Used as the phase reference for burst-on-hold tuning. */
    int64_t pressed_since_ns[DS_ACT_MAX];

    uint8_t seq;
};

void mapper_init(struct mapper *m, const struct config *cfg);

/* Feed a raw evdev event from any input fd. */
void mapper_handle_event(struct mapper *m, const struct input_event *e);

/* Build a 64-byte DualSense USB input report into out_buf. */
void mapper_build_report(struct mapper *m, uint8_t *out_buf, size_t out_len, int64_t now_ns);

#endif
