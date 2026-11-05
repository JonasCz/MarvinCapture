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

/* pin_fmtwatch: a format change counts only once it persists (a single frame or GOP
 * with another value never does), is back-dated to its first unit, and what it does
 * to the open file depends on the file's format. */

#include "pin_fmtwatch.h"
#include <stdio.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);             \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static pin_stream_fmt_t dv(int pal, int wide, int rate)
{
    pin_stream_fmt_t f;
    pin_stream_fmt_init(&f, PIN_KIND_DV);
    f.pal = pal;
    f.wide = wide;
    if (rate) {
        f.audio_rate = rate;
        f.audio_channels = rate == 32000 ? 4 : 2;
        f.audio_bits = rate == 32000 ? 12 : 16;
    }
    return f;
}

/* an HDV picture: with a sequence header (w > 0) or without (all unknown) */
static pin_stream_fmt_t hdv(int w, int h, int fps_num, int fps_den, int progressive)
{
    pin_stream_fmt_t f;
    pin_stream_fmt_init(&f, PIN_KIND_HDV);
    if (w > 0) {
        f.width = w;
        f.height = h;
        f.fps_num = fps_num;
        f.fps_den = fps_den;
        f.progressive = progressive;
    }
    return f;
}

/* feeds n units of f from *idx on; returns the index of the unit that confirmed a change
 * (-1 = none), *start = where it was back-dated to */
static long feed(pin_fmtwatch_t *w, pin_stream_fmt_t f, int n, long *idx, long *start)
{
    long at = -1;
    for (int i = 0; i < n; i++) {
        long st = -1;
        pin_fmtwatch_result_t r = pin_fmtwatch_feed(w, &f, *idx, &st, NULL);
        if (r == PIN_FMTWATCH_CHANGED) {
            at = *idx;
            *start = st;
        }
        (*idx)++;
    }
    return at;
}

/* a GOP of 12 pictures: the first with the sequence header */
static long feed_gop(pin_fmtwatch_t *w, pin_stream_fmt_t seq, long *idx, long *start)
{
    long a = feed(w, seq, 1, idx, start);
    long b = feed(w, hdv(0, 0, 0, 0, -1), 11, idx, start);
    return a >= 0 ? a : b;
}

