/*
 * kbm-passthrough — read /dev/input/event* keyboard+mouse and forward them
 * as HID reports to /dev/hidg0 (boot keyboard) and /dev/hidg1 (mouse).
 *
 * The configfs gadget that creates hidg0/hidg1 is set up by the oneshot
 * kbm-passthrough-setup service (see recipes-core/kbm-passthrough-setup/).
 *
 * Mutually exclusive with the DualSense emulation mode via systemd
 * Conflicts= — only one of {dualsense-ffsd, kbm-passthrough} can own the
 * UDC at a time.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <linux/input.h>

#define HIDG_KBD     "/dev/hidg0"
#define HIDG_MOUSE   "/dev/hidg1"
#define MAX_INPUTS   16
#define KBD_KEYS_MAX 6   /* boot keyboard report supports 6 simultaneous keys */

static volatile sig_atomic_t stopped = 0;
static void on_signal(int sig) { (void)sig; stopped = 1; }

/* === Linux KEY_* -> HID Keyboard/Keypad usage ID (page 0x07) ===
 *
 * Indexed by Linux KEY_* code. Zero means "no HID mapping" — the key is
 * dropped silently. Modifier keys also map to a usage but are handled via
 * the modifier byte instead (see kbd_modifier_bit). Covers the common
 * 104-key + a handful of multimedia / arrow / numpad keys. Anything not
 * listed is simply ignored. */
static const uint8_t key_to_hid[256] = {
    [KEY_ESC]         = 0x29,
    [KEY_1]           = 0x1E, [KEY_2] = 0x1F, [KEY_3] = 0x20, [KEY_4] = 0x21,
    [KEY_5]           = 0x22, [KEY_6] = 0x23, [KEY_7] = 0x24, [KEY_8] = 0x25,
    [KEY_9]           = 0x26, [KEY_0] = 0x27,
    [KEY_MINUS]       = 0x2D, [KEY_EQUAL] = 0x2E,
    [KEY_BACKSPACE]   = 0x2A, [KEY_TAB] = 0x2B,
    [KEY_Q] = 0x14, [KEY_W] = 0x1A, [KEY_E] = 0x08, [KEY_R] = 0x15,
    [KEY_T] = 0x17, [KEY_Y] = 0x1C, [KEY_U] = 0x18, [KEY_I] = 0x0C,
    [KEY_O] = 0x12, [KEY_P] = 0x13,
    [KEY_LEFTBRACE] = 0x2F, [KEY_RIGHTBRACE] = 0x30,
    [KEY_ENTER]     = 0x28,
    [KEY_A] = 0x04, [KEY_S] = 0x16, [KEY_D] = 0x07, [KEY_F] = 0x09,
    [KEY_G] = 0x0A, [KEY_H] = 0x0B, [KEY_J] = 0x0D, [KEY_K] = 0x0E,
    [KEY_L] = 0x0F,
    [KEY_SEMICOLON] = 0x33, [KEY_APOSTROPHE] = 0x34, [KEY_GRAVE] = 0x35,
    [KEY_BACKSLASH] = 0x31,
    [KEY_Z] = 0x1D, [KEY_X] = 0x1B, [KEY_C] = 0x06, [KEY_V] = 0x19,
    [KEY_B] = 0x05, [KEY_N] = 0x11, [KEY_M] = 0x10,
    [KEY_COMMA] = 0x36, [KEY_DOT] = 0x37, [KEY_SLASH] = 0x38,
    [KEY_KPASTERISK] = 0x55,
    [KEY_SPACE]    = 0x2C,
    [KEY_CAPSLOCK] = 0x39,
    [KEY_F1]  = 0x3A, [KEY_F2]  = 0x3B, [KEY_F3]  = 0x3C, [KEY_F4]  = 0x3D,
    [KEY_F5]  = 0x3E, [KEY_F6]  = 0x3F, [KEY_F7]  = 0x40, [KEY_F8]  = 0x41,
    [KEY_F9]  = 0x42, [KEY_F10] = 0x43, [KEY_F11] = 0x44, [KEY_F12] = 0x45,
    [KEY_NUMLOCK]    = 0x53,
    [KEY_SCROLLLOCK] = 0x47,
    [KEY_KP7] = 0x5F, [KEY_KP8] = 0x60, [KEY_KP9] = 0x61, [KEY_KPMINUS] = 0x56,
    [KEY_KP4] = 0x5C, [KEY_KP5] = 0x5D, [KEY_KP6] = 0x5E, [KEY_KPPLUS]  = 0x57,
    [KEY_KP1] = 0x59, [KEY_KP2] = 0x5A, [KEY_KP3] = 0x5B,
    [KEY_KP0]    = 0x62, [KEY_KPDOT] = 0x63,
    [KEY_KPENTER]= 0x58, [KEY_KPSLASH] = 0x54,
    [KEY_SYSRQ]  = 0x46,  /* PrintScreen */
    [KEY_HOME]   = 0x4A, [KEY_UP]     = 0x52, [KEY_PAGEUP]   = 0x4B,
    [KEY_LEFT]   = 0x50, [KEY_RIGHT]  = 0x4F,
    [KEY_END]    = 0x4D, [KEY_DOWN]   = 0x51, [KEY_PAGEDOWN] = 0x4E,
    [KEY_INSERT] = 0x49, [KEY_DELETE] = 0x4C,
    [KEY_PAUSE]  = 0x48,
    [KEY_102ND]  = 0x64,
    [KEY_COMPOSE]= 0x65,  /* application / menu */
};

