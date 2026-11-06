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

#include "pin_scene.h"
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

static pin_scene_record_t mk_tc(long idx, int h, int m, int s, int f, int drop)
{
    pin_scene_record_t r;
    memset(&r, 0, sizeof(r));
    r.frame_index = idx;
    r.tc_valid = 1;
    r.tc_hour = h; r.tc_minute = m; r.tc_second = s; r.tc_frame = f;
    r.tc_drop_frame = drop;
    return r;
}

static void add_date(pin_scene_record_t *r, int d, int mo, int y, int h, int mi, int s)
{
    r->date_valid = 1;
    r->date_day = d; r->date_month = mo; r->date_year = y;
    r->time_valid = 1;
    r->time_hour = h; r->time_minute = mi; r->time_second = s;
}

/* Advances an NTSC drop-frame timecode by exactly 1 real frame (skipping
 * frame numbers 00/01 at the top of a non-tens minute), so tests can build
 * a genuinely continuous drop-frame run without re-deriving the rule. */
static void tc_step_df30(int *h, int *m, int *s, int *f)
{
    (*f)++;
    if (*f >= 30) {
        *f = 0;
        (*s)++;
        if (*s >= 60) {
            *s = 0;
            (*m)++;
            if (*m >= 60) { *m = 0; (*h)++; if (*h >= 24) *h = 0; }
            if (*m % 10 != 0)
                *f = 2; /* drop frames 0 and 1 */
        }
    }
}

static void feed_and_expect_no_cut(pin_scene_detector_t *d, const pin_scene_record_t *r, const char *msg)
{
    long cut;
    int hit = pin_scene_feed(d, r, &cut);
    CHECK(!hit, msg);
}

static void test_clean_run(void)
{
    pin_scene_config_t cfg;
    pin_scene_config_defaults(&cfg, 25.0); /* PAL, no drop-frame */
    pin_scene_detector_t d;
    pin_scene_init(&d, &cfg);

    long idx = 0;
    for (int s = 0; s < 20; s++) {
        pin_scene_record_t r = mk_tc(idx++, 0, 0, s, 0, 0);
        feed_and_expect_no_cut(&d, &r, "clean continuous run must not cut");
    }
}

static void test_single_bad_frame_absorbed(void)
{
    pin_scene_config_t cfg;
    pin_scene_config_defaults(&cfg, 25.0);
    pin_scene_detector_t d;
    pin_scene_init(&d, &cfg);

    long idx = 0;
    for (int s = 0; s < 5; s++) {
        pin_scene_record_t r = mk_tc(idx++, 0, 0, s, 0, 0);
        feed_and_expect_no_cut(&d, &r, "warm-up frames");
    }
    /* One garbage frame, unrelated to the run. */
    pin_scene_record_t bad = mk_tc(idx++, 12, 34, 56, 10, 0);
    long cut;
    CHECK(pin_scene_feed(&d, &bad, &cut) == 0, "a single glitch never confirms immediately");

    /* Real tape resumes close to where it left off (within the 1-second
     * jump threshold of the last good frame, 0:00:04:00). */
    pin_scene_record_t resume = mk_tc(idx++, 0, 0, 4, 2, 0);
    CHECK(pin_scene_feed(&d, &resume, &cut) == 0, "resuming the old run absorbs the glitch, no cut");

    /* And the run should keep going normally afterwards. */
    pin_scene_record_t next = mk_tc(idx++, 0, 0, 4, 3, 0);
    feed_and_expect_no_cut(&d, &next, "continues normally after the absorbed glitch");
}

