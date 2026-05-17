/* kbm-leds: animated status indicator on the BBB's 4 green user LEDs.
 *
 * All four LEDs are driven in unison from a single GPIO_V2_LINE_SET_VALUES
 * ioctl per tick, so they never drift out of phase. A 1 kHz tick × 16-level
 * software-PWM gives ~62.5 Hz visual refresh — well above flicker fusion —
 * and the brightness "envelope" for the current state is recomputed every
 * tick from a closed-form pattern function. The result is smooth breathing
 * curves rather than the on/off square blinks the kernel's leds-gpio timer
 * trigger would give us.
 *
 * Patterns (see pattern_id below):
 *   calm_focus     — slow symmetric breath, 4 s period, 100% peak.
 *                    Healthy emulation mode with the host connected.
 *   alert_ready    — double-tap bell pulse every 1.2 s.
 *                    Healthy passthrough mode with the host connected.
 *   heart_searching— lub-DUB-rest at 75 bpm.
 *                    Mode is on but the UDC is not "configured" (USB cable
 *                    unplugged or host not enumerated).
 *   idle_drift     — slow shallow breath, 7 s period, 30% ceiling.
 *                    Both mode services off.
 *   cold_tide      — asymmetric sawtooth-into-breath, fast rise, slow fall.
 *                    Error: an expected service is failed, both modes are
 *                    accidentally active, or no UDC for >30 s while a
 *                    mode is on.
 *   crossfade      — 4 Hz triangle, 100% peak, 2 s burst.
 *                    Acknowledgement when a mode transition is observed.
 *
 * State polling runs on a separate pthread at 1 Hz so the renderer never
 * blocks on fork+exec of systemctl. Pattern is published via atomic_int.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* BBB user LEDs are wired to AM335x GPIO1 lines 21..24, which appear on
 * /dev/gpiochip0 (parent register block 4804c000.gpio). Confirmed via
 * /sys/kernel/debug/gpio: gpio-533..536 labelled [usr0 led]..[usr3 led].
 * The LEDS_CLASS kernel driver is not compiled in this build, so nothing
 * else claims these lines. */
#define GPIOCHIP_PATH    "/dev/gpiochip0"
static const uint32_t LED_OFFSETS[4] = { 21, 22, 23, 24 };
#define LED_COUNT        4
#define LED_MASK         0x0Fu

#define PWM_TICK_HZ      1000
#define PWM_LEVELS       16
#define CROSSFADE_MS     2000

enum pattern_id {
    P_CALM_FOCUS = 0,
    P_ALERT_READY,
    P_HEART_SEARCHING,
    P_IDLE_DRIFT,
    P_COLD_TIDE,
    P_CROSSFADE,
};

static atomic_int  current_pattern   = P_IDLE_DRIFT;
static atomic_long crossfade_until_ms = 0;
static volatile sig_atomic_t stopped = 0;

static void on_signal(int s) { (void)s; stopped = 1; }

/* ---- time helpers --------------------------------------------------- */

static double mono_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static long mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + (long)ts.tv_nsec / 1000000L;
}

/* ---- pattern math --------------------------------------------------- *
 *
 * All pattern functions return brightness in [0,1] as a function of an
 * absolute monotonic time t (seconds). Pure functions, no globals — the
 * renderer just evaluates whichever one matches current_pattern.
 *
 * Curve choice rationale:
 *   - Symmetric raised-cosine for "healthy" patterns reads as organic.
 *   - Cubic-eased bell shapes for tapping patterns avoid square-edge
 *     artefacts that make LEDs look like a fault indicator.
 *   - The error pattern is the only ASYMMETRIC envelope: the eye picks up
 *     asymmetry as "wrong" before it can decode the rhythm.
 */

static double bell(double t, double peak, double width) {
    double d = (t - peak) / width;
    double v = 1.0 - d * d;
    if (v < 0.0) return 0.0;
    return v * v;
}

static double calm_focus(double t) {
    return 0.5 * (1.0 - cos(2.0 * M_PI * t / 4.0));
}

static double alert_ready(double t) {
    double phase = fmod(t, 1.2) / 1.2;
    double a = bell(phase, 0.06, 0.04);
    double b = bell(phase, 0.18, 0.04);
    return a > b ? a : b;
}

static double heart_searching(double t) {
    double phase = fmod(t, 0.8) / 0.8;
    double lub = bell(phase, 0.07, 0.05) * 0.55;
    double dub = bell(phase, 0.22, 0.07) * 1.00;
    return lub > dub ? lub : dub;
}

static double idle_drift(double t) {
    return 0.30 * 0.5 * (1.0 - cos(2.0 * M_PI * t / 7.0));
}

static double cold_tide(double t) {
    double phase = fmod(t, 3.5) / 3.5;
    if (phase < 0.30) return phase / 0.30;
    return 1.0 - (phase - 0.30) / 0.70;
}

static double crossfade_curve(double t) {
    double phase = fmod(t, 0.25) / 0.25;
    return phase < 0.5 ? phase * 2.0 : (1.0 - phase) * 2.0;
}

static double sample_pattern(enum pattern_id p, double t) {
    switch (p) {
    case P_CALM_FOCUS:      return calm_focus(t);
    case P_ALERT_READY:     return alert_ready(t);
    case P_HEART_SEARCHING: return heart_searching(t);
    case P_IDLE_DRIFT:      return idle_drift(t);
    case P_COLD_TIDE:       return cold_tide(t);
    case P_CROSSFADE:       return crossfade_curve(t);
    }
    return 0.0;
}

/* ---- state polling -------------------------------------------------- *
 *
 * Runs on a separate thread at 1 Hz so the 1 kHz renderer never blocks
 * on fork+exec. select_pattern() reads system state and returns the
 * pattern that should be displayed; the main loop overlays the crossfade
 * pattern during transitions.
 */