/* Modifier-keys bit map: bit-in-byte-0 for each KEY_*. 0 = not a modifier. */
static uint8_t kbd_modifier_bit(int key) {
    switch (key) {
    case KEY_LEFTCTRL:   return 0x01;
    case KEY_LEFTSHIFT:  return 0x02;
    case KEY_LEFTALT:    return 0x04;
    case KEY_LEFTMETA:   return 0x08;
    case KEY_RIGHTCTRL:  return 0x10;
    case KEY_RIGHTSHIFT: return 0x20;
    case KEY_RIGHTALT:   return 0x40;
    case KEY_RIGHTMETA:  return 0x80;
    default:             return 0;
    }
}

/* === Input slot tracking + hot-plug (same shape as kbm-mapper-ff) === */

struct input_slot {
    int  fd;
    char path[64];
};
static struct input_slot input_slots[MAX_INPUTS];

static void input_slots_init(void) {
    for (int i = 0; i < MAX_INPUTS; i++) input_slots[i].fd = -1;
}

static int input_slots_find_path(const char *path) {
    for (int i = 0; i < MAX_INPUTS; i++)
        if (input_slots[i].fd >= 0 && strcmp(input_slots[i].path, path) == 0)
            return i;
    return -1;
}

static int input_slots_add(int fd, const char *path) {
    for (int i = 0; i < MAX_INPUTS; i++) {
        if (input_slots[i].fd < 0) {
            input_slots[i].fd = fd;
            snprintf(input_slots[i].path, sizeof input_slots[i].path, "%s", path);
            return i;
        }
    }
    return -1;
}

static void input_slots_remove_fd(int fd) {
    for (int i = 0; i < MAX_INPUTS; i++) {
        if (input_slots[i].fd == fd) {
            input_slots[i].fd = -1;
            input_slots[i].path[0] = 0;
            return;
        }
    }
}

static int test_bit(const unsigned char *bits, int nr) {
    return (bits[nr / 8] >> (nr % 8)) & 1;
}