static void test_two_bad_frames_absorbed(void)
{
    pin_scene_config_t cfg;
    pin_scene_config_defaults(&cfg, 25.0);
    pin_scene_detector_t d;
    pin_scene_init(&d, &cfg);

    long idx = 0;
    for (int s = 0; s < 5; s++) {
        pin_scene_record_t r = mk_tc(idx++, 0, 0, s, 0, 0);
        feed_and_expect_no_cut(&d, &r, "warm-up frames");
    }
    long cut;
    pin_scene_record_t bad1 = mk_tc(idx++, 12, 0, 0, 0, 0);
    CHECK(pin_scene_feed(&d, &bad1, &cut) == 0, "first glitch frame");
    pin_scene_record_t bad2 = mk_tc(idx++, 18, 0, 0, 0, 0);
    CHECK(pin_scene_feed(&d, &bad2, &cut) == 0, "second, differently-wrong glitch frame");

    /* Real tape resumes close to the last good frame (0:00:04:00), well
     * within the jump threshold. */
    pin_scene_record_t resume = mk_tc(idx++, 0, 0, 4, 3, 0);
    CHECK(pin_scene_feed(&d, &resume, &cut) == 0, "two glitches in a row are still absorbed");
}

static void test_real_scene_change_confirms(void)
{
    pin_scene_config_t cfg;
    pin_scene_config_defaults(&cfg, 25.0); /* debounce_frames = 5 */
    pin_scene_detector_t d;
    pin_scene_init(&d, &cfg);

    long idx = 0;
    for (int s = 0; s < 5; s++) {
        pin_scene_record_t r = mk_tc(idx++, 0, 0, s, 0, 0);
        feed_and_expect_no_cut(&d, &r, "warm-up frames");
    }

    long first_anomalous = idx;
    long cut = -1;
    int confirmed = 0;
    /* A genuinely new, internally consistent run starting at time 5:00:00. */
    for (int s = 0; s < (int)cfg.debounce_frames && !confirmed; s++) {
        pin_scene_record_t r = mk_tc(idx++, 5, 0, 0 + s, 0, 0);
        confirmed = pin_scene_feed(&d, &r, &cut);
    }
    CHECK(confirmed, "a persistent new run must confirm a cut");
    CHECK(cut == first_anomalous, "cut is back-dated to the first anomalous frame");
}

static void test_tc_backward_confirms(void)
{
    pin_scene_config_t cfg;
    pin_scene_config_defaults(&cfg, 25.0);
    pin_scene_detector_t d;
    pin_scene_init(&d, &cfg);

    long idx = 0;
    for (int s = 10; s < 15; s++) {
        pin_scene_record_t r = mk_tc(idx++, 0, 1, s, 0, 0);
        feed_and_expect_no_cut(&d, &r, "warm-up frames");
    }
    long first_anomalous = idx;
    long cut = -1;
    int confirmed = 0;
    for (int s = 0; s < (int)cfg.debounce_frames && !confirmed; s++) {
        pin_scene_record_t r = mk_tc(idx++, 0, 0, s, 0, 0); /* earlier than 0:01:xx */
        confirmed = pin_scene_feed(&d, &r, &cut);
    }
    CHECK(confirmed, "timecode running backward and staying there must confirm a cut");
    CHECK(cut == first_anomalous, "cut back-dated correctly");
}

static void test_midnight_wrap_no_cut(void)
{
    pin_scene_config_t cfg;
    pin_scene_config_defaults(&cfg, 25.0);
    pin_scene_detector_t d;
    pin_scene_init(&d, &cfg);

    long idx = 0;
    pin_scene_record_t before = mk_tc(idx++, 23, 59, 59, 24, 0); /* last frame of the day, PAL 25fps */
    feed_and_expect_no_cut(&d, &before, "warm-up frame before midnight");
    pin_scene_record_t after = mk_tc(idx++, 0, 0, 0, 0, 0); /* wraps to the next day */
    feed_and_expect_no_cut(&d, &after, "midnight wrap must not look like a backward jump");
}

