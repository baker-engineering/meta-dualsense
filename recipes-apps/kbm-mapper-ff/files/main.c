/*
 * dualsense-ffsd — userspace DualSense HID gadget on FunctionFS.
 *
 * Replaces the previous configfs-HID + kbm-mapper split with one process that:
 *   - configures USB descriptors via /dev/ffs0/ep0
 *   - binds the gadget to musb-hdrc.0 UDC
 *   - serves HID class control transfers (GET_REPORT(Feature), GET_DESCRIPTOR
 *     (Report)) from a hand-coded DualSense identity, so DS4Windows and the
 *     hid-playstation kernel driver accept it as a real controller
 *   - reads USB keyboard + mouse over evdev, maps them to a 64-byte DualSense
 *     USB input report stream at 1 kHz on /dev/ffs0/ep1
 *   - consumes host-to-device output reports (rumble, lightbar, adaptive
 *     trigger commands) on /dev/ffs0/ep2 and discards them
 *   - publishes a 64-byte state snapshot to /run/kbm-mapper/state.bin at
 *     100 Hz for the kbm-web live preview
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <linux/input.h>

#include "config.h"
#include "mapper.h"
#include "ffs_descriptors.h"
#include "feature_reports.h"

#define FFS_PATH    "/dev/ffs0"
#define UDC_PATH    "/sys/kernel/config/usb_gadget/dualsense/UDC"
#define MAX_INPUTS  16
#define SNAP_PATH   "/run/kbm-mapper/state.bin"

static volatile sig_atomic_t stopped = 0;
static void on_signal(int sig) { (void)sig; stopped = 1; }

static int64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

static int sysfs_write(const char *path, const char *value) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t n = write(fd, value, strlen(value));
    int saved = errno;
    close(fd);
    if (n < 0) { errno = saved; return -1; }
    return 0;
}

static int test_bit(const unsigned char *bits, int nr) {
    return (bits[nr / 8] >> (nr % 8)) & 1;
}

static int classify_and_open_input(const char *path) {
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
    fprintf(stderr, "dualsense-ffsd: opened %s (%s) as %s%s\n",
            path, name, looks_kbd ? "keyboard" : "", looks_mouse ? "mouse" : "");

    if (ioctl(fd, EVIOCGRAB, 1) < 0)
        fprintf(stderr, "dualsense-ffsd: EVIOCGRAB %s failed: %s\n", path, strerror(errno));

    int clk = CLOCK_MONOTONIC;
    if (ioctl(fd, EVIOCSCLOCKID, &clk) < 0)
        fprintf(stderr, "dualsense-ffsd: EVIOCSCLOCKID %s failed: %s\n", path, strerror(errno));

    return fd;
}

/* Tracked input slots. fd == -1 means unused. path[] is the /dev/input/eventN
 * we opened, used to dedupe inotify events for an already-tracked device. */
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

static int scan_inputs(void) {
    int n = 0;
    DIR *d = opendir("/dev/input");
    if (!d) { perror("/dev/input"); return 0; }
    struct dirent *de;
    while ((de = readdir(d)) && n < MAX_INPUTS) {
        if (strncmp(de->d_name, "event", 5) != 0) continue;
        char path[sizeof("/dev/input/") + sizeof(de->d_name)];
        snprintf(path, sizeof path, "/dev/input/%s", de->d_name);
        int fd = classify_and_open_input(path);
        if (fd >= 0) {
            if (input_slots_add(fd, path) < 0) { close(fd); break; }
            n++;
        }
    }
    closedir(d);
    return n;
}

/* Open inotify. Two watches:
 *  - /dev/input         (IN_CREATE | IN_ATTRIB): hot-plug keyboards/mice.
 *  - /etc               (IN_CLOSE_WRITE | IN_MOVED_TO): config hot-reload
 *                       when kbm-mapper.conf is rewritten. kbm-web saves via
 *                       write-temp+rename so MOVED_TO is what fires; direct
 *                       editor saves trigger CLOSE_WRITE. */