static int classify_and_open(const char *path) {
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;

    unsigned char ev_bits[(EV_MAX + 7) / 8] = {0};
    unsigned char key_bits[(KEY_MAX + 7) / 8] = {0};
    unsigned char rel_bits[(REL_MAX + 7) / 8] = {0};
    ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits);
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits);
    ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel_bits)), rel_bits);

    int looks_kbd   = test_bit(ev_bits, EV_KEY) && test_bit(key_bits, KEY_SPACE);
    int looks_mouse = test_bit(ev_bits, EV_REL) && test_bit(rel_bits, REL_X)
                      && test_bit(rel_bits, REL_Y) && test_bit(key_bits, BTN_LEFT);
    if (!looks_kbd && !looks_mouse) { close(fd); return -1; }

    char name[256] = "?";
    ioctl(fd, EVIOCGNAME(sizeof(name)), name);
    fprintf(stderr, "kbm-passthrough: opened %s (%s) as %s%s\n",
            path, name, looks_kbd ? "keyboard" : "", looks_mouse ? "mouse" : "");

    if (ioctl(fd, EVIOCGRAB, 1) < 0)
        fprintf(stderr, "kbm-passthrough: EVIOCGRAB %s failed: %s\n", path, strerror(errno));

    return fd;
}

static int scan_inputs(void) {
    int n = 0;
    DIR *d = opendir("/dev/input");
    if (!d) { perror("/dev/input"); return 0; }
    struct dirent *de;
    while ((de = readdir(d)) && n < MAX_INPUTS) {
        if (strncmp(de->d_name, "event", 5) != 0) continue;
        char path[sizeof("/dev/input/") + sizeof(de->d_name)];
        snprintf(path, sizeof path, "/dev/input/%s", de->d_name);
        int fd = classify_and_open(path);
        if (fd >= 0) {
            if (input_slots_add(fd, path) < 0) { close(fd); break; }
            n++;
        }
    }
    closedir(d);
    return n;
}

static int open_inotify(void) {
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) { perror("inotify_init1"); return -1; }
    if (inotify_add_watch(fd, "/dev/input", IN_CREATE | IN_ATTRIB) < 0) {
        perror("inotify_add_watch /dev/input");
        close(fd);
        return -1;
    }
    return fd;
}

/* === HID kbd state === */

struct kbd_state {
    uint8_t modifiers;
    uint8_t keys[KBD_KEYS_MAX]; /* HID usage IDs currently pressed; 0 = empty */
};

static bool kbd_press(struct kbd_state *s, uint8_t usage) {
    for (int i = 0; i < KBD_KEYS_MAX; i++) if (s->keys[i] == usage) return false;
    for (int i = 0; i < KBD_KEYS_MAX; i++) {
        if (s->keys[i] == 0) { s->keys[i] = usage; return true; }
    }
    return false; /* roll-over */
}

static bool kbd_release(struct kbd_state *s, uint8_t usage) {
    for (int i = 0; i < KBD_KEYS_MAX; i++) {
        if (s->keys[i] == usage) { s->keys[i] = 0; return true; }
    }
    return false;
}

static void kbd_write_report(int hidg_fd, const struct kbd_state *s) {
    uint8_t report[8] = { s->modifiers, 0,
                          s->keys[0], s->keys[1], s->keys[2],
                          s->keys[3], s->keys[4], s->keys[5] };
    ssize_t w = write(hidg_fd, report, sizeof report);
    if (w < 0 && errno != EAGAIN && errno != ESHUTDOWN && errno != EPIPE)
        fprintf(stderr, "kbm-passthrough: hidg kbd write: %s\n", strerror(errno));
}

/* Send an all-zero keyboard report — clears any phantom modifier/key state
 * Windows may have inherited. Called on startup (clean baseline before any
 * forwarding) and on shutdown (so the last report Windows sees from this
 * gadget instance is unambiguously "no keys held"). Without this, the
 * emulation->passthrough hotkey hand-off could leak a stuck Ctrl into
 * Windows' merged-modifier state across all HID keyboards. */
static void kbd_send_release_all(int hidg_fd) {
    struct kbd_state z = {0};
    kbd_write_report(hidg_fd, &z);
}

/* === HID mouse state === */

