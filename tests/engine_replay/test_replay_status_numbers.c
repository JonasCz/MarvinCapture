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
 * ctest: the numbers the status shows, on the replay device through pin_api.h:
 *   - before any frame, a DV / HDV input estimates the free space at the DV rate (not the
 *     analog one), a folder that cannot be asked gives no estimate (est_seconds_left < 0, no
 *     "min left" text) and no low-disk warning;
 *   - a caller built against a shorter pin_status_snapshot_t / pin_event_t gets only its
 *     fields, the memory behind them is not touched;
 *   - a PAL then NTSC tape captured as raw DV: stop_captured_s is the sum of both rates'
 *     durations, the total bytes never exceed what the files hold (a file handed over at the
 *     format change is not counted twice), and pin_get_capture_end_status() shows the ended
 *     capture's totals; `frames` is the frames in the files; the snapshot carries the running
 *     capture's own limits (for countdowns) and its file count, and a closed file's bytes are in
 *     the total while the capture still runs; the end summary names the bytes and files.
 *
 * Usage: test_replay_status_numbers <data dir>
 */

#include "replay_test_util.h"

#include <stddef.h>

static long file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    CHECK(f);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

static void test_estimate_before_data(pin_session_t *s)
{
    pin_capture_opts_t opts;
    pin_capture_opts_defaults(&opts);
    strncpy(opts.path, "status_numbers_hint", sizeof(opts.path) - 1);
    opts.format_dv = PIN_FMT_DV_RAW;
    CHECK(pin_set_output_hint(s, &opts) == PIN_OK);
    pin_status_snapshot_t st;
    memset(&st, 0, sizeof(st));
    st.size = sizeof(st);
    CHECK(pin_get_status(s, &st) == PIN_OK);
    printf("DV input, no data: %.0f GB/h, %.0f s left\n", st.est_bytes_per_hour / 1e9, st.est_seconds_left);
    CHECK(st.disk_free_bytes > 0 && st.est_seconds_left >= 0);
    CHECK(st.est_bytes_per_hour > 5e9 && st.est_bytes_per_hour < 20e9);   /* DV, not analog's ~75 GB/h */

    strncpy(opts.path, "no_such_folder_for_status_numbers/clip", sizeof(opts.path) - 1);
    CHECK(pin_set_output_hint(s, &opts) == PIN_OK);
    memset(&st, 0, sizeof(st));
    st.size = sizeof(st);
    CHECK(pin_get_status(s, &st) == PIN_OK);
    CHECK(st.est_seconds_left < 0);
    CHECK(!st.disk_low);
    char text[256];
    pin_format_storage_free(&st, text, sizeof(text));
    CHECK(strstr(text, "left") == NULL);
    pin_format_storage_detail(&st, text, sizeof(text));
    CHECK(strstr(text, "left") == NULL);
    printf("OK: estimate at the DV rate; an unreadable folder gives no estimate and no warning\n");
}