static int open_inotify(void) {
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) { perror("inotify_init1"); return -1; }
    if (inotify_add_watch(fd, "/dev/input", IN_CREATE | IN_ATTRIB) < 0) {
        perror("inotify_add_watch /dev/input");
        close(fd);
        return -1;
    }
    if (inotify_add_watch(fd, "/etc", IN_CLOSE_WRITE | IN_MOVED_TO) < 0) {
        perror("inotify_add_watch /etc");
        /* non-fatal: hot-reload disabled but daemon stays functional */
    }
    return fd;
}

static int bind_udc(void) {
    DIR *d = opendir("/sys/class/udc");
    if (!d) return -1;
    char udc[64] = {0};
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        strncpy(udc, de->d_name, sizeof(udc) - 1);
        break;
    }
    closedir(d);
    if (!udc[0]) { fprintf(stderr, "no UDC available\n"); return -1; }
    fprintf(stderr, "dualsense-ffsd: binding to UDC %s\n", udc);
    return sysfs_write(UDC_PATH, udc);
}

/* === Capture socket ===
 *
 * Userland tools (kbm-web) connect to /run/kbm-mapper/capture.sock to receive
 * raw EV_KEY events as JSON lines. Used by the web UI's "Press a key" capture
 * flow: in emulation mode the daemon has EVIOCGRAB'd the input devices, so
 * the only path to their events is through here.
 *
 * Wire format: one JSON object per event, newline-terminated:
 *   {"type":1,"code":30,"value":1,"source":"keyboard"}
 * Clients close the socket when done. We support a small fixed number of
 * concurrent subscribers; older ones get dropped if the cap is hit. */

#define CAPTURE_SOCK_PATH "/run/kbm-mapper/capture.sock"
#define CAPTURE_MAX_CLIENTS 4

static int capture_listen_fd = -1;
static int capture_clients[CAPTURE_MAX_CLIENTS] = { -1, -1, -1, -1 };

static int capture_listen_open(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("capture socket"); return -1; }
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", CAPTURE_SOCK_PATH);
    unlink(CAPTURE_SOCK_PATH);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("capture bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 4) < 0) {
        perror("capture listen");
        close(fd);
        return -1;
    }
    chmod(CAPTURE_SOCK_PATH, 0660);
    return fd;
}

static void capture_accept(void) {
    int cfd = accept4(capture_listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (cfd < 0) return;
    for (int i = 0; i < CAPTURE_MAX_CLIENTS; i++) {
        if (capture_clients[i] < 0) { capture_clients[i] = cfd; return; }
    }
    /* All slots full — evict slot 0 (oldest) and take its place. */
    close(capture_clients[0]);
    capture_clients[0] = cfd;
}

static void capture_publish(const struct input_event *e, bool from_mouse) {
    if (e->type != EV_KEY) return;
    char line[128];
    int n = snprintf(line, sizeof line,
                     "{\"type\":%u,\"code\":%u,\"value\":%d,\"source\":\"%s\"}\n",
                     e->type, e->code, e->value, from_mouse ? "mouse" : "keyboard");
    if (n <= 0) return;
    for (int i = 0; i < CAPTURE_MAX_CLIENTS; i++) {
        if (capture_clients[i] < 0) continue;
        ssize_t w = write(capture_clients[i], line, n);
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            close(capture_clients[i]);
            capture_clients[i] = -1;
        }
    }
}

/* Cheap is-mouse classifier — checked once per fd by inspecting evdev capability
 * bits at open time would be cleaner, but we already pass each fd by file
 * descriptor so we just check the device name string. */
static bool fd_is_mouse(int fd) {
    char name[256] = "";
    ioctl(fd, EVIOCGNAME(sizeof name), name);
    /* Anything reporting REL_X is a mouse for our purposes. */
    unsigned char rel_bits[(REL_MAX + 7) / 8] = {0};
    ioctl(fd, EVIOCGBIT(EV_REL, sizeof rel_bits), rel_bits);
    return (rel_bits[REL_X / 8] >> (REL_X % 8)) & 1;
}