struct mouse_state {
    uint8_t  buttons;     /* bits 0..2: L, R, M (descriptor declares 3) */
    int32_t  pending_dx;  /* accumulated since last EV_SYN */
    int32_t  pending_dy;
    int32_t  pending_wheel;
};

static uint8_t btn_to_bit(int code) {
    switch (code) {
    case BTN_LEFT:    return 0x01;
    case BTN_RIGHT:   return 0x02;
    case BTN_MIDDLE:  return 0x04;
    /* SIDE/EXTRA dropped — descriptor declares 3 buttons (boot-mouse style)
     * because the 5-button variant tripped CM_PROB_FAILED_START on Windows
     * during enumeration. */
    default:          return 0;
    }
}

static int8_t clamp_int8(int32_t v) {
    if (v >  127) return  127;
    if (v < -127) return -127;
    return (int8_t)v;
}

static void mouse_flush(int hidg_fd, struct mouse_state *s) {
    int8_t x = clamp_int8(s->pending_dx);
    int8_t y = clamp_int8(s->pending_dy);
    int8_t w = clamp_int8(s->pending_wheel);
    /* 4-byte boot-mouse report: byte 0 = buttons (3 used, 5 padding),
     * bytes 1-3 = signed int8 X / Y / wheel. */
    uint8_t report[4] = { s->buttons, (uint8_t)x, (uint8_t)y, (uint8_t)w };
    ssize_t wr = write(hidg_fd, report, sizeof report);
    if (wr < 0 && errno != EAGAIN && errno != ESHUTDOWN && errno != EPIPE)
        fprintf(stderr, "kbm-passthrough: hidg mouse write: %s\n", strerror(errno));
    /* Drain any residual motion that exceeded int8 range — we don't carry
     * over to the next tick to avoid lag. */
    s->pending_dx = s->pending_dy = s->pending_wheel = 0;
}

/* All-zero mouse report — releases buttons, zero motion. Pair with
 * kbd_send_release_all() at startup/shutdown. */
static void mouse_send_release_all(int hidg_fd) {
    uint8_t report[4] = {0};
    ssize_t wr = write(hidg_fd, report, sizeof report);
    if (wr < 0 && errno != EAGAIN && errno != ESHUTDOWN && errno != EPIPE)
        fprintf(stderr, "kbm-passthrough: hidg mouse release write: %s\n", strerror(errno));
}

/* === main === */

static int open_hidg(const char *path) {
    for (int i = 0; i < 100; i++) {
        int fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) return fd;
        if (errno != ENOENT && errno != EACCES) {
            fprintf(stderr, "kbm-passthrough: open %s: %s\n", path, strerror(errno));
            return -1;
        }
        usleep(50000); /* 50 ms x 100 = 5s grace for hidg node + permissions */
    }
    fprintf(stderr, "kbm-passthrough: timed out waiting for %s\n", path);
    return -1;
}

/* === Mode-switch hotkey ===
 *
 * Configurable chord read from /etc/kbm-mapper.conf at startup. All keys
 * must be held simultaneously for >=1 s to fire
 * `systemctl start dualsense-ffsd`. Mirror of the same logic in
 * dualsense-ffsd; we duplicate the minimal name->code table to avoid
 * pulling in the entire mapper config parser. */

#define HOTKEY_MAX 8

struct hotkey_state {
    int     codes[HOTKEY_MAX];
    int     n;
    bool    pressed[HOTKEY_MAX];
    int64_t all_since_ns;
    bool    fired;
};
static struct hotkey_state hotkey = {0};

static int64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

/* Tiny key-name table covering common chord choices: modifiers, ESC, TAB,
 * SPACE, ENTER, F1..F12, A..Z, 0..9. Anything else falls through to "unknown"
 * and the user can pick something we know. */
