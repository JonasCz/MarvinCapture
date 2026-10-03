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
 * pinctl -- the API-only CLI/debug tool for marvin-core. Links
 * nothing but pin_api.h/marvin-core, so it doubles as the API's
 * integration test (same promise pincli/pindeck/pinanalog made for
 * src/core, one layer up). Subcommands:
 *
 *   list
 *   status <id>
 *   capture -d id -i input [-s std] [-f format] -o path [--title T]
 *           [--split] [--passes N] [--idle-min M] [--max-min M] [--duration S]
 *           [--aspect a] [--start-deck] [--rewind-first]
 *   deck <id> <play|pause|stop|ff|rew|state|timecode>
 *   preview-dump <id> -n N -o prefix
 *   preview-rate <id> [seconds]
 *   monitor <id> [seconds] [T:in=dv|svideo|composite] [T:std=pal|ntsc|...]
 *   watch
 *
 * Device ids (list/status/deck/capture -d/preview-dump/monitor) may
 * also be a device serial (16 hex chars, case-insensitive) or a path to an
 * existing file to replay -- see pin_api.h's pin_open().
 */

#include "pin_api.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
#include <unistd.h>
static void sleep_ms(int ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
#endif

static volatile sig_atomic_t g_stop;
static pin_session_t *g_stop_session;
static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
    if (g_stop_session)
        pin_capture_stop(g_stop_session);
    pin_devices_wake(); /* unblock `pinctl watch`'s pin_devices_wait() promptly */
}

static const char *dev_state_name(pin_dev_state_t s)
{
    switch (s) {
    case PIN_DEV_READY: return "READY";
    case PIN_DEV_PREPARING: return "PREPARING";
    case PIN_DEV_IN_USE: return "IN USE";
    case PIN_DEV_OPEN_HERE: return "OPEN (this process)";
    case PIN_DEV_NO_DRIVER: return "NO DRIVER";
    case PIN_DEV_UNSUPPORTED: return "UNSUPPORTED";
    }
    return "?";
}

static void print_device_list(void)
{
    pin_device_info_t devs[16];
    for (int i = 0; i < 16; i++) { memset(&devs[i], 0, sizeof(devs[i])); devs[i].size = sizeof(devs[i]); }
    int n = pin_enumerate(devs, 16);
    if (n == 0) { printf("No devices found.\n"); return; }
    int shown = n > 16 ? 16 : n;
    for (int i = 0; i < shown; i++) {
        printf("%-24s %-28s %04x:%04x  %s%s", devs[i].id, devs[i].name, devs[i].vid, devs[i].pid,
               dev_state_name(devs[i].state),
               devs[i].tested || devs[i].state == PIN_DEV_UNSUPPORTED ? "" : " (untested)");
        if (devs[i].state == PIN_DEV_IN_USE || devs[i].state == PIN_DEV_PREPARING)
            printf(" (pid %u)", (unsigned)devs[i].owner_pid);
        if (devs[i].serial[0])
            printf("  serial %s", devs[i].serial);
        printf("\n");
    }
}

static int cmd_list(void)
{
    print_device_list();
    return 0;
}

/* Loops pin_devices_wait(), printing the device list on every change --
 * useful for agent/scripted testing of hotplug without a GUI, and as a
 * quick manual check that plugging/unplugging the device is actually
 * noticed (see pin_api.h's pin_devices_wait()). Ctrl-C to stop. */
static int cmd_watch(void)
{
    printf("watching for device changes (Ctrl-C to stop)...\n");
    print_device_list();
    while (!g_stop) {
        int rc = pin_devices_wait(1000);
        if (g_stop)
            break;
        if (rc < 0) {
            fprintf(stderr, "pinctl: pin_devices_wait failed\n");
            return 1;
        }
        if (rc == 1) {
            printf("--- device list changed ---\n");
            print_device_list();
        }
    }
    printf("\n");
    return 0;
}