static void test_ntsc_drop_frame_continuity_no_cut(void)
{
    pin_scene_config_t cfg;
    pin_scene_config_defaults(&cfg, 29.97);
    pin_scene_detector_t d;
    pin_scene_init(&d, &cfg);

    int h = 0, m = 9, s = 59, f = 28; /* about to cross into minute 10 (a multiple of ten: no drop) */
    long idx = 0;
    pin_scene_record_t r0 = mk_tc(idx++, h, m, s, f, 1);
    feed_and_expect_no_cut(&d, &r0, "warm-up");
    for (int i = 0; i < 10; i++) {
        tc_step_df30(&h, &m, &s, &f);
        pin_scene_record_t r = mk_tc(idx++, h, m, s, f, 1);
        feed_and_expect_no_cut(&d, &r, "drop-frame continuity across a minute boundary must not cut");
    }

    /* Now cross a boundary where frames ARE dropped (minute 10 -> 11), with
     * its own fresh detector: reusing `d` would make this jump-cut, from
     * wherever the first loop left off, to 0:10:59:28 look like a real
     * anomaly instead of testing what it's meant to test. */
    pin_scene_detector_t d2;
    pin_scene_init(&d2, &cfg);
    h = 0; m = 10; s = 59; f = 28;
    pin_scene_record_t r1 = mk_tc(idx++, h, m, s, f, 1);
    feed_and_expect_no_cut(&d2, &r1, "warm-up 2");
    for (int i = 0; i < 5; i++) {
        tc_step_df30(&h, &m, &s, &f);
        pin_scene_record_t r = mk_tc(idx++, h, m, s, f, 1);
        feed_and_expect_no_cut(&d2, &r, "drop-frame continuity across a drop boundary must not cut");
    }
}

static void test_date_gap_with_continuous_tc_confirms(void)
{
    /* Some camcorders keep timecode running continuously across a
     * rec-pause; only the recorded date/time shows the break. */
    pin_scene_config_t cfg;
    pin_scene_config_defaults(&cfg, 25.0);
    pin_scene_detector_t d;
    pin_scene_init(&d, &cfg);

    long idx = 0;
    for (int s = 0; s < 5; s++) {
        pin_scene_record_t r = mk_tc(idx++, 0, 0, s, 0, 0);
        add_date(&r, 1, 6, 2024, 10, 0, s);
        feed_and_expect_no_cut(&d, &r, "warm-up frames with date");
    }

    long first_anomalous = idx;
    long cut = -1;
    int confirmed = 0;
    for (int i = 0; i < (int)cfg.debounce_frames && !confirmed; i++) {
        /* Timecode keeps counting up normally... */
        pin_scene_record_t r = mk_tc(idx++, 0, 0, 5 + i, 0, 0);
        /* ...but the recorded date/time jumped by an hour: a new shot. */
        add_date(&r, 1, 6, 2024, 11, 0, i);
        confirmed = pin_scene_feed(&d, &r, &cut);
    }
    CHECK(confirmed, "a persistent date/time gap must confirm a cut even with continuous TC");
    CHECK(cut == first_anomalous, "cut back-dated to the first date-gap frame");
}

static void test_invalid_tc_stretch_is_neutral(void)
{
    pin_scene_config_t cfg;
    pin_scene_config_defaults(&cfg, 25.0);
    pin_scene_detector_t d;
    pin_scene_init(&d, &cfg);

    long idx = 0;
    for (int s = 0; s < 5; s++) {
        pin_scene_record_t r = mk_tc(idx++, 0, 0, s, 0, 0);
        feed_and_expect_no_cut(&d, &r, "warm-up frames");
    }
    for (int i = 0; i < 20; i++) {
        pin_scene_record_t r;
        memset(&r, 0, sizeof(r));
        r.frame_index = idx++; /* everything invalid: dropout / no lock */
        feed_and_expect_no_cut(&d, &r, "a long invalid-TC stretch must never cut on its own");
    }
    /* Resuming close to where the real tape would be (well within the
     * jump threshold of the last *valid* frame, 0:00:04:00) must be seen
     * as continuous, proving the invalid stretch never silently mutated
     * the baseline or by itself counted as a jump. */
    pin_scene_record_t resume = mk_tc(idx++, 0, 0, 4, 20, 0);
    feed_and_expect_no_cut(&d, &resume, "resuming after an invalid stretch is continuous");
}