static void test_dv_cooldown(void)
{
    pin_fmtwatch_t w;
    pin_fmtwatch_init(&w);
    long idx = 0, start = -1;
    pin_stream_fmt_t pal = dv(1, 0, 48000), ntsc = dv(0, 0, 48000);
    pin_stream_fmt_t first = pal;
    CHECK(pin_fmtwatch_feed(&w, &first, idx++, NULL, NULL) == PIN_FMTWATCH_FIRST, "first format");
    CHECK(feed(&w, pal, 20, &idx, &start) < 0, "steady PAL");

    /* one NTSC frame (a corrupt DSF), then PAL again: nothing */
    CHECK(feed(&w, ntsc, 1, &idx, &start) < 0, "single frame: no change");
    long st;
    CHECK(pin_fmtwatch_pending(&w, &st) && st == idx - 1, "pending from the odd frame");
    CHECK(feed(&w, pal, 1, &idx, &start) < 0, "back to PAL");
    CHECK(!pin_fmtwatch_pending(&w, NULL), "candidate dropped");
    /* four in a row, then PAL: still nothing */
    CHECK(feed(&w, ntsc, PIN_FMTWATCH_DV_FRAMES - 1, &idx, &start) < 0, "one short of the cool-down");
    CHECK(feed(&w, pal, 1, &idx, &start) < 0, "back to PAL");
    CHECK(w.cur.pal == 1, "still PAL");

    /* a real change: confirmed at the 5th frame, back-dated to the 1st */
    long first_ntsc = idx;
    long at = feed(&w, ntsc, 20, &idx, &start);
    CHECK(at == first_ntsc + PIN_FMTWATCH_DV_FRAMES - 1, "confirmed at the 5th NTSC frame");
    CHECK(start == first_ntsc, "back-dated to the first NTSC frame");
    CHECK(w.cur.pal == 0 && w.cur.audio_rate == 48000, "now NTSC, audio as before");

    /* units without the pack are neutral: 16:9 frames with VAUX-less frames between still
     * confirm, and the change starts at the first 16:9 frame */
    pin_stream_fmt_t wide = dv(0, 1, 48000), novaux = dv(0, -1, 48000);
    long first_wide = idx;
    long a1 = feed(&w, wide, 1, &idx, &start);
    long a2 = feed(&w, novaux, 3, &idx, &start);
    long a3 = feed(&w, wide, PIN_FMTWATCH_DV_FRAMES - 1, &idx, &start);
    CHECK(a1 < 0 && a2 < 0 && a3 == idx - 1, "16:9 confirmed after 5 frames that say so");
    CHECK(start == first_wide, "back-dated to the first 16:9 frame");
    CHECK(w.cur.wide == 1, "now 16:9");

    /* a lone 4:3 frame, then only frames without VAUX: the candidate expires (the caller
     * stops holding), and a 4:3 run after that starts afresh */
    pin_stream_fmt_t narrow = dv(0, 0, 48000);
    long lone = idx;
    feed(&w, narrow, 1, &idx, &start);
    feed(&w, novaux, PIN_FMTWATCH_DV_STALE, &idx, &start);
    CHECK(pin_fmtwatch_pending(&w, &st) && st == lone, "still pending at the stale limit");
    feed(&w, novaux, 1, &idx, &start);
    CHECK(!pin_fmtwatch_pending(&w, NULL), "expired");
    long first_narrow = idx;
    CHECK(feed(&w, narrow, 10, &idx, &start) == first_narrow + PIN_FMTWATCH_DV_FRAMES - 1 &&
          start == first_narrow, "a later run is back-dated to its own start");

    /* an unknown field learnt later is no change */
    pin_fmtwatch_init(&w);
    idx = 0;
    pin_stream_fmt_t noaudio = dv(1, 0, 0);
    CHECK(pin_fmtwatch_feed(&w, &noaudio, idx++, NULL, NULL) == PIN_FMTWATCH_FIRST, "first, no AAUX");
    CHECK(feed(&w, pal, 10, &idx, &start) < 0, "audio learnt: no change");
    CHECK(w.cur.audio_rate == 48000, "audio filled in");
    /* an audio change */
    pin_stream_fmt_t a32 = dv(1, 0, 32000);
    long first32 = idx;
    CHECK(feed(&w, a32, 10, &idx, &start) == first32 + PIN_FMTWATCH_DV_FRAMES - 1 && start == first32,
          "48 -> 32 kHz confirmed and back-dated");
}

static void test_kind_and_hdv(void)
{
    pin_fmtwatch_t w;
    pin_fmtwatch_init(&w);
    long idx = 0, start = -1;
    pin_stream_fmt_t pal = dv(1, 0, 48000);
    pin_stream_fmt_t i1080 = hdv(1440, 1080, 25, 1, 0), p720 = hdv(1280, 720, 60000, 1001, 1);
    CHECK(pin_fmtwatch_feed(&w, &pal, idx++, NULL, NULL) == PIN_FMTWATCH_FIRST, "first");
    feed(&w, pal, 10, &idx, &start);

    /* a single HDV GOP between DV frames: no change */
    CHECK(feed_gop(&w, i1080, &idx, &start) < 0, "one GOP: no change");
    CHECK(feed(&w, pal, 1, &idx, &start) < 0 && w.cur.kind == PIN_KIND_DV, "DV again");
    /* two GOPs: still not */
    CHECK(feed_gop(&w, i1080, &idx, &start) < 0 && feed_gop(&w, i1080, &idx, &start) < 0, "two GOPs");
    CHECK(feed(&w, pal, 1, &idx, &start) < 0 && w.cur.kind == PIN_KIND_DV, "DV again");

    /* DV -> HDV: the 3rd GOP confirms, back-dated to the 1st picture */
    long first_hdv = idx;
    long at = -1;
    for (int g = 0; g < 5; g++) {
        long a = feed_gop(&w, i1080, &idx, &start);
        if (a >= 0) at = a;
    }
    CHECK(at == first_hdv + 2 * 12, "confirmed at the 3rd GOP's sequence header");
    CHECK(start == first_hdv, "back-dated to the first HDV picture");
    CHECK(w.cur.kind == PIN_KIND_HDV && w.cur.width == 1440 && w.cur.progressive == 0, "now HDV 1080i");

    /* a GOP with another sequence header (a corrupt one): nothing; 1080i -> 720p: a change */
    CHECK(feed_gop(&w, p720, &idx, &start) < 0, "one odd GOP");
    CHECK(feed_gop(&w, i1080, &idx, &start) < 0 && w.cur.width == 1440, "1080i again");
    long first_720 = idx;
    at = -1;
    for (int g = 0; g < 4; g++) {
        long a = feed_gop(&w, p720, &idx, &start);
        if (a >= 0) at = a;
    }
    CHECK(at >= 0 && start == first_720 && w.cur.width == 1280 && w.cur.fps_num == 60000,
          "720p confirmed, back-dated");

    /* HDV -> DV: 5 frames */
    long first_dv = idx;
    CHECK(feed(&w, pal, 10, &idx, &start) == first_dv + 4 && start == first_dv, "back to DV");
    CHECK(w.cur.kind == PIN_KIND_DV, "DV");
}