static int hotkey_key_name_to_code(const char *s) {
    static const struct { const char *n; int c; } table[] = {
        {"LEFTCTRL",KEY_LEFTCTRL},{"RIGHTCTRL",KEY_RIGHTCTRL},
        {"LEFTSHIFT",KEY_LEFTSHIFT},{"RIGHTSHIFT",KEY_RIGHTSHIFT},
        {"LEFTALT",KEY_LEFTALT},{"RIGHTALT",KEY_RIGHTALT},
        {"LEFTMETA",KEY_LEFTMETA},{"RIGHTMETA",KEY_RIGHTMETA},
        {"ESC",KEY_ESC},{"TAB",KEY_TAB},{"SPACE",KEY_SPACE},
        {"ENTER",KEY_ENTER},{"BACKSPACE",KEY_BACKSPACE},{"GRAVE",KEY_GRAVE},
        {"UP",KEY_UP},{"DOWN",KEY_DOWN},{"LEFT",KEY_LEFT},{"RIGHT",KEY_RIGHT},
        {"F1",KEY_F1},{"F2",KEY_F2},{"F3",KEY_F3},{"F4",KEY_F4},
        {"F5",KEY_F5},{"F6",KEY_F6},{"F7",KEY_F7},{"F8",KEY_F8},
        {"F9",KEY_F9},{"F10",KEY_F10},{"F11",KEY_F11},{"F12",KEY_F12},
        {"A",KEY_A},{"B",KEY_B},{"C",KEY_C},{"D",KEY_D},{"E",KEY_E},
        {"F",KEY_F},{"G",KEY_G},{"H",KEY_H},{"I",KEY_I},{"J",KEY_J},
        {"K",KEY_K},{"L",KEY_L},{"M",KEY_M},{"N",KEY_N},{"O",KEY_O},
        {"P",KEY_P},{"Q",KEY_Q},{"R",KEY_R},{"S",KEY_S},{"T",KEY_T},
        {"U",KEY_U},{"V",KEY_V},{"W",KEY_W},{"X",KEY_X},{"Y",KEY_Y},{"Z",KEY_Z},
        {"0",KEY_0},{"1",KEY_1},{"2",KEY_2},{"3",KEY_3},{"4",KEY_4},
        {"5",KEY_5},{"6",KEY_6},{"7",KEY_7},{"8",KEY_8},{"9",KEY_9},
        {NULL,0}
    };
    for (int i = 0; table[i].n; i++)
        if (strcasecmp(table[i].n, s) == 0) return table[i].c;
    return -1;
}

/* Parse hotkey.mode_toggle from /etc/kbm-mapper.conf at startup. Default
 * is LEFTCTRL+ESC if the file is missing or has no hotkey line. */
static void hotkey_load(const char *path) {
    hotkey.n = 0;
    hotkey.codes[hotkey.n++] = KEY_LEFTCTRL;
    hotkey.codes[hotkey.n++] = KEY_ESC;
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    char line[512];
    while (fgets(line, sizeof line, fp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "hotkey.mode_toggle=", 19) != 0) continue;
        p += 19;
        char *end = strchr(p, '\n'); if (end) *end = 0;
        end = strchr(p, '#');        if (end) *end = 0;
        hotkey.n = 0;
        while (*p && hotkey.n < HOTKEY_MAX) {
            char *sep = p;
            while (*sep && *sep != '+' && *sep != ',') sep++;
            int had_sep = (*sep != 0);
            if (had_sep) *sep = 0;
            while (*p == ' ' || *p == '\t') p++;
            char *e = p + strlen(p);
            while (e > p && (e[-1]==' '||e[-1]=='\t')) *--e = 0;
            if (*p) {
                int code = hotkey_key_name_to_code(p);
                if (code >= 0) hotkey.codes[hotkey.n++] = code;
            }
            if (!had_sep) break;
            p = sep + 1;
        }
        break;
    }
    fclose(fp);
    if (hotkey.n == 0) {
        hotkey.codes[hotkey.n++] = KEY_LEFTCTRL;
        hotkey.codes[hotkey.n++] = KEY_ESC;
    }
}