/* ---- HDV: one record per picture, a timecode only on the GOP's first one ---- */

/* Feeds n pictures of 12-picture GOPs at 25 fps, starting at picture *idx with
 * the timecode tc0 (in frames) on the first; gop_tc(g) can override GOP g's
 * timecode in frames (-1 = no GOP header found, -2 = the run's own). Returns
 * 1 on a cut (its index in *cut). */
static long tc_frames_of(int h, int m, int s, int f) { return ((h * 60L + m) * 60 + s) * 25 + f; }

static pin_scene_record_t mk_frames(long idx, long frames)
{
    return mk_tc(idx, (int)(frames / 90000), (int)(frames / 1500 % 60), (int)(frames / 25 % 60),
                 (int)(frames % 25), 0);
}

static int feed_gops(pin_scene_detector_t *d, long *idx, long tc0, int gops, const long *gop_tc,
                     long *cut)
{
    for (int g = 0; g < gops; g++) {
        for (int p = 0; p < 12; p++) {
            pin_scene_record_t r;
            memset(&r, 0, sizeof(r));
            r.frame_index = *idx;
            long tc = gop_tc && gop_tc[g] != -2 ? gop_tc[g] : tc0 + g * 12;
            if (p == 0 && tc >= 0)
                r = mk_frames(*idx, tc);
            (*idx)++;
            if (pin_scene_feed(d, &r, cut))
                return 1;
        }
    }
    return 0;
}

static void test_hdv_config(void)
{
    pin_scene_config_t cfg;
    pin_scene_config_hdv(&cfg, 25.0);
    CHECK(cfg.debounce_frames == 3 && cfg.sparse_tc == 1, "HDV: 3 GOPs, sparse timecode");
    CHECK(cfg.tc_jump_seconds == 1.0 && cfg.fps == 25.0, "HDV: the defaults otherwise");
    pin_scene_config_defaults(&cfg, 25.0);
    CHECK(cfg.debounce_frames == 5 && cfg.sparse_tc == 0, "DV defaults unchanged");
}

static void test_hdv_clean_run(void)
{
    pin_scene_config_t cfg;
    pin_scene_config_hdv(&cfg, 25.0);
    pin_scene_detector_t d;
    pin_scene_init(&d, &cfg);
    long idx = 0, cut;
    CHECK(!feed_gops(&d, &idx, tc_frames_of(1, 0, 0, 0), 20, NULL, &cut), "HDV: clean run, no cut");
    CHECK(!pin_scene_pending(&d, NULL), "HDV: nothing pending in a clean run");
}

/* the user's cool-down: one GOP with a wrong timecode never splits */
static void test_hdv_stray_gops_absorbed(void)
{
    pin_scene_config_t cfg;
    pin_scene_config_hdv(&cfg, 25.0);
    long t0 = tc_frames_of(1, 0, 0, 0), bad = tc_frames_of(5, 0, 0, 0), cut;

    /* one stray GOP */
    pin_scene_detector_t d;
    pin_scene_init(&d, &cfg);
    long idx = 0;
    long one[8] = { -2, -2, -2, bad, -2, -2, -2, -2 };
    CHECK(!feed_gops(&d, &idx, t0, 8, one, &cut), "HDV: one stray GOP never splits");
    CHECK(!pin_scene_pending(&d, NULL), "HDV: absorbed, nothing pending");

    /* two stray GOPs, each wrong in its own way */
    pin_scene_init(&d, &cfg);
    idx = 0;
    long two[8] = { -2, -2, -2, bad, tc_frames_of(9, 9, 9, 9), -2, -2, -2 };
    CHECK(!feed_gops(&d, &idx, t0, 8, two, &cut), "HDV: two stray GOPs never split");

    /* GOP headers lost for 3 GOPs (1.4 s): the next one is where the timecode should be */
    pin_scene_init(&d, &cfg);
    idx = 0;
    long lost[8] = { -2, -2, -1, -1, -1, -2, -2, -2 };
    CHECK(!feed_gops(&d, &idx, t0, 8, lost, &cut), "HDV: lost GOP headers never split");

    /* without sparse_tc the same loss would have split (the next GOP is 48 frames on) */
    pin_scene_config_t plain = cfg;
    plain.sparse_tc = 0;
    pin_scene_init(&d, &plain);
    idx = 0;
    CHECK(feed_gops(&d, &idx, t0, 8, lost, &cut), "plain: the same loss reads as a jump");
}