static void test_action(void)
{
    pin_stream_fmt_t pal = dv(1, 0, 48000), ntsc = dv(0, 0, 48000), wide = dv(1, 1, 48000),
                     a32 = dv(1, 0, 32000);
    pin_stream_fmt_t i1080 = hdv(1440, 1080, 25, 1, 0), p720 = hdv(1280, 720, 60000, 1001, 1),
                     i1080b = hdv(1440, 1080, 25, 1, 0);

    /* raw DV: only the standard */
    CHECK(pin_format_change_action(PIN_FMT_DV_RAW, 0, 0, &pal, &ntsc) == PIN_FMTCHG_NEW_FILE, "raw std");
    CHECK(pin_format_change_action(PIN_FMT_DV_RAW, 0, 0, &pal, &wide) == PIN_FMTCHG_KEEP, "raw aspect");
    CHECK(pin_format_change_action(PIN_FMT_DV_RAW, 0, 0, &pal, &a32) == PIN_FMTCHG_KEEP, "raw audio");
    /* the other DV formats: anything */
    pin_format_t others[] = { PIN_FMT_DV_AVI, PIN_FMT_DV_MOV, PIN_FMT_DV_H264_MKV, PIN_FMT_DV_HEVC_MKV };
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        CHECK(pin_format_change_action(others[i], 0, 0, &pal, &ntsc) == PIN_FMTCHG_NEW_FILE, "std");
        CHECK(pin_format_change_action(others[i], 0, 0, &pal, &wide) == PIN_FMTCHG_NEW_FILE, "aspect");
        CHECK(pin_format_change_action(others[i], 0, 0, &pal, &a32) == PIN_FMTCHG_NEW_FILE, "audio");
        /* the user's aspect: an aspect change changes nothing in the file */
        CHECK(pin_format_change_action(others[i], 0, 1, &pal, &wide) == PIN_FMTCHG_KEEP, "forced aspect");
    }
    /* HDV: the video format */
    CHECK(pin_format_change_action(PIN_FMT_HDV_TS, 0, 0, &i1080, &p720) == PIN_FMTCHG_NEW_FILE, "hdv");
    CHECK(pin_format_change_action(PIN_FMT_HDV_TS, 0, 0, &i1080, &i1080b) == PIN_FMTCHG_KEEP, "hdv same");
    /* DV <-> HDV: a new file, a pipe stops */
    CHECK(pin_format_change_action(PIN_FMT_DV_RAW, 0, 0, &pal, &i1080) == PIN_FMTCHG_NEW_FILE, "kind");
    CHECK(pin_format_change_action(PIN_FMT_HDV_TS, 0, 0, &i1080, &pal) == PIN_FMTCHG_NEW_FILE, "kind");
    CHECK(pin_format_change_action(PIN_FMT_DV_RAW, 1, 0, &pal, &i1080) == PIN_FMTCHG_STOP, "pipe kind");
    /* a pipe keeps going otherwise */
    CHECK(pin_format_change_action(PIN_FMT_DV_RAW, 1, 0, &pal, &ntsc) == PIN_FMTCHG_KEEP, "pipe std");
    CHECK(pin_format_change_action(PIN_FMT_HDV_TS, 1, 0, &i1080, &p720) == PIN_FMTCHG_KEEP, "pipe hdv");

    char t[160];
    pin_format_change_stop_text(PIN_KIND_DV, PIN_KIND_HDV, t, sizeof(t));
    CHECK(strcmp(t, "the input changed from DV to HDV; a pipe carries one format only") == 0, "stop text");
}

int main(void)
{
    test_dv_cooldown();
    test_kind_and_hdv();
    test_action();
    if (g_failures) {
        printf("%d failure(s)\n", g_failures);
        return 1;
    }
    printf("OK\n");
    return 0;
}