static int cmd_status(const char *id)
{
    pin_session_t *s = NULL;
    pin_status_t st = pin_open(id, &s);
    if (st != PIN_OK) { fprintf(stderr, "pinctl: open failed: %s\n", pin_strerror(st)); return 1; }
    pin_status_snapshot_t snap;
    memset(&snap, 0, sizeof(snap)); snap.size = sizeof(snap);
    pin_get_status(s, &snap);
    char line[512];
    pin_format_status_line(&snap, line, sizeof(line));
    printf("%s\n", line);
    pin_close(s);
    return 0;
}

static int cmd_deck(const char *id, const char *what)
{
    pin_session_t *s = NULL;
    pin_status_t st = pin_open(id, &s);
    if (st != PIN_OK) { fprintf(stderr, "pinctl: open failed: %s\n", pin_strerror(st)); return 1; }
    pin_set_input(s, PIN_INPUT_DV);
    /* wait for READY (or ERROR) before sending a deck command */
    for (int i = 0; i < 200; i++) {
        pin_status_snapshot_t snap; memset(&snap, 0, sizeof(snap)); snap.size = sizeof(snap);
        pin_get_status(s, &snap);
        if (snap.state == PIN_STATE_READY || snap.state == PIN_STATE_ERROR) break;
        sleep_ms(100);
    }

    int rc = 0;
    if (!strcmp(what, "state") || !strcmp(what, "timecode")) {
        pin_status_snapshot_t snap; memset(&snap, 0, sizeof(snap)); snap.size = sizeof(snap);
        sleep_ms(1200); /* let the ~1 Hz async transport poll answer once */
        pin_get_status(s, &snap);
        if (!strcmp(what, "state"))
            printf("deck: %d\n", (int)snap.deck);
        else
            printf("timecode: %s\n", snap.timecode[0] ? snap.timecode : "(none)");
    } else {
        pin_deck_cmd_t cmd;
        if (!strcmp(what, "play")) cmd = PIN_DECK_CMD_PLAY;
        else if (!strcmp(what, "pause")) cmd = PIN_DECK_CMD_PAUSE;
        else if (!strcmp(what, "stop")) cmd = PIN_DECK_CMD_STOP;
        else if (!strcmp(what, "ff")) cmd = PIN_DECK_CMD_FF;
        else if (!strcmp(what, "rew")) cmd = PIN_DECK_CMD_REW;
        else { fprintf(stderr, "pinctl: unknown deck command '%s'\n", what); pin_close(s); return 2; }
        st = pin_deck(s, cmd);
        if (st != PIN_OK) { fprintf(stderr, "pinctl: deck command failed: %s\n", pin_strerror(st)); rc = 1; }
    }
    pin_close(s);
    return rc;
}