/* === Mode-switch hotkey ===
 *
 * Configurable chord (cfg->hotkey_codes / cfg->hotkey_n). All keys in the
 * chord must be held simultaneously for >=1 s for the toggle to fire.
 * Default LEFTCTRL+ESC; users override per-profile via kbm-web.
 *
 * The chord is detected in this daemon because in emulation mode it
 * EVIOCGRAB's the keyboard and the events never reach Windows. The
 * kbm-passthrough daemon mirrors the same logic with the opposite target. */
struct hotkey_state {
    bool    pressed[8];      /* matches cfg->hotkey_codes[i] */
    int64_t all_since_ns;
    bool    fired;
};
static struct hotkey_state hotkey = {0};

static void hotkey_update(const struct config *cfg,
                          const struct input_event *e, int64_t now_ns) {
    if (e->type != EV_KEY) return;
    if (cfg->hotkey_n == 0) return;
    bool relevant = false;
    for (int i = 0; i < cfg->hotkey_n; i++) {
        if (e->code == cfg->hotkey_codes[i]) {
            hotkey.pressed[i] = (e->value != 0);
            relevant = true;
            break;
        }
    }
    if (!relevant) return;
    bool all = true;
    for (int i = 0; i < cfg->hotkey_n; i++)
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

/* Reset hotkey pressed-state when the config reloads, otherwise a key that
 * was held when the user changed the chord could leave stale pressed bits. */
static void hotkey_reset(void) {
    memset(&hotkey, 0, sizeof hotkey);
}

static void hotkey_fire_switch_to_passthrough(void) {
    fprintf(stderr, "dualsense-ffsd: Ctrl+Esc held 1 s -> switching to passthrough\n");
    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/systemctl", "systemctl", "start", "kbm-passthrough", (char *)NULL);
        execl("/usr/bin/systemctl", "systemctl", "start", "kbm-passthrough", (char *)NULL);
        _exit(127);
    }
    /* Parent continues; systemd Conflicts= will SIGTERM us shortly. */
}

/* === FunctionFS ep0: HID control-transfer handler === */

/* Respond to an IN-direction setup: write the response payload to ep0. */
static void respond_in(int ep0, const void *data, size_t len, size_t requested) {
    if (len > requested) len = requested;
    ssize_t w = write(ep0, data, len);
    if (w < 0)
        fprintf(stderr, "dualsense-ffsd: ep0 write %zu bytes: %s\n", len, strerror(errno));
}

/* Acknowledge an OUT-direction setup with zero-length status. */
static void ack_out(int ep0) {
    char zero;
    /* Reading zero bytes acknowledges the OUT setup data phase. */
    (void)read(ep0, &zero, 0);
}

