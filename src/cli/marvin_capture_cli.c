/*
 * Pinnacle Studio 500-USB open driver
 * Copyright (C) 2026 Jonas Cz.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * MarvinCaptureCLI -- the command-line front end of marvin-core (docs/cli.md).
 * A thin shell: the argument parser, the step sequencer and the help text all
 * live in the core (pin_script_*); this file opens the device, runs the script,
 * shows steps, log lines and a status line, and turns Ctrl-C into
 * pin_script_cancel(). It links nothing but marvin-core. All human output goes
 * to stderr (stdout is reserved for the stream); only --help prints to stdout.
 */

#include "pin_api.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#define ISATTY_STDERR() _isatty(_fileno(stderr))
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
static double now_s(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
#else
#include <sys/ioctl.h>
#include <unistd.h>
#define ISATTY_STDERR() isatty(2)
static void sleep_ms(int ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
#endif

/* ---- Ctrl-C ------------------------------------------------------------- */

static volatile sig_atomic_t g_interrupts;   /* number of Ctrl-C presses so far */

static void note_interrupt(void)
{
    if (++g_interrupts >= 2)
        _exit(130);   /* second press: give up on the orderly stop */
}

#if defined(_WIN32)
static BOOL WINAPI on_console_ctrl(DWORD type)
{
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        note_interrupt();
        return TRUE;
    }
    return FALSE;
}
static void install_interrupt_handler(void) { SetConsoleCtrlHandler(on_console_ctrl, TRUE); }
#else
static void on_signal(int sig) { (void)sig; note_interrupt(); }
static void install_interrupt_handler(void)
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
}
#endif

/* ---- output ------------------------------------------------------------- */

static int g_inplace_width;   /* > 0: an in-place status line of this width is on screen */

static int console_width(void)
{
#if defined(_WIN32)
    CONSOLE_SCREEN_BUFFER_INFO ci;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_ERROR_HANDLE), &ci))
        return ci.srWindow.Right - ci.srWindow.Left + 1;