static void test_caller_struct_size(pin_session_t *s)
{
    enum { BIG = 16384 };
    unsigned char *buf = malloc(BIG);
    CHECK(buf && sizeof(pin_status_snapshot_t) < BIG);
    memset(buf, 0xAB, BIG);
    pin_status_snapshot_t *st = (pin_status_snapshot_t *)buf;
    st->size = (uint32_t)offsetof(pin_status_snapshot_t, last_error) + 4;
    uint32_t want = st->size;
    CHECK(pin_get_status(s, st) == PIN_OK);
    CHECK_EQ_I(st->size, want);
    for (size_t i = want; i < BIG; i++)
        CHECK(buf[i] == 0xAB);
    CHECK(st->state == PIN_STATE_READY);
    free(buf);
    printf("OK: a short caller struct is filled up to its size only\n");
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    char tape[4096];
    snprintf(tape, sizeof(tape), "%s/ep88-pal-ntsc.bin", argv[1]);
    pin_test_require_fixture(tape);

    pin_session_t *s = pin_test_open_replay(tape);
    pin_test_prepare_dv(s);
    test_estimate_before_data(s);
    test_caller_struct_size(s);

    remove("status_numbers.dv");
    remove("status_numbers-0002.dv");
    pin_status_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    snap.size = sizeof(snap);
    CHECK(pin_deck(s, PIN_DECK_CMD_REW) == PIN_OK);
    int stopped = 0;
    for (int i = 0; i < 300 && !stopped; i++) {
        pin_test_sleep_ms(20);
        pin_get_status(s, &snap);
        stopped = snap.deck == PIN_DECK_STOPPED;
    }
    CHECK(stopped);
    pin_capture_opts_t opts;
    pin_capture_opts_defaults(&opts);
    strncpy(opts.path, "status_numbers", sizeof(opts.path) - 1);
    opts.format_dv = PIN_FMT_DV_RAW;
    opts.idle_stop_minutes = 7;
    opts.max_duration_minutes = 9;
    opts.start_deck = 0;
    /* a drive a little slower than the tape: the first file still has a queue when it is handed
     * over, so its bytes reach the total only as the closer finishes it */
    pin_test_setenv("PIN_TEST_SINK_DELAY_MS", "45");
    CHECK(pin_deck(s, PIN_DECK_CMD_PLAY) == PIN_OK);
    CHECK(pin_capture_start(s, &opts, 1) == PIN_OK);

    /* the total while it runs: it never passes what the files end up holding; once the first
     * file is closed, its bytes are in it while the capture goes on */
    uint64_t max_total = 0;
    int saw_limits = 0, first_closed = 0, closed_in_total = 0;
    for (int i = 0; i < 20000; i++) {
        memset(&snap, 0, sizeof(snap));
        snap.size = sizeof(snap);
        pin_get_status(s, &snap);
        if (snap.total_bytes_written > max_total)
            max_total = snap.total_bytes_written;
        if (snap.state == PIN_STATE_CAPTURING) {
            saw_limits |= snap.capture_idle_stop_s == 420.0 && snap.capture_duration_s == 540.0 &&
                          snap.capture_with_deck == 0 && snap.capture_files >= 1;
            if (first_closed && !closed_in_total &&
                snap.total_bytes_written >= (uint64_t)file_size("status_numbers.dv"))
                closed_in_total = 1;
        }
        pin_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.size = sizeof(ev);
        while (pin_poll_event(s, &ev)) {
            if (ev.kind == PIN_EVT_FILE_CLOSED && !first_closed && strstr(ev.text, "status_numbers.dv")) {
                first_closed = 1;
                printf("first file closed at poll %d, state %d, total %llu\n", i, (int)snap.state,
                       (unsigned long long)snap.total_bytes_written);
            }
            ev.size = sizeof(ev);
        }
        if (snap.stop_reason != PIN_STOP_NONE && snap.state == PIN_STATE_READY)
            break;
        pin_test_sleep_ms(2);
    }
    pin_test_setenv("PIN_TEST_SINK_DELAY_MS", "");
    CHECK(saw_limits);
    CHECK(first_closed);
    CHECK(closed_in_total);
    CHECK(snap.capture_idle_stop_s == 0 && snap.capture_duration_s == 0);   /* no capture now */
    CHECK(snap.state == PIN_STATE_READY);
    CHECK_EQ_I(snap.stop_reason, PIN_STOP_END_OF_TAPE);

    long pal_frames = file_size("status_numbers.dv") / 144000;
    long ntsc_frames = file_size("status_numbers-0002.dv") / 120000;
    uint64_t on_disk = (uint64_t)(file_size("status_numbers.dv") + file_size("status_numbers-0002.dv"));
    double want_s = (double)pal_frames / 25.0 + (double)ntsc_frames * 1001.0 / 30000.0;
    printf("%ld PAL + %ld NTSC frame(s): captured %.2f s (expect %.2f), max total %llu of %llu bytes\n",
           pal_frames, ntsc_frames, snap.stop_captured_s, want_s, (unsigned long long)max_total,
           (unsigned long long)on_disk);
    CHECK(snap.stop_captured_s > want_s - 0.08 && snap.stop_captured_s < want_s + 0.08);
    CHECK(max_total <= on_disk);
    CHECK_EQ_I(snap.total_bytes_written, on_disk);

    pin_status_snapshot_t end;
    memset(&end, 0, sizeof(end));
    end.size = sizeof(end);
    CHECK(pin_get_capture_end_status(s, &end) == PIN_OK);
    CHECK_EQ_I(end.total_bytes_written, on_disk);
    CHECK_EQ_I(end.frames, snap.frames);
    CHECK_EQ_I(end.frames, (uint64_t)(pal_frames + ntsc_frames));   /* the frames in the files */
    CHECK_EQ_I(end.stop_reason, PIN_STOP_END_OF_TAPE);
    CHECK_EQ_I(end.capture_files, 2);
    char line[256];
    pin_format_capture_summary(&end, line, sizeof(line));
    printf("summary: %s\n", line);
    CHECK(strstr(line, " in 2 files") != NULL);
    printf("OK: captured length, byte totals and the ended capture's status\n");
    pin_close(s);
    return 0;
}