/* a real jump: confirmed at the third GOP, back-dated to the first */
static void test_hdv_jump_confirms(void)
{
    pin_scene_config_t cfg;
    pin_scene_config_hdv(&cfg, 25.0);
    pin_scene_detector_t d;
    pin_scene_init(&d, &cfg);
    long idx = 0, cut = -1, start = -1;
    CHECK(!feed_gops(&d, &idx, tc_frames_of(1, 0, 0, 0), 4, NULL, &cut), "HDV: first scene");
    long first_new = idx; /* 48 */
    long tc = tc_frames_of(5, 0, 0, 0);
    int confirmed = 0;
    for (int g = 0; g < 3 && !confirmed; g++) {
        confirmed = feed_gops(&d, &idx, tc + g * 12, 1, NULL, &cut);
        if (g < 2)
            CHECK(!confirmed && pin_scene_pending(&d, &start) && start == first_new,
                  "HDV: pending from the first new GOP until confirmed");
    }
    CHECK(confirmed, "HDV: a real jump confirms within 3 GOPs");
    CHECK(cut == first_new, "HDV: cut at the first picture of the first new GOP");
    CHECK(idx == first_new + 24 + 1, "HDV: confirmed at the third GOP's first picture");
    CHECK(!pin_scene_pending(&d, NULL), "HDV: nothing pending after the cut");

    /* backward, even by less than a second: a break too */
    pin_scene_init(&d, &cfg);
    idx = 0;
    CHECK(!feed_gops(&d, &idx, tc_frames_of(1, 0, 10, 0), 4, NULL, &cut), "HDV: first scene");
    first_new = idx;
    CHECK(feed_gops(&d, &idx, tc_frames_of(1, 0, 10, 0) + 48 - 20, 4, NULL, &cut) && cut == first_new,
          "HDV: a timecode 20 frames back splits at the GOP");
}

/* a camera that writes 00:00:00:00 in every GOP header (3.9): never a cut, as before,
 * even after a stretch without GOP headers */
static void test_hdv_zero_tc_never_cuts(void)
{
    pin_scene_config_t cfg;
    pin_scene_config_hdv(&cfg, 25.0);
    pin_scene_detector_t d;
    pin_scene_init(&d, &cfg);
    long idx = 0, cut;
    long zeros[12];
    for (int g = 0; g < 12; g++)
        zeros[g] = g >= 4 && g < 8 ? -1 : 0;
    CHECK(!feed_gops(&d, &idx, 0, 12, zeros, &cut), "HDV: zero timecode never cuts");
}

/* HDV's camera clock (the video PES DTS, on each GOP-opening picture next to the timecode): a
 * jump of it splits even where the timecode runs on (a camera that pauses and resumes), a dropout
 * that lost pictures does not, and neither does the 33-bit wrap. 12-picture GOPs at 25 fps. */
