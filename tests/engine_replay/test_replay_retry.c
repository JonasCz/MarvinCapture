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
 * ctest: a capture with retry (pin_capture_opts_t.retry) goes through the retry engine
 * (pin_retry.h) without changing what reaches the file: the replay device has no deck to wind,
 * so with the test seam PIN_TEST_RETRY_REPLAY the engine holds frames back, holds a damaged
 * section until it is let go (the window, or the end of the capture) and releases everything in
 * order. The file and the frame counts must be the same as without retry, for
 *   - a raw EP 0x88 dump of a real camera (it has real losses: error frames, a held section),
 *   - an HDV transport stream (GOP slots, the continuity counter fix: one read, no change),
 * and the status must say the capture retries (retry_on) and, after it, how many sections.
 *
 * Usage: test_replay_retry <ep88-pal.bin> <hdv.ts>
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

static int same_files(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    int same = fa && fb;
    long at = 0;
    while (same) {
        int ca = fgetc(fa), cb = fgetc(fb);
        if (ca != cb) {
            same = 0;
            printf("first difference at byte %ld (packet %ld, byte %ld): %02x vs %02x\n", at,
                   at / 188, at % 188, ca, cb);
        }
        if (ca == EOF || cb == EOF) break;
        at++;
    }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return same;
}

/* One capture of the whole replay, from its start to its end (END_OF_TAPE). */
static void capture(const char *src, int kind_hdv, int retry, const char *base,
                    pin_status_snapshot_t *end, int *saw_retry_on)
{
    pin_test_setenv("PIN_TEST_RETRY_REPLAY", retry ? "1" : "");
    pin_session_t *s = pin_test_open_replay(src);
    pin_test_prepare_dv(s);
    pin_status_snapshot_t snap = { .size = sizeof(snap) };
    CHECK_EQ_I(pin_deck(s, PIN_DECK_CMD_REW), PIN_OK);
    int stopped = 0;
    for (int i = 0; i < 300 && !stopped; i++) {
        pin_test_sleep_ms(20);
        pin_get_status(s, &snap);
        stopped = snap.deck == PIN_DECK_STOPPED;
    }
    CHECK(stopped);
    CHECK_EQ_I(pin_deck(s, PIN_DECK_CMD_PLAY), PIN_OK);
    pin_capture_opts_t o;
    pin_capture_opts_defaults(&o);
    snprintf(o.path, sizeof(o.path), "%s", base);
    o.format_dv = PIN_FMT_DV_RAW;
    o.format_hdv = PIN_FMT_HDV_TS;
    o.retry = retry;
    CHECK_EQ_I(pin_capture_start(s, &o, 1), PIN_OK);
    *saw_retry_on = 0;
    for (int i = 0; i < 3000; i++) {
        pin_test_sleep_ms(20);
        pin_get_status(s, &snap);
        if (snap.retry_on)
            *saw_retry_on = 1;
        if (snap.stop_reason != PIN_STOP_NONE && snap.state == PIN_STATE_READY)
            break;
    }
    CHECK_EQ_I(snap.stop_reason, PIN_STOP_END_OF_TAPE);
    memset(end, 0, sizeof(*end));
    end->size = sizeof(*end);
    CHECK_EQ_I(pin_get_capture_end_status(s, end), PIN_OK);
    pin_close(s);
    (void)kind_hdv;
}

static void compare(const char *src, int hdv, const char *ext)
{
    char a[64], b[64];
    snprintf(a, sizeof(a), "rt_off.%s", ext);
    snprintf(b, sizeof(b), "rt_on.%s", ext);
    remove(a);
    remove(b);
    pin_status_snapshot_t off, on;
    int r_off = 0, r_on = 0;
    capture(src, hdv, 0, "rt_off", &off, &r_off);
    capture(src, hdv, 1, "rt_on", &on, &r_on);
    printf("%s: without retry %ld bytes, %llu frames, %llu with error, %llu dropped\n", ext,
           file_size(a), (unsigned long long)off.frames, (unsigned long long)off.frames_error,
           (unsigned long long)off.frames_dropped);
    printf("%s: with retry    %ld bytes, %llu frames, %llu with error, %llu dropped, %llu sections\n",
           ext, file_size(b), (unsigned long long)on.frames, (unsigned long long)on.frames_error,
           (unsigned long long)on.frames_dropped, (unsigned long long)on.retry_sections);
    CHECK(!r_off);
    CHECK(r_on);
    CHECK(file_size(a) > 0);
    int same = same_files(a, b);
    if (!same) {
        printf("kept %s and %s\n", a, b);
        CHECK(same);
    }
    CHECK_EQ_I(on.frames, off.frames);
    CHECK_EQ_I(on.frames_error, off.frames_error);
    CHECK_EQ_I(on.frames_dropped, off.frames_dropped);
    CHECK_EQ_I(on.retry_mended, 0);
    remove(a);
    remove(b);
    printf("OK: %s through the retry engine is the same file\n", ext);
}

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    pin_test_require_fixture(argv[1]);
    pin_test_require_fixture(argv[2]);
    compare(argv[1], 0, "dv");
    compare(argv[2], 1, "m2t");
    pin_test_setenv("PIN_TEST_RETRY_REPLAY", "");
    return 0;
}