#else
    struct winsize ws;
    if (ioctl(2, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
#endif
    return 80;
}

/* Wipes the in-place status line so the next printed line starts clean. */
static void finish_inplace(void)
{
    if (g_inplace_width > 0) {
        fprintf(stderr, "\r%*s\r", g_inplace_width, "");
        g_inplace_width = 0;
    }
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    va_list ap;
    finish_inplace();
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static void show_status(pin_session_t *s, int inplace)
{
    pin_status_snapshot_t st;
    memset(&st, 0, sizeof(st));
    st.size = sizeof(st);
    if (pin_get_status(s, &st) != PIN_OK)
        return;
    char line[512];
    pin_format_status_line(&st, line, sizeof(line));
    if (!inplace) {
        say("%s", line);
        return;
    }
    int w = console_width() - 1;
    if (w < 20)
        w = 20;
    if (w > (int)sizeof(line) - 1)
        w = (int)sizeof(line) - 1;
    line[w] = '\0';
    int len = (int)strlen(line);
    int pad = g_inplace_width > len ? g_inplace_width : len;
    fprintf(stderr, "\r%-*s", pad, line);
    fflush(stderr);
    g_inplace_width = pad;
}

/* ---- help wrapping ------------------------------------------------------ */

/* Terminal width of stdout in columns, or 0 when it is not a terminal (then nothing is wrapped). */
static int stdout_columns(void)
{
#if defined(_WIN32)
    CONSOLE_SCREEN_BUFFER_INFO ci;
    if (_isatty(_fileno(stdout)) && GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &ci))
        return ci.srWindow.Right - ci.srWindow.Left + 1;
    return 0;
#else
    struct winsize ws;
    if (isatty(1) && ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 0;
#endif
}

/* The help text has one logical line per entry and no line width of its own. Wrap each line at
 * `width` on spaces, continuing under the entry's text: "  --opt   description" hangs under the
 * description, "  - item" under the item text, anything else under its own indent. */
static void print_wrapped(FILE *f, const char *text, int width)
{
    if (width <= 0) {
        fputs(text, f);
        return;
    }
    if (width < 40)
        width = 40;
    width -= 1;   /* never write into the last column: some terminals wrap early there */
    while (*text) {
        const char *eol = strchr(text, '\n');
        size_t len = eol ? (size_t)(eol - text) : strlen(text);
        size_t indent = 0;
        while (indent < len && text[indent] == ' ')
            indent++;
        size_t hang = indent;
        if (indent == 2 && indent < len) {
            if (text[2] == '-' && len > 3 && text[3] == ' ') {
                hang = 4;                              /* bullet */
            } else {
                for (size_t i = 3; i + 1 < len; i++)   /* first gap of 2+ spaces: description column */
                    if (text[i] == ' ' && text[i + 1] == ' ') {
                        size_t j = i;
                        while (j < len && text[j] == ' ')
                            j++;
                        if (j < len)
                            hang = j;
                        break;
                    }
            }
        } else if (indent >= 2 && indent < len && text[indent] == '-' && indent + 1 < len && text[indent + 1] == ' ') {
            hang = indent + 2;
        }
        if ((int)hang > width / 2)
            hang = 0;
        size_t pos = 0;
        int first = 1;
        while (pos < len || first) {
            size_t pad = first ? 0 : hang;
            size_t room = (size_t)width - pad;
            size_t take = len - pos;
            if (take > room) {
                size_t cut = room;
                while (cut > 0 && text[pos + cut] != ' ')
                    cut--;
                /* do not break inside the leading indentation of the first row */
                if (cut <= (first ? indent : 0))
                    cut = room;
                take = cut;
            }
            for (size_t i = 0; i < pad; i++)
                fputc(' ', f);
            fwrite(text + pos, 1, take, f);
            fputc('\n', f);
            pos += take;
            while (pos < len && text[pos] == ' ')
                pos++;
            first = 0;
            if (pos >= len)
                break;
        }
        if (len == 0)
            ;  /* empty line: first loop iteration already wrote the newline */
        text += len + (eol ? 1 : 0);
    }
}

/* ---- device table ------------------------------------------------------- */

static const char *dev_state_name(pin_dev_state_t s)
{
    switch (s) {
    case PIN_DEV_READY: return "ready";
    case PIN_DEV_PREPARING: return "preparing";
    case PIN_DEV_IN_USE: return "in use";
    case PIN_DEV_OPEN_HERE: return "open here";
    case PIN_DEV_NO_DRIVER: return "no driver";
    case PIN_DEV_UNSUPPORTED: return "unsupported";
    }
    return "?";
}

/* One "  - " bullet, wrapped under its text like the help. */
static void print_bullet(FILE *f, const char *text)
{
    char line[1400];
    snprintf(line, sizeof(line), "  - %s\n", text);
    print_wrapped(f, line, stdout_columns());
}

/* Everything that costs capture reliability, in one block: USB hubs between the computer and
 * a device, then the computer's own power settings (pin_perf_check). Prints nothing if there
 * is nothing to say. */
static void print_performance_recommendations(FILE *f, int behind_hub)
{
    int host = pin_perf_check();
    if (!behind_hub && !host)
        return;
    fprintf(f, "\nPerformance recommendations:\n");
    if (behind_hub) {
        char text[1024];
        snprintf(text, sizeof(text), "One or more devices are connected via a USB hub. %s",
                 pin_usb_hub_hint());
        print_bullet(f, text);
    }
    for (int kind = 1; kind <= PIN_PERF_POWER_PLAN; kind <<= 1) {
        if (!(host & kind))
            continue;
        print_bullet(f, pin_perf_text(kind));
        const char *fix = pin_perf_fix(kind);
        while (*fix) {
            const char *eol = strchr(fix, '\n');
            int n = eol ? (int)(eol - fix) : (int)strlen(fix);
            fprintf(f, "    %.*s\n", n, fix);
            fix += n + (eol ? 1 : 0);
        }
    }
}

static void print_device_table(FILE *f)
{
    pin_device_info_t devs[16];
    for (int i = 0; i < 16; i++) {
        memset(&devs[i], 0, sizeof(devs[i]));
        devs[i].size = sizeof(devs[i]);
    }
    int n = pin_enumerate(devs, 16);
    if (n <= 0) {
        fprintf(f, "No devices found.\n");
        return;
    }
    int shown = n > 16 ? 16 : n;
    int no_driver = 0, behind_hub = 0;
    fprintf(f,"Devices:\n  %-14s %-26s %-9s %-16s %s\n", "id", "name", "vid:pid", "serial", "state");
    for (int i = 0; i < shown; i++) {
        const pin_device_info_t *d = &devs[i];
        char state[96];
        snprintf(state, sizeof(state), "%s", dev_state_name(d->state));
        if (d->state == PIN_DEV_IN_USE || d->state == PIN_DEV_PREPARING) {
            size_t l = strlen(state);
            snprintf(state + l, sizeof(state) - l, " (pid %u)", (unsigned)d->owner_pid);
        }
        if (d->state != PIN_DEV_UNSUPPORTED && !d->tested) {
            size_t l = strlen(state);
            snprintf(state + l, sizeof(state) - l, " (untested)");
        }
        if (d->state == PIN_DEV_NO_DRIVER)
            no_driver = 1;
        if (d->hub_depth > 0) {
            size_t l = strlen(state);
            snprintf(state + l, sizeof(state) - l, " (behind USB hub)");
            behind_hub = 1;
        }
        fprintf(f, "  %-14s %-26s %04x:%04x %-16s %s\n", d->id, d->name, d->vid, d->pid,
                d->serial[0] ? d->serial : "-", state);
    }
    if (n > shown)
        fprintf(f, "  (%d more device(s) not shown)\n", n - shown);
    if (no_driver)
        fprintf(f, "A device has no driver: bind WinUSB to it (e.g. with Zadig), see docs/usage.md.\n");
    print_performance_recommendations(f, behind_hub);
}

/* ---- main --------------------------------------------------------------- */

static const char *log_level_name(int level)
{
    switch (level) {
    case 0: return "debug";
    case 1: return "info";
    case 2: return "warning";
    default: return "error";
    }
}

/* The core's log lines are process-wide events (pin_poll_event(NULL)), not
 * part of the session's queue. Prints them: every level with --debug,
 * else warnings and errors. */
static void print_log_events(int debug)
{
    pin_event_t ev;
    for (;;) {
        memset(&ev, 0, sizeof(ev));
        ev.size = sizeof(ev);
        if (!pin_poll_event(NULL, &ev))
            break;
        if (ev.kind != PIN_EVT_LOG || !(debug || ev.a >= 2))
            continue;
        size_t n = strlen(ev.text);
        while (n > 0 && (ev.text[n - 1] == '\n' || ev.text[n - 1] == '\r'))
            ev.text[--n] = '\0';
        if (n > 0)
            say("%s: %s", log_level_name(ev.a), ev.text);
    }
}

int main(int argc, char **argv)
{
    /* Hidden: the help text alone, unwrapped and without the device list; the build copies it into
     * docs/cli.md (scripts/sync-cli-help.sh). */
    if (argc == 2 && strcmp(argv[1], "--help-text") == 0) {
        fputs(pin_script_help(), stdout);
        return 0;
    }
    pin_script_t *sc = NULL;
    char err[256] = "";
    pin_status_t st = pin_script_parse(argc - 1, (const char *const *)(argv + 1), &sc, err, sizeof(err));
    if (st != PIN_OK) {
        fprintf(stderr, "error: %s\nRun MarvinCaptureCLI --help for usage.\n",
                err[0] ? err : pin_strerror(st));
        return 1;
    }
    if (pin_script_help_requested(sc)) {
        print_wrapped(stdout, pin_script_help(), stdout_columns());
        fputc('\n', stdout);
        print_device_table(stdout);
        pin_script_free(sc);
        return 0;
    }
    if (!pin_script_needs_session(sc)) {
        fprintf(stderr, "Nothing to do: the command line has settings but no action.\n"
                        "Run MarvinCaptureCLI --help for usage.\n");
        pin_script_free(sc);
        return 0;
    }

#if !defined(_WIN32)
    signal(SIGPIPE, SIG_IGN);   /* --capture -: a reader that exits is an error return, not a kill */
#endif
    const int debug = pin_script_debug(sc);
    const int tty = ISATTY_STDERR() ? 1 : 0;
    const int inplace = tty && !debug;
    pin_set_log_level(debug ? 0 : 2);
    install_interrupt_handler();

    pin_session_t *s = NULL;
    st = pin_open(pin_script_device(sc), &s);
    print_log_events(debug);
    if (st != PIN_OK) {
        fprintf(stderr, "error: cannot open the device: %s\n", pin_strerror(st));
        pin_script_free(sc);
        return 2;
    }
    {
        pin_device_info_t devs[16];
        for (int i = 0; i < 16; i++) {
            memset(&devs[i], 0, sizeof(devs[i]));
            devs[i].size = sizeof(devs[i]);
        }
        int n = pin_enumerate(devs, 16);
        const pin_device_info_t *d = NULL;
        for (int i = 0; i < n && i < 16; i++)
            if (devs[i].state == PIN_DEV_OPEN_HERE) { d = &devs[i]; break; }
        if (d)
            say("Device: %s (%s)%s%s", d->name, d->id, d->serial[0] ? ", serial " : "", d->serial);
        print_performance_recommendations(stderr, d && d->hub_depth > 0);
    }

    st = pin_script_run(s, sc);
    if (st != PIN_OK) {
        say("error: cannot run the script: %s", pin_strerror(st));
        pin_close(s);
        pin_script_free(sc);
        return 2;
    }

    char last_error[PIN_PATH_MAX] = "";
    int exit_code = 2;
    int done = 0, cancelled = 0;
    double next_status = 0;
    const double period = inplace ? 0.2 : 1.0;
    while (!done) {
        if (g_interrupts > 0 && !cancelled) {
            cancelled = 1;
            say("Stopping: finalising files... (Ctrl-C again to quit at once)");
            pin_script_cancel(s);
        }
        pin_event_t ev;
        int got = 0;
        print_log_events(debug);
        for (;;) {
            memset(&ev, 0, sizeof(ev));
            ev.size = sizeof(ev);
            if (!pin_poll_event(s, &ev))
                break;
            got = 1;
            switch (ev.kind) {
            case PIN_EVT_STEP:
                say("[%d/%d] %s", ev.a + 1, pin_script_step_count(sc), ev.text);
                break;
            case PIN_EVT_LOG:   /* the session queue carries none today; kept for safety */
                break;
            case PIN_EVT_ERROR:
                say("error: %s", ev.text);
                snprintf(last_error, sizeof(last_error), "%s", ev.text);
                break;
            case PIN_EVT_FILE_OPENED:
                say("Writing %s", ev.text);
                break;
            case PIN_EVT_FILE_CLOSED:
                if (ev.a == PIN_OK)
                    say("Closed %s", ev.text);
                else
                    say("Closed %s with an error: %s", ev.text, pin_strerror((pin_status_t)ev.a));
                break;
            case PIN_EVT_CAPTURE_ENDED:
                say("%s", ev.text);
                break;
            case PIN_EVT_NO_VIDEO:   /* the same sentence was just printed as the capture's end */
                snprintf(last_error, sizeof(last_error), "%s", ev.text);   /* ... and is the exit-5 reason: not twice */
                break;
            case PIN_EVT_DONE:
                exit_code = ev.a;
                if (ev.a != 0 && ev.text[0] && strcmp(ev.text, last_error) != 0)
                    say(ev.a == 130 ? "%s" : "error: %s", ev.text);
                done = 1;
                break;
            default:
                break;
            }
            if (done)
                break;
        }
        if (done)
            break;
        double t = now_s();
        if (t >= next_status) {
            show_status(s, inplace);
            next_status = t + period;
        }
        if (!got)
            sleep_ms(50);
    }
    finish_inplace();
    pin_close(s);
    print_log_events(debug);
    pin_script_free(sc);
    return exit_code;
}