static int cmd_capture(int argc, char **argv)
{
    const char *id = NULL, *input = "dv", *std_s = "auto", *fmt = NULL, *out_path = NULL;
    const char *title = "", *aspect = "auto";
    int split = 0, passes = 1, idle_min = 0, max_min = 0, duration = 0, start_deck = 0, rewind_first = 0;

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) id = argv[++i];
        else if (!strcmp(argv[i], "-i") && i + 1 < argc) input = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) std_s = argv[++i];
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) fmt = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "--title") && i + 1 < argc) title = argv[++i];
        else if (!strcmp(argv[i], "--split")) split = 1;
        else if (!strcmp(argv[i], "--passes") && i + 1 < argc) passes = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--idle-min") && i + 1 < argc) idle_min = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-min") && i + 1 < argc) max_min = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--duration") && i + 1 < argc) duration = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--aspect") && i + 1 < argc) aspect = argv[++i];
        else if (!strcmp(argv[i], "--start-deck")) start_deck = 1;
        else if (!strcmp(argv[i], "--rewind-first")) rewind_first = 1;
    }
    if (!out_path) { fprintf(stderr, "pinctl capture: -o <path> is required\n"); return 2; }

    pin_session_t *s = NULL;
    pin_status_t st = pin_open(id, &s);
    if (st != PIN_OK) { fprintf(stderr, "pinctl: open failed: %s\n", pin_strerror(st)); return 1; }
    g_stop_session = s;

    pin_input_t pin_input = !strcmp(input, "svideo") ? PIN_INPUT_SVIDEO
                            : !strcmp(input, "composite") ? PIN_INPUT_COMPOSITE : PIN_INPUT_DV;
    pin_set_input(s, pin_input);
    if (pin_input != PIN_INPUT_DV) {
        static const struct { const char *n; pin_std_t v; } tbl[] = {
            { "auto", PIN_STD_AUTO }, { "pal", PIN_STD_PAL }, { "ntsc", PIN_STD_NTSC },
        };
        for (unsigned i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++)
            if (!strcmp(std_s, tbl[i].n)) pin_set_standard(s, tbl[i].v);
    }

    pin_status_snapshot_t wait_snap;
    memset(&wait_snap, 0, sizeof(wait_snap)); wait_snap.size = sizeof(wait_snap);
    for (int i = 0; i < 100 && !g_stop; i++) {
        pin_get_status(s, &wait_snap);
        if (wait_snap.state == PIN_STATE_READY || wait_snap.state == PIN_STATE_ERROR) break;
        sleep_ms(100);
    }
    if (wait_snap.state == PIN_STATE_ERROR) {
        fprintf(stderr, "pinctl: device not ready: %s\n", wait_snap.error_text);
        pin_close(s);
        return 1;
    }

    pin_capture_opts_t opts;
    pin_capture_opts_defaults(&opts);
    strncpy(opts.path, out_path, sizeof(opts.path) - 1);
    strncpy(opts.title, title, sizeof(opts.title) - 1);
    opts.scene_split = split;
    opts.passes = passes < 1 ? 1 : passes;
    opts.idle_stop_minutes = idle_min;
    opts.max_duration_minutes = max_min;
    opts.start_deck = start_deck || rewind_first;
    opts.rewind_first = rewind_first;
    opts.aspect = !strcmp(aspect, "4:3") ? PIN_ASPECT_4_3
                 : !strcmp(aspect, "16:9") ? PIN_ASPECT_16_9 : PIN_ASPECT_AUTO;
    if (fmt) {
        static const struct { const char *k; pin_format_t f; } tbl[] = {
            { "dv", PIN_FMT_DV_RAW }, { "dv-avi", PIN_FMT_DV_AVI }, { "dv-mov", PIN_FMT_DV_MOV },
            { "hdv-ts", PIN_FMT_HDV_TS }, { "hdv-mov", PIN_FMT_HDV_MOV }, { "hdv-mkv", PIN_FMT_HDV_MKV },
            { "avi", PIN_FMT_ANALOG_AVI }, { "ffv1-mkv", PIN_FMT_ANALOG_FFV1_MKV },
        };
        for (unsigned i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++)
            if (!strcmp(fmt, tbl[i].k)) {
                pin_format_info_t fi; pin_format_info(tbl[i].f, &fi);
                if (fi.kind == PIN_KIND_ANALOG) opts.format_analog = tbl[i].f;
                else if (fi.kind == PIN_KIND_DV) opts.format_dv = tbl[i].f;
                else opts.format_hdv = tbl[i].f;
            }
    }

    st = pin_capture_start(s, &opts, 1);
    if (st != PIN_OK) {
        fprintf(stderr, "pinctl: capture start failed: %s\n", pin_strerror(st));
        pin_close(s);
        return 1;
    }

    /* pin_capture_start() is non-blocking (it queues to the session's
     * worker); give it a moment to actually reach CAPTURING before the
     * "has it stopped yet" loop below, or a status check in the race right
     * after posting would see the still-READY state and mistake "hasn't
     * started yet" for "already finished". */
    for (int i = 0; i < 50 && !g_stop; i++) {
        pin_status_snapshot_t snap; memset(&snap, 0, sizeof(snap)); snap.size = sizeof(snap);
        pin_get_status(s, &snap);
        if (snap.state == PIN_STATE_CAPTURING || snap.state == PIN_STATE_ERROR)
            break;
        sleep_ms(100);
    }

    time_t deadline = duration ? time(NULL) + duration : 0;
    int seen_capturing = 0;
    time_t ready_since = 0;
    while (!g_stop) {
        pin_status_snapshot_t snap; memset(&snap, 0, sizeof(snap)); snap.size = sizeof(snap);
        pin_get_status(s, &snap);
        char line[512];
        pin_format_status_line(&snap, line, sizeof(line));
        printf("\r%-100s", line);
        fflush(stdout);
        if (snap.state == PIN_STATE_CAPTURING) seen_capturing = 1;
        if (snap.state != PIN_STATE_CAPTURING && snap.state != PIN_STATE_STOPPING &&
            snap.state != PIN_STATE_REWINDING) {
            /* After an automatic rewind the session is READY while PLAY is
             * sent and the first frame awaited (tape threading, several
             * seconds): that is "not started yet", not "finished". */
            if (!seen_capturing && snap.state == PIN_STATE_READY) {
                if (!ready_since) ready_since = time(NULL);
                if (time(NULL) - ready_since < 60) { sleep_ms(200); continue; }
            }
            break;
        }
        ready_since = 0;
        if (deadline && time(NULL) >= deadline) { pin_capture_stop(s); deadline = 0; }
        pin_event_t ev;
        while (pin_poll_event(s, &ev)) {
            if (ev.kind == PIN_EVT_ERROR) fprintf(stderr, "\nerror: %s\n", ev.text);
        }
        sleep_ms(200);
    }
    printf("\n");
    pin_capture_stop(s);
    sleep_ms(300);
    pin_status_snapshot_t fin; memset(&fin, 0, sizeof(fin)); fin.size = sizeof(fin);
    pin_get_status(s, &fin);
    if (fin.stop_text[0])
        printf("%s\n", fin.stop_text);
    pin_close(s);
    return pin_stop_reason_abnormal(fin.stop_reason) ? 2 : 0;
}