static void handle_setup(int ep0, const struct usb_ctrlrequest *s) {
    uint8_t bmRT = s->bRequestType;
    uint8_t bReq = s->bRequest;
    uint16_t wValue = le16toh(s->wValue);
    uint16_t wIndex = le16toh(s->wIndex);
    uint16_t wLength = le16toh(s->wLength);
    int direction_in = (bmRT & USB_DIR_IN) != 0;
    int type = bmRT & USB_TYPE_MASK;

    /* Trace every setup so we can correlate FAILED_START captures with
     * what our handler actually saw. Removable once FAILED_START is fixed. */
    fprintf(stderr, "ffs setup: bmRT=0x%02x bReq=0x%02x wValue=0x%04x wIndex=0x%04x wLength=0x%04x\n",
            bmRT, bReq, wValue, wIndex, wLength);

    if (type == USB_TYPE_STANDARD) {
        /* GET_DESCRIPTOR(HID Report) — host wants the 273-byte report desc. */
        if (direction_in && bReq == USB_REQ_GET_DESCRIPTOR) {
            uint8_t desc_type = (wValue >> 8) & 0xFF;
            if (desc_type == HID_DT_REPORT) {
                respond_in(ep0, dualsense_report_descriptor,
                           sizeof(dualsense_report_descriptor), wLength);
                return;
            }
            if (desc_type == HID_DT_HID) {
                /* Host wants the HID class descriptor (9 bytes). */
                respond_in(ep0, &descriptors.fs.hid, sizeof(descriptors.fs.hid), wLength);
                return;
            }
        }
        /* Other standard requests are handled by the kernel; we just NAK. */
        goto stall;
    }

    if (type == USB_TYPE_CLASS) {
        if (direction_in && bReq == 0x01 /* HID GET_REPORT */) {
            uint8_t report_type = (wValue >> 8) & 0xFF;  /* 1=Input, 2=Output, 3=Feature */
            uint8_t report_id   = wValue & 0xFF;
            if (report_type == 0x03) {
                const struct feature_report *fr = find_feature_report(report_id);
                if (fr) {
                    respond_in(ep0, fr->data, fr->size, wLength);
                    return;
                }
                fprintf(stderr, "dualsense-ffsd: feature 0x%02x not stubbed\n", report_id);
                goto stall;
            }
            /* GET_REPORT(Input) — return a centered idle report so polling
             * doesn't break before the IN endpoint is active. */
            uint8_t idle[64] = {0};
            idle[0] = 0x01;
            idle[1] = idle[2] = idle[3] = idle[4] = 0x80;
            idle[8] = 0x08;  /* dpad null */
            respond_in(ep0, idle, sizeof(idle), wLength);
            return;
        }
        if (!direction_in && bReq == 0x09 /* HID SET_REPORT */) {
            /* Read and discard the data phase. */
            uint8_t buf[256];
            ssize_t r = read(ep0, buf, sizeof(buf));
            (void)r;
            return;
        }
        if (bReq == 0x0A /* HID SET_IDLE */) { ack_out(ep0); return; }
        if (direction_in && bReq == 0x02 /* HID GET_IDLE */) {
            uint8_t v = 0;
            respond_in(ep0, &v, 1, wLength);
            return;
        }
        if (bReq == 0x0B /* HID SET_PROTOCOL */) { ack_out(ep0); return; }
        if (direction_in && bReq == 0x03 /* HID GET_PROTOCOL */) {
            uint8_t v = 1;
            respond_in(ep0, &v, 1, wLength);
            return;
        }
    }

stall:
    /* Stall the endpoint to NAK the request. */
    if (direction_in)
        (void)write(ep0, NULL, 0);
    else
        (void)read(ep0, NULL, 0);
    (void)wIndex;
}

static void handle_ffs_event(int ep0, const struct usb_functionfs_event *ev) {
    switch (ev->type) {
    case FUNCTIONFS_BIND:    fprintf(stderr, "ffs: BIND\n"); break;
    case FUNCTIONFS_UNBIND:  fprintf(stderr, "ffs: UNBIND\n"); break;
    case FUNCTIONFS_ENABLE:  fprintf(stderr, "ffs: ENABLE (host configured us)\n"); break;
    case FUNCTIONFS_DISABLE: fprintf(stderr, "ffs: DISABLE\n"); break;
    case FUNCTIONFS_SETUP:   handle_setup(ep0, &ev->u.setup); break;
    case FUNCTIONFS_SUSPEND: fprintf(stderr, "ffs: SUSPEND\n"); break;
    case FUNCTIONFS_RESUME:  fprintf(stderr, "ffs: RESUME\n"); break;
    default:                 fprintf(stderr, "ffs: unknown event %u\n", ev->type); break;
    }
}