static int feed_clock(pin_scene_detector_t *d, long *idx, long tc, int64_t *ts, int gops, long *cut)
{
    int fired = 0;
    for (int g = 0; g < gops; g++) {
        for (int p = 0; p < 12; p++) {
            pin_scene_record_t r;
            memset(&r, 0, sizeof(r));
            r.frame_index = (*idx)++;
            if (p == 0) {
                r.tc_valid = 1;
                r.tc_hour = (int)(tc / (25 * 3600));
                r.tc_minute = (int)(tc / (25 * 60) % 60);
                r.tc_second = (int)(tc / 25 % 60);
                r.tc_frame = (int)(tc % 25);
                r.ts_valid = 1;
                r.ts = *ts & ((1LL << 33) - 1);
            }
            if (pin_scene_feed(d, &r, cut))
                fired = 1;
        }
        tc += 12;
        *ts += 12 * 3600;
    }
    return fired;
}

static void test_hdv_clock(void)
{
    pin_scene_config_t cfg;
    pin_scene_config_hdv(&cfg, 25.0);
    pin_scene_detector_t d;
    long idx, cut = -1, tc = tc_frames_of(1, 0, 0, 0);
    int64_t ts;

    /* a clean run across the 33-bit wrap: no cut */
    pin_scene_init(&d, &cfg);
    idx = 0;
    ts = (1LL << 33) - 10 * 12 * 3600;
    CHECK(!feed_clock(&d, &idx, tc, &ts, 20, &cut), "clock: the 33-bit wrap is no jump");

    /* the clock jumps 10 s ahead, the timecode runs on: a cut at the first GOP after it */
    pin_scene_init(&d, &cfg);
    idx = 0;
    ts = 834LL * 90000;
    CHECK(!feed_clock(&d, &idx, tc, &ts, 4, &cut), "clock: first recording");
    long first_new = idx;
    ts += 10LL * 90000;
    CHECK(feed_clock(&d, &idx, tc + 48, &ts, 4, &cut) && cut == first_new,
          "clock: a jump with the timecode running on splits at the GOP");

    /* back by a second: a cut too */
    pin_scene_init(&d, &cfg);
    idx = 0;
    ts = 834LL * 90000;
    feed_clock(&d, &idx, tc, &ts, 4, &cut);
    first_new = idx;
    ts -= 2LL * 90000;
    CHECK(feed_clock(&d, &idx, tc + 48, &ts, 4, &cut) && cut == first_new, "clock: backward splits");

    /* a dropout: 0.6 s of pictures never arrived, clock and timecode moved on by them: no cut */
    pin_scene_init(&d, &cfg);
    idx = 0;
    ts = 834LL * 90000;
    feed_clock(&d, &idx, tc, &ts, 4, &cut);
    ts += 15 * 3600;
    CHECK(!feed_clock(&d, &idx, tc + 48 + 15, &ts, 6, &cut), "clock: a dropout that lost pictures");

    /* a single GOP with a garbled clock: absorbed by the debounce */
    pin_scene_init(&d, &cfg);
    idx = 0;
    ts = 834LL * 90000;
    feed_clock(&d, &idx, tc, &ts, 4, &cut);
    int64_t bad = ts + 500LL * 90000;
    CHECK(!feed_clock(&d, &idx, tc + 48, &bad, 1, &cut), "clock: one garbled GOP");
    ts += 12 * 3600;
    CHECK(!feed_clock(&d, &idx, tc + 60, &ts, 6, &cut), "clock: back on the run, no cut");
}

int main(void)
{
    test_clean_run();
    test_single_bad_frame_absorbed();
    test_two_bad_frames_absorbed();
    test_real_scene_change_confirms();
    test_tc_backward_confirms();
    test_midnight_wrap_no_cut();
    test_ntsc_drop_frame_continuity_no_cut();
    test_date_gap_with_continuous_tc_confirms();
    test_invalid_tc_stretch_is_neutral();
    test_hdv_config();
    test_hdv_clean_run();
    test_hdv_stray_gops_absorbed();
    test_hdv_jump_confirms();
    test_hdv_zero_tc_never_cuts();
    test_hdv_clock();

    if (g_failures == 0) {
        printf("test_pin_scene: all tests passed\n");
        return 0;
    }
    printf("test_pin_scene: %d failure(s)\n", g_failures);
    return g_failures;
}