static int cmd_preview_dump(int argc, char **argv, const char *id)
{
    int n = 10;
    const char *prefix = "preview";
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) n = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) prefix = argv[++i];
    }
    pin_session_t *s = NULL;
    pin_status_t st = pin_open(id, &s);
    if (st != PIN_OK) { fprintf(stderr, "pinctl: open failed: %s\n", pin_strerror(st)); return 1; }
    pin_set_input(s, PIN_INPUT_DV);

    char txt_path[600];
    snprintf(txt_path, sizeof(txt_path), "%s.txt", prefix);
    FILE *txt = fopen(txt_path, "w");

    uint64_t seq = 0;
    int written = 0;
    for (int i = 0; i < n * 5 && written < n && !g_stop; i++) {
        if (pin_preview_wait(s, seq, 2000) != 1)
            continue;
        pin_frame_t f; memset(&f, 0, sizeof(f)); f.size = sizeof(f);
        if (pin_preview_lock(s, &f) != PIN_OK)
            continue;
        seq = f.seq;
        char path[600];
        snprintf(path, sizeof(path), "%s-%04d.yuv", prefix, written);
        FILE *fp = fopen(path, "wb");
        if (fp) {
            int cw = f.width >> f.chroma_shift_x, ch = f.height >> f.chroma_shift_y;
            for (int r = 0; r < f.height; r++)
                fwrite(f.plane[0] + (size_t)r * f.stride[0], 1, (size_t)f.width, fp);
            for (int r = 0; r < ch; r++)
                fwrite(f.plane[1] + (size_t)r * f.stride[1], 1, (size_t)cw, fp);
            for (int r = 0; r < ch; r++)
                fwrite(f.plane[2] + (size_t)r * f.stride[2], 1, (size_t)cw, fp);
            fclose(fp);
            if (txt)
                fprintf(txt, "%s %dx%d chroma_shift %d,%d dar %d:%d matrix %d\n", path, f.width,
                        f.height, f.chroma_shift_x, f.chroma_shift_y, f.dar_num, f.dar_den, f.matrix);
            written++;
        }
        pin_preview_unlock(s);
    }
    if (txt) fclose(txt);
    printf("wrote %d frame(s) to %s-*.yuv (see %s)\n", written, prefix, txt_path);
    pin_close(s);
    return written > 0 ? 0 : 1;
}