/* === ep_in writer thread ===
 *
 * FFS's per-endpoint write() blocks in wait_for_completion_interruptible
 * until the USB host actually reads the IN packet, REGARDLESS of O_NONBLOCK
 * on the fd. (O_NONBLOCK only short-circuits the "wait for endpoint enable"
 * check at the top of ffs_epfile_io; the actual transfer always blocks.)
 *
 * When the Windows host puts the device into CM_PROB_FAILED_START and stops
 * polling ep1, our 1 kHz tick would block in write(ep_in, ...) for 5 s+
 * waiting for the request to complete. The main loop would then miss SETUP
 * events on ep0 and Windows would never get a chance to recover via
 * GET_DESCRIPTOR(Report). Dedicating a writer thread to ep_in fully decouples
 * the main loop from this blocking behavior. */
struct ep_writer {
    int             fd;
    pthread_t       thread;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    uint8_t         report[DS_REPORT_LEN];
    bool            has_pending;
    bool            stop;
};

static void *ep_writer_loop(void *arg) {
    struct ep_writer *w = arg;
    uint8_t local[DS_REPORT_LEN];
    for (;;) {
        pthread_mutex_lock(&w->mu);
        while (!w->has_pending && !w->stop)
            pthread_cond_wait(&w->cv, &w->mu);
        if (w->stop) { pthread_mutex_unlock(&w->mu); break; }
        memcpy(local, w->report, sizeof local);
        w->has_pending = false;
        pthread_mutex_unlock(&w->mu);
        /* write blocks until the host reads the packet (or transfer aborts).
         * Either outcome is fine — we just loop and pick up the latest
         * report on the next signal. */
        ssize_t r = write(w->fd, local, sizeof local);
        if (r < 0 && errno != EAGAIN && errno != ESHUTDOWN && errno != EPIPE)
            fprintf(stderr, "ep_in write: %s\n", strerror(errno));
    }
    return NULL;
}

static void ep_writer_start(struct ep_writer *w, int fd) {
    memset(w, 0, sizeof(*w));
    w->fd = fd;
    pthread_mutex_init(&w->mu, NULL);
    pthread_cond_init(&w->cv, NULL);
    pthread_create(&w->thread, NULL, ep_writer_loop, w);
}

static void ep_writer_submit(struct ep_writer *w, const uint8_t *report) {
    pthread_mutex_lock(&w->mu);
    memcpy(w->report, report, DS_REPORT_LEN);
    w->has_pending = true;
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->mu);
}

static void ep_writer_stop(struct ep_writer *w) {
    pthread_mutex_lock(&w->mu);
    w->stop = true;
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->mu);
    pthread_join(w->thread, NULL);
    pthread_cond_destroy(&w->cv);
    pthread_mutex_destroy(&w->mu);
}

/* === Main === */