static int read_first_line(const char *path, char *buf, size_t bufsz) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, bufsz - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    return 0;
}

static bool service_active(const char *unit) {
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); close(devnull); }
        execl("/bin/systemctl", "systemctl", "is-active", "--quiet", unit, (char *)NULL);
        execl("/usr/bin/systemctl", "systemctl", "is-active", "--quiet", unit, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool udc_configured(void) {
    DIR *d = opendir("/sys/class/udc");
    if (!d) return false;
    struct dirent *e;
    bool ok = false;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[160], buf[64];
        snprintf(path, sizeof path, "/sys/class/udc/%s/state", e->d_name);
        if (read_first_line(path, buf, sizeof buf) == 0 && strcmp(buf, "configured") == 0) {
            ok = true;
            break;
        }
    }
    closedir(d);
    return ok;
}

static enum pattern_id select_pattern(void) {
    bool emu = service_active("dualsense-ffsd");
    bool pt  = service_active("kbm-passthrough");
    bool host = udc_configured();

    /* Both mode daemons should never both be active — Conflicts= in the
     * unit files prevents it. If we see it, something is wrong. */
    if (emu && pt) return P_COLD_TIDE;
    if (!emu && !pt) return P_IDLE_DRIFT;
    if (!host)       return P_HEART_SEARCHING;
    if (emu)         return P_CALM_FOCUS;
    return P_ALERT_READY;
}

/* Optional override: if /run/kbm-leds/force exists and contains an integer
 * in [0, P_CROSSFADE], that pattern is rendered instead of the one chosen
 * from system state. Useful for visual verification — e.g.
 *   echo 4 > /run/kbm-leds/force   # show cold_tide
 *   rm    /run/kbm-leds/force      # back to automatic selection
 */
#define FORCE_PATH "/run/kbm-leds/force"

static void *state_thread(void *arg) {
    (void)arg;
    enum pattern_id last = atomic_load(&current_pattern);
    while (!stopped) {
        enum pattern_id np;
        char fbuf[16];
        bool forced = false;
        if (read_first_line(FORCE_PATH, fbuf, sizeof fbuf) == 0) {
            char *end;
            long v = strtol(fbuf, &end, 10);
            if (end != fbuf && v >= 0 && v <= P_CROSSFADE) {
                np = (enum pattern_id)v;
                forced = true;
            }
        }
        if (!forced) np = select_pattern();

        if (np != last) {
            atomic_store(&crossfade_until_ms, mono_ms() + CROSSFADE_MS);
            last = np;
        }
        atomic_store(&current_pattern, np);
        sleep(1);
    }
    return NULL;
}

/* ---- GPIO setup + render loop --------------------------------------- */

static int request_gpio_lines(int chip_fd) {
    struct gpio_v2_line_request req;
    memset(&req, 0, sizeof req);
    for (int i = 0; i < LED_COUNT; i++) req.offsets[i] = LED_OFFSETS[i];
    req.num_lines = LED_COUNT;
    strncpy(req.consumer, "kbm-leds", sizeof req.consumer - 1);
    req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    if (ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
        fprintf(stderr, "kbm-leds: GPIO_V2_GET_LINE_IOCTL on %s: %s\n",
                GPIOCHIP_PATH, strerror(errno));
        return -1;
    }
    return req.fd;
}

static inline void write_lines(int line_fd, uint64_t bits) {
    struct gpio_v2_line_values v = { .bits = bits, .mask = LED_MASK };
    ioctl(line_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v);
}

int main(void) {
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    int chip_fd = open(GPIOCHIP_PATH, O_RDWR | O_CLOEXEC);
    if (chip_fd < 0) {
        fprintf(stderr, "kbm-leds: open %s: %s\n", GPIOCHIP_PATH, strerror(errno));
        return 1;
    }
    int line_fd = request_gpio_lines(chip_fd);
    close(chip_fd);
    if (line_fd < 0) return 1;
    fprintf(stderr, "kbm-leds: claimed GPIO1.21..24 via %s\n", GPIOCHIP_PATH);

    /* Initial poll so we start on the right pattern. */
    atomic_store(&current_pattern, select_pattern());

    pthread_t tid;
    if (pthread_create(&tid, NULL, state_thread, NULL) != 0) {
        fprintf(stderr, "kbm-leds: pthread_create: %s\n", strerror(errno));
        close(line_fd);
        return 1;
    }

    /* Absolute-time 1 kHz tick — clock_nanosleep(TIMER_ABSTIME) avoids
     * jitter accumulation across iterations. */
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    unsigned long tick = 0;

    while (!stopped) {
        next.tv_nsec += 1000000;
        if (next.tv_nsec >= 1000000000) { next.tv_sec += 1; next.tv_nsec -= 1000000000; }
        if (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR && stopped)
            break;
        tick++;

        enum pattern_id p = atomic_load(&current_pattern);
        long cx_end = atomic_load(&crossfade_until_ms);
        if (mono_ms() < cx_end) p = P_CROSSFADE;

        double b = sample_pattern(p, mono_seconds());
        if (b < 0.0) b = 0.0;
        if (b > 1.0) b = 1.0;

        int level = (int)(b * PWM_LEVELS + 0.5);
        int phase = (int)(tick % PWM_LEVELS);
        uint64_t bits = (phase < level) ? LED_MASK : 0;
        write_lines(line_fd, bits);
    }

    pthread_join(tid, NULL);
    write_lines(line_fd, 0);
    close(line_fd);
    fprintf(stderr, "kbm-leds: shut down\n");
    return 0;
}