static int cmd_preview_rate(const char *id, int seconds)
{
    pin_session_t *s = NULL;
    pin_status_t st = pin_open(id, &s);
    if (st != PIN_OK) { fprintf(stderr, "pinctl: open failed: %s\n", pin_strerror(st)); return 1; }
    pin_set_input(s, PIN_INPUT_DV);
    if (seconds <= 0) seconds = 10;

    uint64_t seq = 0, first_seq = 0;
    int have_first = 0;
    double t0 = 0, t_last = 0, worst_gap = 0;
    long frames = 0;
    time_t deadline = time(NULL) + seconds + 5; /* +5: bring-up */
    while (!g_stop && time(NULL) < deadline) {
        if (pin_preview_wait(s, seq, 500) != 1)
            continue;
        pin_frame_t f; memset(&f, 0, sizeof(f)); f.size = sizeof(f);
        if (pin_preview_lock(s, &f) != PIN_OK)
            continue;
        seq = f.seq;
        pin_preview_unlock(s);
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        double now = ts.tv_sec + ts.tv_nsec / 1e9;
        if (!have_first) { have_first = 1; first_seq = seq; t0 = now; }
        else if (now - t_last > worst_gap) worst_gap = now - t_last;
        t_last = now;
        frames++;
        if (now - t0 >= seconds)
            break;
    }
    if (have_first && t_last > t0)
        printf("preview: %ld frames seen, %llu decoded in %.1f s = %.2f fps decoded, worst gap %.0f ms\n",
               frames, (unsigned long long)(seq - first_seq), t_last - t0,
               (double)(seq - first_seq) / (t_last - t0), worst_gap * 1000.0);
    else
        printf("preview: no frames\n");
    pin_close(s);
    return have_first ? 0 : 1;
}

/* monitor <id> [seconds] [step...]: prints the status once per 0.5 s. Steps
 * are applied at their time: "T:in=dv|svideo|composite" or "T:std=auto|pal|
 * ntsc|...". With no in= step at 0 the session stays unprepared (as the GUI
 * would before the first pin_set_input). */
typedef struct { double at; int is_input; int value; int done; } mon_step_t;