static void hotkey_update(const struct input_event *e, int64_t now_ns) {
    if (e->type != EV_KEY) return;
    if (hotkey.n == 0) return;
    bool relevant = false;
    for (int i = 0; i < hotkey.n; i++) {
        if (e->code == hotkey.codes[i]) {
            hotkey.pressed[i] = (e->value != 0);
            relevant = true;
            break;
        }
    }
    if (!relevant) return;
    bool all = true;
    for (int i = 0; i < hotkey.n; i++)
        if (!hotkey.pressed[i]) { all = false; break; }
    if (all) {
        if (hotkey.all_since_ns == 0)
            hotkey.all_since_ns = now_ns;
    } else {
        hotkey.all_since_ns = 0;
        hotkey.fired = false;
    }
}

static bool hotkey_should_fire(int64_t now_ns) {
    if (hotkey.fired) return false;
    if (hotkey.all_since_ns == 0) return false;
    int64_t held_ms = (now_ns - hotkey.all_since_ns) / 1000000;
    if (held_ms >= 1000) { hotkey.fired = true; return true; }
    return false;
}

static void hotkey_fire_switch_to_emulation(void) {
    fprintf(stderr, "kbm-passthrough: Ctrl+Esc held 1 s -> switching to emulation\n");
    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/systemctl", "systemctl", "start", "dualsense-ffsd", (char *)NULL);
        execl("/usr/bin/systemctl", "systemctl", "start", "dualsense-ffsd", (char *)NULL);
        _exit(127);
    }
}

