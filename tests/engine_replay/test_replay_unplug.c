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
 * ctest: the capture device disappears during a capture (USB unplug). The replay
 * device fakes it (PIN_REPLAY_UNPLUG_AFTER=<frames>) and the session goes through
 * the same stream_failed() path as a real LIBUSB_ERROR_NO_DEVICE: the open file
 * must be finalised (raw DV whole frames, DV AVI with its idx1 index, HDV TS whole
 * packets), the stop reason must be PIN_STOP_DEVICE_LOST (abnormal), the session
 * ends in ERROR, the stop text names the file, and a new session opens afterwards.
 *
 * Usage: test_replay_unplug <dv_frames_file.dv> <hdv.ts>
 */

#include "replay_test_util.h"

static long file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

static int file_contains(const char *path, const char *needle)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n);
    CHECK(buf && fread(buf, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    size_t len = strlen(needle);
    int found = 0;
    for (long i = 0; i + (long)len <= n && !found; i++)
        found = memcmp(buf + i, needle, len) == 0;
    free(buf);
    return found;
}

static void run_case(const char *trace, const char *base, const char *ext, int dv_fmt, int hdv_fmt)
{
    pin_test_setenv("PIN_REPLAY_UNPLUG_AFTER", "5");
    pin_session_t *s = pin_test_open_replay(trace);
    pin_test_prepare_dv(s);

    pin_capture_opts_t opts;
    pin_capture_opts_defaults(&opts);
    strncpy(opts.path, base, sizeof(opts.path) - 1);
    opts.format_dv = (pin_format_t)dv_fmt;
    opts.format_hdv = (pin_format_t)hdv_fmt;
    CHECK(pin_capture_start(s, &opts, 1) == PIN_OK);

    pin_status_snapshot_t snap;
    pin_test_wait_state(s, PIN_STATE_ERROR, -1, 10000, &snap); /* no hang */
    CHECK(snap.state == PIN_STATE_ERROR);
    CHECK(snap.stop_reason == PIN_STOP_DEVICE_LOST);
    CHECK(pin_stop_reason_abnormal(snap.stop_reason));
    CHECK(strstr(snap.stop_text, "disconnected") != NULL);
    CHECK(strstr(snap.stop_text, base) != NULL);   /* "saved up to that point: <path>" */
    CHECK(snap.error_text[0] != 0);

    int saw_ended = 0, saw_closed = 0, saw_error = 0;
    pin_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.size = sizeof(ev);
    while (pin_poll_event(s, &ev)) {
        if (ev.kind == PIN_EVT_CAPTURE_ENDED) { saw_ended = 1; CHECK(ev.a == PIN_STOP_DEVICE_LOST); }
        if (ev.kind == PIN_EVT_FILE_CLOSED) { saw_closed = 1; CHECK(ev.a == PIN_OK); }
        if (ev.kind == PIN_EVT_ERROR) saw_error = 1;
        ev.size = sizeof(ev);
    }
    CHECK(saw_ended && saw_closed && saw_error);

    pin_close(s); /* must not hang on the dead session */

    char path[256];
    snprintf(path, sizeof(path), "%s.%s", base, ext);
    long n = file_size(path);
    CHECK(n > 0);
    if (strcmp(ext, "dv") == 0)
        CHECK(n % 120000 == 0);
    else if (strcmp(ext, "ts") == 0)
        CHECK(n % 188 == 0);
    else if (strcmp(ext, "avi") == 0)
        CHECK(file_contains(path, "idx1")); /* index written: playable up to the cut */

    /* the device is back: a new session works without restarting */
    pin_test_setenv("PIN_REPLAY_UNPLUG_AFTER", "");
    s = pin_test_open_replay(trace);
    pin_test_prepare_dv(s);
    pin_close(s);
    printf("OK: %s (%ld bytes)\n", path, n);
}

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    run_case(argv[1], "unplug_raw", "dv", PIN_FMT_DV_RAW, PIN_FMT_HDV_TS);
    run_case(argv[1], "unplug_avi", "avi", PIN_FMT_DV_AVI, PIN_FMT_HDV_TS);
    if (pin_test_file_exists(argv[2]))
        run_case(argv[2], "unplug_hdv", "ts", PIN_FMT_DV_RAW, PIN_FMT_HDV_TS);
    return 0;
}