static int cmd_monitor(const char *id, int argc, char **argv)
{
    int seconds = argc >= 1 ? atoi(argv[0]) : 0;
    mon_step_t steps[16]; int nsteps = 0;
    static const char *const stds[] = { "auto", "pal", "ntsc", "pal-m", "pal-n", "pal-60",
                                        "ntsc-443", "ntsc-j", "secam" };
    for (int i = 1; i < argc && nsteps < 16; i++) {
        char *colon = strchr(argv[i], ':');
        if (!colon) continue;
        mon_step_t *m = &steps[nsteps];
        memset(m, 0, sizeof(*m));
        m->at = atof(argv[i]);
        const char *what = colon + 1;
        if (!strncmp(what, "in=", 3)) {
            m->is_input = 1;
            m->value = !strcmp(what + 3, "dv") ? PIN_INPUT_DV
                     : !strcmp(what + 3, "svideo") ? PIN_INPUT_SVIDEO : PIN_INPUT_COMPOSITE;
        } else if (!strncmp(what, "std=", 4)) {
            m->value = 0;
            for (int k = 0; k < 9; k++)
                if (!strcmp(what + 4, stds[k])) m->value = k;
        } else continue;
        nsteps++;
    }
    pin_session_t *s = NULL;
    pin_status_t st = pin_open(id, &s);
    if (st != PIN_OK) { fprintf(stderr, "pinctl: open failed: %s\n", pin_strerror(st)); return 1; }
    struct timespec ts0; clock_gettime(CLOCK_MONOTONIC, &ts0);
    double t0 = ts0.tv_sec + ts0.tv_nsec / 1e9, tend = seconds > 0 ? seconds : 3600;
    while (!g_stop) {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        double t = ts.tv_sec + ts.tv_nsec / 1e9 - t0;
        if (t >= tend) break;
        for (int i = 0; i < nsteps; i++) {
            if (steps[i].done || t < steps[i].at) continue;
            steps[i].done = 1;
            printf("[%5.1f] -> %s %d\n", t, steps[i].is_input ? "set_input" : "set_standard", steps[i].value);
            if (steps[i].is_input) pin_set_input(s, (pin_input_t)steps[i].value);
            else pin_set_standard(s, (pin_std_t)steps[i].value);
        }
        pin_status_snapshot_t snap; memset(&snap, 0, sizeof(snap)); snap.size = sizeof(snap);
        pin_get_status(s, &snap);
        char line[512];
        pin_format_status_line(&snap, line, sizeof(line));
        printf("[%5.1f] %s | sig=%d cam=%d %dx%d std=%d kind=%d frames=%llu | %s\n", t, line, snap.signal,
               snap.camera_present, snap.width, snap.height, (int)snap.detected_std,
               (int)snap.stream_kind, (unsigned long long)snap.frames, snap.detail);
        pin_event_t ev;
        while (pin_poll_event(s, &ev))
            printf("  event kind=%d a=%d text=%s\n", ev.kind, ev.a, ev.text);
        sleep_ms(500);
    }
    pin_close(s);
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: pinctl list\n"
        "       pinctl status <id>\n"
        "       pinctl capture -d id -i dv|svideo|composite [-s std] [-f format] -o path\n"
        "                      [--title T] [--split] [--passes N] [--idle-min M] [--max-min M]\n"
        "                      [--duration S] [--aspect auto|4:3|16:9] [--start-deck]\n"
        "                      [--rewind-first]\n"
        "       pinctl deck <id> <play|pause|stop|ff|rew|state|timecode>\n"
        "       pinctl preview-dump <id> [-n N] [-o prefix]\n"
        "       pinctl preview-rate <id> [seconds]\n"
        "       pinctl monitor <id> [seconds] [T:in=dv|svideo|composite] [T:std=pal|ntsc]\n"
        "       pinctl watch\n");
}

int main(int argc, char **argv)
{
    signal(SIGINT, on_sigint);
    if (argc < 2) { usage(); return 2; }

    const char *sub = argv[1];
    if (!strcmp(sub, "list"))
        return cmd_list();
    if (!strcmp(sub, "status") && argc >= 3)
        return cmd_status(argv[2]);
    if (!strcmp(sub, "capture"))
        return cmd_capture(argc - 2, argv + 2);
    if (!strcmp(sub, "deck") && argc >= 4)
        return cmd_deck(argv[2], argv[3]);
    if (!strcmp(sub, "preview-dump") && argc >= 3)
        return cmd_preview_dump(argc - 3, argv + 3, argv[2]);
    if (!strcmp(sub, "preview-rate") && argc >= 3)
        return cmd_preview_rate(argv[2], argc >= 4 ? atoi(argv[3]) : 0);
    if (!strcmp(sub, "monitor") && argc >= 3)
        return cmd_monitor(argv[2], argc - 3, argv + 3);
    if (!strcmp(sub, "watch"))
        return cmd_watch();

    usage();
    return 2;
}