int main(void) {
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    hotkey_load("/etc/kbm-mapper.conf");

    int hidg_kbd   = open_hidg(HIDG_KBD);
    int hidg_mouse = open_hidg(HIDG_MOUSE);
    if (hidg_kbd < 0 || hidg_mouse < 0) return 1;
    fprintf(stderr, "kbm-passthrough: hidg0/hidg1 open\n");

    /* Clean baseline: clear any phantom modifier/button state on the new
     * gadget instance. The emulation->passthrough hotkey hand-off leaves the
     * user's fingers physically on Ctrl+Esc, and the kernel can drop the
     * press events that occurred before our evdev open. Without this, Windows'
     * merged-modifier-across-keyboards view could attribute a stuck Ctrl to
     * the BBB keyboard and apply it to the directly-connected keyboard too. */
    kbd_send_release_all(hidg_kbd);
    mouse_send_release_all(hidg_mouse);

    input_slots_init();
    int n_inputs = scan_inputs();
    if (n_inputs == 0)
        fprintf(stderr, "kbm-passthrough: no keyboard or mouse under /dev/input\n");
    int inotify_fd = open_inotify();

    int ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev = { .events = EPOLLIN };
    if (inotify_fd >= 0) {
        ev.data.fd = inotify_fd;
        epoll_ctl(ep, EPOLL_CTL_ADD, inotify_fd, &ev);
    }
    for (int i = 0; i < MAX_INPUTS; i++) {
        if (input_slots[i].fd < 0) continue;
        ev.data.fd = input_slots[i].fd;
        epoll_ctl(ep, EPOLL_CTL_ADD, input_slots[i].fd, &ev);
    }

    struct kbd_state kbd = {0};
    struct mouse_state mouse = {0};

    while (!stopped) {
        struct epoll_event events[8 + MAX_INPUTS];
        int n = epoll_wait(ep, events, sizeof(events) / sizeof(events[0]), -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == inotify_fd) {
                char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
                ssize_t r = read(inotify_fd, buf, sizeof buf);
                if (r <= 0) continue;
                for (char *p = buf; p < buf + r; ) {
                    struct inotify_event *iev = (struct inotify_event *)p;
                    if (iev->len > 0 && strncmp(iev->name, "event", 5) == 0) {
                        char path[64];
                        snprintf(path, sizeof path, "/dev/input/%s", iev->name);
                        if (input_slots_find_path(path) < 0) {
                            int newfd = classify_and_open(path);
                            if (newfd >= 0) {
                                if (input_slots_add(newfd, path) < 0) {
                                    fprintf(stderr, "kbm-passthrough: input table full, ignoring %s\n", path);
                                    close(newfd);
                                } else {
                                    struct epoll_event nev = { .events = EPOLLIN };
                                    nev.data.fd = newfd;
                                    epoll_ctl(ep, EPOLL_CTL_ADD, newfd, &nev);
                                }
                            }
                        }
                    }
                    p += sizeof(struct inotify_event) + iev->len;
                }
                continue;
            }

            /* evdev fd: drain events and apply them. */
            struct input_event ie;
            ssize_t r;
            bool kbd_dirty = false;
            while ((r = read(fd, &ie, sizeof ie)) == (ssize_t)sizeof ie) {
                hotkey_update(&ie, mono_ns());
                if (ie.type == EV_KEY) {
                    if (ie.value == 2) continue; /* ignore auto-repeat */
                    bool pressed = (ie.value == 1);
                    uint8_t modbit = kbd_modifier_bit(ie.code);
                    if (modbit) {
                        if (pressed) kbd.modifiers |=  modbit;
                        else         kbd.modifiers &= ~modbit;
                        kbd_dirty = true;
                    } else if (ie.code < (int)(sizeof(key_to_hid))) {
                        uint8_t usage = key_to_hid[ie.code];
                        if (usage) {
                            if (pressed) kbd_press(&kbd, usage);
                            else         kbd_release(&kbd, usage);
                            kbd_dirty = true;
                        }
                    }
                    /* Mouse buttons live in EV_KEY too. */
                    uint8_t bbit = btn_to_bit(ie.code);
                    if (bbit) {
                        if (pressed) mouse.buttons |=  bbit;
                        else         mouse.buttons &= ~bbit;
                        mouse_flush(hidg_mouse, &mouse);
                    }
                } else if (ie.type == EV_REL) {
                    if (ie.code == REL_X)            mouse.pending_dx    += ie.value;
                    else if (ie.code == REL_Y)       mouse.pending_dy    += ie.value;
                    else if (ie.code == REL_WHEEL)   mouse.pending_wheel += ie.value;
                } else if (ie.type == EV_SYN && ie.code == SYN_REPORT) {
                    if (mouse.pending_dx || mouse.pending_dy || mouse.pending_wheel)
                        mouse_flush(hidg_mouse, &mouse);
                }
            }
            if (kbd_dirty) kbd_write_report(hidg_kbd, &kbd);
            if (hotkey_should_fire(mono_ns()))
                hotkey_fire_switch_to_emulation();

            if (r < 0 && (errno == ENODEV || errno == ENOENT)) {
                fprintf(stderr, "kbm-passthrough: input fd %d removed (%s)\n",
                        fd, strerror(errno));
                epoll_ctl(ep, EPOLL_CTL_DEL, fd, NULL);
                ioctl(fd, EVIOCGRAB, 0);
                close(fd);
                input_slots_remove_fd(fd);
            }
        }
    }

    fprintf(stderr, "kbm-passthrough: shutting down\n");
    /* Send a final all-release before tearing down so the LAST report Windows
     * sees from this gadget instance is unambiguously "no keys held". If we
     * close mid-press (SIGTERM during hotkey-triggered mode flip), this
     * prevents a stuck modifier from being inherited by the directly-attached
     * keyboard via Windows' merged-modifier-across-HID-keyboards behavior. */
    kbd_send_release_all(hidg_kbd);
    mouse_send_release_all(hidg_mouse);
    for (int i = 0; i < MAX_INPUTS; i++) {
        if (input_slots[i].fd < 0) continue;
        ioctl(input_slots[i].fd, EVIOCGRAB, 0);
        close(input_slots[i].fd);
    }
    if (inotify_fd >= 0) close(inotify_fd);
    close(hidg_kbd);
    close(hidg_mouse);
    close(ep);
    return 0;
}