int main(int argc, char **argv) {
    const char *cfg_path = (argc > 1) ? argv[1] : "/etc/kbm-mapper.conf";
    struct config cfg;
    config_load(&cfg, cfg_path);

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    if (mlockall(MCL_CURRENT | MCL_FUTURE) < 0)
        fprintf(stderr, "mlockall: %s\n", strerror(errno));
    struct sched_param sp = { .sched_priority = 80 };
    if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0)
        fprintf(stderr, "SCHED_FIFO: %s\n", strerror(errno));
    cpu_set_t cpus; CPU_ZERO(&cpus); CPU_SET(0, &cpus);
    sched_setaffinity(0, sizeof cpus, &cpus);

    /* 1. ep0: write descriptors + strings. */
    int ep0 = open(FFS_PATH "/ep0", O_RDWR);
    if (ep0 < 0) { perror(FFS_PATH "/ep0"); return 1; }
    if (write(ep0, &descriptors, sizeof descriptors) != (ssize_t)sizeof descriptors) {
        perror("write descriptors");
        return 1;
    }
    if (write(ep0, &strings_descriptor, sizeof strings_descriptor) != (ssize_t)sizeof strings_descriptor) {
        perror("write strings");
        return 1;
    }
    fprintf(stderr, "dualsense-ffsd: FFS descriptors loaded\n");

    /* 2. Bind UDC so the gadget shows up on the bus. */
    if (bind_udc() < 0) {
        fprintf(stderr, "dualsense-ffsd: bind_udc failed: %s\n", strerror(errno));
        return 1;
    }

    /* 3. Open IN/OUT endpoints (they become readable/writable only after the
     * kernel has accepted the descriptors). */
    int ep_in = -1, ep_out = -1;
    for (int i = 0; i < 100; i++) {
        ep_in = open(FFS_PATH "/ep1", O_WRONLY | O_NONBLOCK);
        ep_out = open(FFS_PATH "/ep2", O_RDONLY | O_NONBLOCK);
        if (ep_in >= 0 && ep_out >= 0) break;
        if (ep_in >= 0) { close(ep_in); ep_in = -1; }
        if (ep_out >= 0) { close(ep_out); ep_out = -1; }
        usleep(50000);
    }
    if (ep_in < 0 || ep_out < 0) { perror("open ep1/ep2"); return 1; }
    fprintf(stderr, "dualsense-ffsd: ep1+ep2 open\n");

    /* Hand ep_in off to a dedicated writer thread so the main loop never
     * blocks in write(ep_in, ...) when the host stops polling. */
    struct ep_writer writer;
    ep_writer_start(&writer, ep_in);

    /* 4. evdev devices + inotify watcher for hot-plug. */
    input_slots_init();
    int n_inputs = scan_inputs();
    if (n_inputs == 0)
        fprintf(stderr, "warning: no keyboard or mouse under /dev/input\n");
    int inotify_fd = open_inotify();

    /* 5. timerfd at 1 kHz. */
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    struct itimerspec its = {
        .it_value    = { .tv_nsec = 1000000 },
        .it_interval = { .tv_nsec = 1000000 },
    };
    timerfd_settime(tfd, 0, &its, NULL);

    /* 6. /run snapshot file for kbm-web. */
    int snap_fd = open(SNAP_PATH, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (snap_fd >= 0) ftruncate(snap_fd, 64);
    int snap_counter = 0;

    /* 7. epoll: tfd, ep0, ep_out, inotify, capture listen, all input fds. */
    capture_listen_fd = capture_listen_open();
    int ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev = { .events = EPOLLIN };
    ev.data.fd = tfd;    epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &ev);
    ev.data.fd = ep0;    epoll_ctl(ep, EPOLL_CTL_ADD, ep0, &ev);
    ev.data.fd = ep_out; epoll_ctl(ep, EPOLL_CTL_ADD, ep_out, &ev);
    if (inotify_fd >= 0) {
        ev.data.fd = inotify_fd;
        epoll_ctl(ep, EPOLL_CTL_ADD, inotify_fd, &ev);
    }
    if (capture_listen_fd >= 0) {
        ev.data.fd = capture_listen_fd;
        epoll_ctl(ep, EPOLL_CTL_ADD, capture_listen_fd, &ev);
    }
    for (int i = 0; i < MAX_INPUTS; i++) {
        if (input_slots[i].fd < 0) continue;
        ev.data.fd = input_slots[i].fd;
        epoll_ctl(ep, EPOLL_CTL_ADD, input_slots[i].fd, &ev);
    }

    struct mapper m;
    mapper_init(&m, &cfg);
    uint8_t report[DS_REPORT_LEN];
    uint8_t out_buf[64];

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
            if (fd == tfd) {
                uint64_t exp;
                if (read(tfd, &exp, sizeof exp) != sizeof exp) continue;
                mapper_build_report(&m, report, sizeof report, mono_ns());
                ep_writer_submit(&writer, report);
                if (snap_fd >= 0 && ++snap_counter >= 10) {
                    snap_counter = 0;
                    pwrite(snap_fd, report, sizeof report, 0);
                }
            } else if (fd == ep0) {
                struct usb_functionfs_event evs[4];
                ssize_t r = read(ep0, evs, sizeof(evs));
                if (r > 0) {
                    int nev = r / sizeof(struct usb_functionfs_event);
                    for (int k = 0; k < nev; k++)
                        handle_ffs_event(ep0, &evs[k]);
                }
            } else if (fd == ep_out) {
                /* Host-to-device output reports — rumble/lightbar/etc. Discard. */
                while (read(ep_out, out_buf, sizeof out_buf) > 0) {}
            } else if (fd == inotify_fd) {
                /* Multiple watches share this fd. Filter by name. */
                char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
                ssize_t r = read(inotify_fd, buf, sizeof buf);
                if (r <= 0) continue;
                for (char *p = buf; p < buf + r; ) {
                    struct inotify_event *iev = (struct inotify_event *)p;
                    if (iev->len > 0 && strncmp(iev->name, "event", 5) == 0) {
                        /* New evdev node in /dev/input — open + classify. */
                        char path[64];
                        snprintf(path, sizeof path, "/dev/input/%s", iev->name);
                        if (input_slots_find_path(path) < 0) {
                            int newfd = classify_and_open_input(path);
                            if (newfd >= 0) {
                                if (input_slots_add(newfd, path) < 0) {
                                    fprintf(stderr, "dualsense-ffsd: input table full, ignoring %s\n", path);
                                    close(newfd);
                                } else {
                                    struct epoll_event nev = { .events = EPOLLIN };
                                    nev.data.fd = newfd;
                                    epoll_ctl(ep, EPOLL_CTL_ADD, newfd, &nev);
                                }
                            }
                        }
                    } else if (iev->len > 0 && strcmp(iev->name, "kbm-mapper.conf") == 0) {
                        /* Config file rewritten — re-parse in place. The mapper
                         * holds a pointer to this struct; refresh happens on
                         * its next 1 ms tick. No locking needed because the
                         * mapper and inotify handler both run on this thread. */
                        fprintf(stderr, "dualsense-ffsd: reloading %s\n", cfg_path);
                        config_load(&cfg, cfg_path);
                        hotkey_reset();
                    }
                    p += sizeof(struct inotify_event) + iev->len;
                }
            } else if (fd == capture_listen_fd) {
                capture_accept();
            } else {
                bool is_mouse = fd_is_mouse(fd);
                struct input_event ie;
                ssize_t r;
                while ((r = read(fd, &ie, sizeof ie)) == (ssize_t)sizeof ie) {
                    hotkey_update(&cfg, &ie, mono_ns());
                    capture_publish(&ie, is_mouse);
                    mapper_handle_event(&m, &ie);
                }
                if (hotkey_should_fire(mono_ns()))
                    hotkey_fire_switch_to_passthrough();
                if (r < 0 && (errno == ENODEV || errno == ENOENT)) {
                    fprintf(stderr, "dualsense-ffsd: input fd %d removed (%s)\n",
                            fd, strerror(errno));
                    epoll_ctl(ep, EPOLL_CTL_DEL, fd, NULL);
                    ioctl(fd, EVIOCGRAB, 0);
                    close(fd);
                    input_slots_remove_fd(fd);
                }
            }
        }
    }

    fprintf(stderr, "dualsense-ffsd: shutting down\n");
    /* Unbind UDC so the gadget tears down cleanly. Stop the writer thread
     * BEFORE closing ep_in so it doesn't keep submitting into a closed fd. */
    sysfs_write(UDC_PATH, "");
    ep_writer_stop(&writer);
    for (int i = 0; i < MAX_INPUTS; i++) {
        if (input_slots[i].fd < 0) continue;
        ioctl(input_slots[i].fd, EVIOCGRAB, 0);
        close(input_slots[i].fd);
    }
    if (inotify_fd >= 0) close(inotify_fd);
    if (snap_fd >= 0) close(snap_fd);
    for (int i = 0; i < CAPTURE_MAX_CLIENTS; i++)
        if (capture_clients[i] >= 0) close(capture_clients[i]);
    if (capture_listen_fd >= 0) { close(capture_listen_fd); unlink(CAPTURE_SOCK_PATH); }
    close(ep_out); close(ep_in); close(ep0); close(tfd); close(ep);
    return 0;
}
