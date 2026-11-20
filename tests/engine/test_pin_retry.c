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

/* pin_retry against a simulated tape and deck: a tape of numbered DV frames / HDV pictures
 * with damage that goes away on a re-read (dirt) or stays (a crease), frames lost in transit,
 * a deck that winds at some speed and may or may not tell its timecode while winding. The
 * output must hold every frame of the tape exactly once, in order, as clean as the reads allow. */

#include "pin_retry.h"
#include "dv_error.h"
#include "dv_subcode.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);             \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

#define FPS 25.0
#define NSEQ 12
#define FRAME_LEN (NSEQ * DV_SEQ_SIZE)
#define KEY_BASE 90000   /* timecode 01:00:00:00 at 25 fps */
#define GOP 12
#define VPID 0x810

typedef struct {
    int n;
    int hdv;
    unsigned char *perm;    /* damaged on every read */
    int *trans;             /* damaged on the first trans[k] reads */
    unsigned char *lost1;   /* lost in transit on the first read */
    unsigned char *notc;    /* timecode unreadable when damaged */
    int *reads;
    double rate;            /* wind speed, x real time */
    int tc_wind;            /* TIME CODE answers while winding */
    int ignore_rew;         /* the deck refuses REW */
    int stale;              /* after PLAY the deck first sends the last frame it showed again */
} tape_t;

typedef struct {
    int idx[20000];
    int bad[20000];
    int n;
    int cc_breaks;
    pin_retry_cc_fix_t cc;
    int last_cc;
} out_t;

static tape_t *tape_new(int n, int hdv)
{
    tape_t *t = calloc(1, sizeof(*t));
    t->n = n;
    t->hdv = hdv;
    t->perm = calloc(n, 1);
    t->trans = calloc(n, sizeof(int));
    t->lost1 = calloc(n, 1);
    t->notc = calloc(n, 1);
    t->reads = calloc(n, sizeof(int));
    t->rate = 10;
    t->tc_wind = 1;
    return t;
}

static void tape_free(tape_t *t)
{
    free(t->perm); free(t->trans); free(t->lost1); free(t->notc); free(t->reads);
    free(t);
}

/* A clean DV frame numbered k (in every video block's payload). */
static void make_dv(uint8_t *f, int k, int damaged)
{
    memset(f, 0, FRAME_LEN);
    for (unsigned si = 0; si < NSEQ; si++) {
        unsigned vdbn = 0, adbn = 0;
        for (unsigned b = 0; b < 150; b++) {
            uint8_t *p = f + si * DV_SEQ_SIZE + b * 80;
            unsigned sct, dbn;
            if (b == 0) { sct = 0; dbn = 0; }
            else if (b < 3) { sct = 1; dbn = b - 1; }
            else if (b < 6) { sct = 2; dbn = b - 3; }
            else if ((b - 6) % 16 == 0) { sct = 3; dbn = adbn++; }
            else { sct = 4; dbn = vdbn++; }
            p[0] = (uint8_t)((sct << 5) | 0x1F);
            p[1] = (uint8_t)((si << 4) | 7);
            p[2] = (uint8_t)dbn;
            if (sct == 3)
                for (int i = 0; i < 72; i++)
                    p[8 + i] = (uint8_t)(k * 7 + si * 13 + dbn * 5 + i);
            if (sct == 4) {
                p[4] = (uint8_t)(k >> 24); p[5] = (uint8_t)(k >> 16);
                p[6] = (uint8_t)(k >> 8); p[7] = (uint8_t)k;
                p[8] = (uint8_t)(si * 31 + dbn);
                if (damaged && (dbn % 9 == si % 9))
                    p[3] = 0x70;    /* STA 7: an error nobody concealed */
            }
        }
    }
}

static int dv_index(const uint8_t *f)
{
    const uint8_t *p = f + 7 * 80;  /* block 7: the first video block */
    return (p[4] << 24) | (p[5] << 16) | (p[6] << 8) | p[7];
}

static int dv_damaged(const uint8_t *f, size_t len)
{
    dv_frame_errors_t e;
    dv_error_analyze(f, len, &e);
    return e.frame_error;
}

static void on_release(void *user, uint8_t *data, size_t len, const void *meta, unsigned read, int mended)
{
    out_t *o = user;
    (void)mended;
    int k = *(const int *)meta;
    if (len == FRAME_LEN) {
        CHECK(dv_index(data) == k, "the released frame is the one its descriptor names");
        o->idx[o->n] = k;
        o->bad[o->n] = dv_damaged(data, len);
    } else {
        pin_retry_cc_fix_apply(&o->cc, data, len / 188, read);
        int cc = data[3] & 15;
        if (o->last_cc >= 0 && cc != ((o->last_cc + 1) & 15))
            o->cc_breaks++;
        o->last_cc = cc;
        o->idx[o->n] = k;
        o->bad[o->n] = data[4] == 0xEE;
    }
    o->n++;
}

/* A frame on its way from the deck to the engine (the bus, USB and the reassembler take time). */
typedef struct {
    double at;
    int k, dam, cc, lost;
} inflight_t;

#define DELIVERY_S 0.3      /* frames reach the engine this long after the deck sent them */
#define WIND_DELAY_S 0.5    /* after REW the deck plays on this long before it winds (MarvinPlayout) */
#define PLAY_DELAY_S 1.0    /* after PLAY from winding, the first frame comes this much later */

typedef struct {
    int state;          /* 0 stop, 1 play, 2 rew */
    int want;           /* the commanded state (takes effect at switch_at) */
    int last_k;         /* the last frame sent */
    double switch_at;
    double p;           /* position, frames */
    double rew_t;
    int next;           /* next frame to emit while playing */
    double last_emit;
    int cc;             /* continuity counter of the running play */
    int at_start;
} deck_t;

/* Runs the whole tape through the engine. Returns the engine's stats. */
static void run(tape_t *t, const pin_retry_cfg_t *cfg, out_t *o, pin_retry_stats_t *st)
{
    memset(o, 0, sizeof(*o));
    o->last_cc = -1;
    pin_retry_cc_fix_init(&o->cc);
    pin_retry_t *r = pin_retry_new(cfg, on_release, o);
    deck_t d;
    memset(&d, 0, sizeof(d));
    d.state = 1;
    d.cc = 3;
    uint8_t *frame = malloc(FRAME_LEN);
    double dt = 1.0 / FPS / 4;
    int failed = 0;
    double t_end = 0;
    static inflight_t q[4096];
    int qh = 0, qt = 0;
    d.want = 1;
    for (double now = 0; now < 2000; now += dt) {
        if (d.want != d.state && now >= d.switch_at) {
            d.state = d.want;
            if (d.state == 1)
                d.next = (int)ceil(d.p);
        }
        if (d.state == 1) {
            d.p += FPS * dt;
            while (d.next <= (int)floor(d.p)) {
                int k = d.next++;
                if (k < 0 || k >= t->n)
                    continue;
                t->reads[k]++;
                inflight_t *f = &q[qt++ % 4096];
                f->at = now + DELIVERY_S;
                d.last_k = k;
                f->k = k;
                f->dam = t->perm[k] || t->reads[k] <= t->trans[k];
                f->lost = t->lost1[k] && t->reads[k] == 1;
                f->cc = d.cc;
                d.cc += 1;
            }
            if (d.p > t->n + 2 * FPS && !t_end)
                t_end = now;
        } else if (d.state == 2) {
            d.p -= t->rate * FPS * dt;
            if (d.p <= 0) {
                d.p = 0;
                d.state = d.want = 0;
                d.at_start = 1;
            }
        }
        while (qh < qt && q[qh % 4096].at <= now) {
            inflight_t *f = &q[qh++ % 4096];
            int k = f->k, dam = f->dam;
            if (f->lost) {
                pin_retry_lost(r, 1);
                continue;
            }
            {
                d.last_emit = now;
                pin_retry_unit_t u;
                memset(&u, 0, sizeof(u));
                u.fps = FPS;
                u.meta = &k;
                u.meta_len = sizeof(k);
                if (!t->hdv) {
                    make_dv(frame, k, dam);
                    u.data = frame;
                    u.len = FRAME_LEN;
                    u.key_valid = !(dam && t->notc[k]);
                    u.key = KEY_BASE + k;
                } else {
                    memset(frame, 0xFF, 188);
                    frame[0] = 0x47;
                    frame[1] = (uint8_t)(0x40 | (VPID >> 8));
                    frame[2] = (uint8_t)(VPID & 0xFF);
                    frame[3] = (uint8_t)(0x10 | (f->cc & 15));
                    frame[4] = dam ? 0xEE : 0x00;
                    u.hdv = 1;
                    u.data = frame;
                    u.len = 188;
                    u.picture = 1;
                    u.opens_gop = k % GOP == 0;
                    u.damaged = dam;
                    u.key_valid = u.opens_gop && !(dam && t->notc[k]);
                    u.key = (int64_t)k * 3600;
                    u.dts_valid = !(dam && t->notc[k]);
                    u.dts = (int64_t)k * 3600;
                }
                pin_retry_push(r, now, &u);
            }
        }
        pin_retry_deck_t dk;
        memset(&dk, 0, sizeof(dk));
        dk.signal = now - d.last_emit < 1.0 && d.state == 1;
        dk.can_send = 1;
        dk.at_start = d.at_start;
        dk.cmd_failed = failed;
        failed = 0;
        if (d.state != 2 || t->tc_wind || d.want != 2) {
            dk.tc_valid = 1;
            dk.tc_frames = KEY_BASE + (int64_t)floor(d.p);
        }
        pin_retry_action_t a = pin_retry_tick(r, now, &dk);
        if (a == PIN_RETRY_ACT_REW && t->ignore_rew) {
            failed = 1;
        } else if (a == PIN_RETRY_ACT_REW) {
            /* plays on for a moment, then winds */
            d.want = 2;
            d.switch_at = now + WIND_DELAY_S;
            d.rew_t = now;
            d.at_start = 0;
        } else if (a == PIN_RETRY_ACT_PLAY) {
            /* brakes at once, the picture comes later */
            if (d.state == 2)
                d.state = 0;
            d.want = 1;
            d.switch_at = now + (d.state == 1 ? 0 : PLAY_DELAY_S);
            if (t->stale && d.state != 1) {
                inflight_t *f = &q[qt++ % 4096];
                f->at = d.switch_at;
                f->k = d.last_k;
                f->dam = 0;
                f->lost = 0;
                f->cc = d.cc;
            }
            d.cc = (d.cc * 7 + 5) & 15;   /* a new run starts anywhere */
        } else if (a == PIN_RETRY_ACT_STOP) {
            d.state = d.want = 0;
        }
        if (t_end && now - t_end > 10 && !pin_retry_busy(r) && qh == qt) {
            pin_retry_stats_t ps;
            pin_retry_get_stats(r, &ps);
            if (ps.phase == PIN_RETRY_IDLE || now - t_end > 40)
                break;
        }
    }
    pin_retry_flush(r);
    pin_retry_get_stats(r, st);
    pin_retry_free(r);
    free(frame);
}

/* every frame once, in order; returns the damaged ones */
static int check_complete(const tape_t *t, const out_t *o, const char *what)
{
    int ok = o->n == t->n;
    for (int i = 0; ok && i < o->n; i++)
        if (o->idx[i] != i) ok = 0;
    if (!ok) {
        printf("FAIL %s: output not the tape in order (%d of %d units)\n", what, o->n, t->n);
        for (int i = 1; i < o->n; i++)
            if (o->idx[i] != o->idx[i - 1] + 1) {
                printf("  first break at %d: %d -> %d\n", i, o->idx[i - 1], o->idx[i]);
                break;
            }
        g_failures++;
    }
    int bad = 0;
    for (int i = 0; i < o->n; i++)
        bad += o->bad[i];
    return bad;
}

static void cfg_def(pin_retry_cfg_t *c)
{
    pin_retry_cfg_defaults(c);
}

static out_t g_out;

static void test_clean(void)
{
    tape_t *t = tape_new(400, 0);
    pin_retry_cfg_t c; cfg_def(&c);
    pin_retry_stats_t st;
    run(t, &c, &g_out, &st);
    CHECK(check_complete(t, &g_out, "clean") == 0, "clean tape: no damage");
    CHECK(st.rewinds == 0, "clean tape: no rewind");
    tape_free(t);
}

static void test_transient(void)
{
    tape_t *t = tape_new(600, 0);
    for (int k = 200; k < 205; k++) t->trans[k] = 1;
    pin_retry_cfg_t c; cfg_def(&c);
    pin_retry_stats_t st;
    run(t, &c, &g_out, &st);
    CHECK(check_complete(t, &g_out, "transient") == 0, "dirt: mended by the re-read");
    CHECK(st.sections == 1 && st.sections_fixed == 1, "dirt: one section, fixed");
    CHECK(st.rewinds == 1, "dirt: one rewind");
    tape_free(t);
}

static void test_cluster(void)
{
    /* the first spot is retried at once; the next one comes soon after: the join gap grows and
     * the third joins the second's section */
    tape_t *t = tape_new(900, 0);
    for (int k = 200; k < 203; k++) t->trans[k] = 1;
    for (int k = 240; k < 243; k++) t->trans[k] = 1;
    for (int k = 280; k < 282; k++) t->trans[k] = 1;
    pin_retry_cfg_t c; cfg_def(&c);
    pin_retry_stats_t st;
    run(t, &c, &g_out, &st);
    CHECK(check_complete(t, &g_out, "cluster") == 0, "cluster: mended");
    printf("cluster: %llu sections, %llu rewinds, join %.1f s\n", (unsigned long long)st.sections,
           (unsigned long long)st.rewinds, st.join_s);
    CHECK(st.sections == 2 && st.rewinds == 2, "cluster: first spot alone, the next two together");
    tape_free(t);
}

static void test_lone_spot_quick(void)
{
    /* one bad spot: re-read at once, a short section (the join gap starts at about a second) */
    tape_t *t = tape_new(900, 0);
    for (int k = 300; k < 302; k++) t->trans[k] = 1;
    pin_retry_cfg_t c; cfg_def(&c);
    pin_retry_stats_t st;
    run(t, &c, &g_out, &st);
    CHECK(check_complete(t, &g_out, "lone") == 0, "lone spot: mended");
    CHECK(st.rewinds == 1 && st.join_s <= 1.01, "lone spot: one rewind, short join gap");
    tape_free(t);
}

static void test_dense_then_sparse(void)
{
    /* a badly damaged minute (a spot every 2 s), then clean tape with two lone spots far apart:
     * the dense stretch takes far fewer rewinds than spots, and the join gap comes back down */
    tape_t *t = tape_new(25 * 400, 0);
    int spots = 0;
    for (int k = 500; k < 500 + 25 * 60; k += 50) { t->trans[k] = 1; spots++; }
    t->trans[25 * 250] = 1;
    t->trans[25 * 370] = 1;
    pin_retry_cfg_t c; cfg_def(&c);
    c.mode = PIN_RETRY_MODE_SECTIONS;
    pin_retry_stats_t st;
    run(t, &c, &g_out, &st);
    CHECK(check_complete(t, &g_out, "dense") == 0, "dense then sparse: mended");
    printf("dense: %d spots, %llu sections, %llu rewinds, join at the end %.1f s\n", spots + 2,
           (unsigned long long)st.sections, (unsigned long long)st.rewinds, st.join_s);
    CHECK(st.rewinds < (uint64_t)spots / 2, "dense: many spots per rewind");
    CHECK(st.join_s <= 2.01, "sparse again: short join gap");
    tape_free(t);
}

static void test_whole_tape(void)
{
    /* WHOLE: one pass to the end, then a re-read from the first bad frame; a temporary file */
    tape_t *t = tape_new(25 * 120, 0);
    for (int k = 300; k < 303; k++) t->trans[k] = 1;
    for (int k = 1500; k < 1502; k++) t->trans[k] = 1;
    for (int k = 2600; k < 2601; k++) t->trans[k] = 2;
    pin_retry_cfg_t c; cfg_def(&c);
    c.mode = PIN_RETRY_MODE_WHOLE;
    pin_retry_stats_t st;
    run(t, &c, &g_out, &st);
    CHECK(check_complete(t, &g_out, "whole") == 0, "whole tape: mended");
    printf("whole: %llu sections, %llu rewinds, whole %d, spool failed %d\n",
           (unsigned long long)st.sections, (unsigned long long)st.rewinds, st.whole, st.spool_failed);
    CHECK(st.whole && !st.spool_failed, "whole tape: held in the temporary file");
    CHECK(st.sections == 1 && st.rewinds == 2, "whole tape: one section, a pass per attempt");
    tape_free(t);
}

static void test_file_storage(void)
{
    /* sections kept in the temporary file: the same result as in RAM */
    tape_t *t = tape_new(900, 0);
    for (int k = 300; k < 306; k++) t->trans[k] = k < 303 ? 1 : 2;
    pin_retry_cfg_t c; cfg_def(&c);
    c.storage = PIN_RETRY_STORE_FILE;
    pin_retry_stats_t st;
    run(t, &c, &g_out, &st);
    CHECK(check_complete(t, &g_out, "file storage") == 0, "file storage: mended");
    CHECK(!st.spool_failed && st.rewinds == 2, "file storage: works like RAM");
    tape_free(t);
}

static void test_auto_turns_whole(void)
{
    /* AUTO on a very poor tape: damage every second for minutes: it turns to the whole tape */
    tape_t *t = tape_new(25 * 400, 0);
    for (int k = 250; k < 25 * 380; k += 25) t->trans[k] = 1;
    pin_retry_cfg_t c; cfg_def(&c);
    c.window_s = 20;
    pin_retry_stats_t st;
    run(t, &c, &g_out, &st);
    CHECK(check_complete(t, &g_out, "auto whole") == 0, "auto: mended");
    printf("auto: %llu sections, %llu rewinds, whole %d\n", (unsigned long long)st.sections,
           (unsigned long long)st.rewinds, st.whole);
    CHECK(st.whole, "auto: a very poor tape is done whole");
    tape_free(t);
}

static void test_apart(void)
{
    tape_t *t = tape_new(2000, 0);
    for (int k = 200; k < 203; k++) t->trans[k] = 1;
    for (int k = 1200; k < 1203; k++) t->trans[k] = 1;  /* 40 s later: separate */
    pin_retry_cfg_t c; cfg_def(&c);
    pin_retry_stats_t st;
    run(t, &c, &g_out, &st);
    CHECK(check_complete(t, &g_out, "apart") == 0, "apart: mended");
    CHECK(st.sections == 2 && st.sections_fixed == 2, "apart: two sections");
    tape_free(t);
}

static void test_permanent(void)
{
    tape_t *t = tape_new(700, 0);
    for (int k = 300; k < 304; k++) t->perm[k] = 1;
    pin_retry_cfg_t c; cfg_def(&c);
    pin_retry_stats_t st;
    run(t, &c, &g_out, &st);
    int bad = check_complete(t, &g_out, "permanent");
    CHECK(bad == 4, "crease: the damage stays");
    CHECK(st.sections_failed == 1, "crease: counted as failed");
    CHECK(st.rewinds == 2, "crease: given up after two attempts that mended nothing");
    tape_free(t);
}

static void test_two_reads(void)
{
    tape_t *t = tape_new(700, 0);
    for (int k = 300; k < 306; k++) t->trans[k] = k < 303 ? 1 : 2;
    pin_retry_cfg_t c; cfg_def(&c);
    pin_retry_stats_t st;
    run(t, &c, &g_out, &st);
    CHECK(check_complete(t, &g_out, "two reads") == 0, "two reads: mended");
    CHECK(st.rewinds == 2 && st.sections_fixed == 1, "two reads: two attempts");
    tape_free(t);
}

static void test_attempt_limit(void)
{
    tape_t *t = tape_new(700, 0);
    for (int k = 300; k < 306; k++) t->trans[k] = k - 299;   /* 1..6 reads to come clean */
    pin_retry_cfg_t c; cfg_def(&c);
    c.attempts = 2;
    pin_retry_stats_t st;
    run(t, &c, &g_out, &st);
    int bad = check_complete(t, &g_out, "attempt limit");
    CHECK(bad == 4, "attempt limit: what three reads could not mend stays");
    CHECK(st.rewinds == 2 && st.sections_partly == 1, "attempt limit: stops after 2 attempts");
    tape_free(t);
}

static void test_lost(void)
{
    tape_t *t = tape_new(600, 0);
    t->lost1[250] = 1;
    t->lost1[251] = 1;
    pin_retry_cfg_t c; cfg_def(&c);
    pin_retry_stats_t st;
    run(t, &c, &g_out, &st);
    CHECK(check_complete(t, &g_out, "lost") == 0, "lost frames: filled in by the re-read");
    CHECK(st.sections_fixed == 1, "lost frames: fixed");
    tape_free(t);
}

static void test_no_timecode(void)
{
    tape_t *t = tape_new(800, 0);
    for (int k = 400; k < 404; k++) { t->trans[k] = 1; t->notc[k] = 1; }
    t->tc_wind = 0;
    t->rate = 20;
    pin_retry_cfg_t c; cfg_def(&c);
    pin_retry_stats_t st;
    run(t, &c, &g_out, &st);
    CHECK(check_complete(t, &g_out, "no tc") == 0, "no timecode while winding: mended");
    CHECK(st.rew_rate > 10, "no timecode while winding: the wind speed was learned");
    tape_free(t);
}

static void test_slow_deck(void)
{
    /* winds slower than the first guess: lands too late, winds again */
    tape_t *t = tape_new(1500, 0);
    for (int k = 500; k < 700; k += 20) t->trans[k] = 1;
    t->tc_wind = 0;
    t->rate = 2;
    pin_retry_cfg_t c; cfg_def(&c);
    pin_retry_stats_t st;
    run(t, &c, &g_out, &st);
    CHECK(check_complete(t, &g_out, "slow deck") == 0, "slow deck: mended");
    printf("slow deck: %llu rewinds, speed %.1f\n", (unsigned long long)st.rewinds, st.rew_rate);
    CHECK(st.rew_rate < 4, "slow deck: the slow wind speed was learned");
    tape_free(t);
}

static void test_stale_frame(void)
{
    /* the deck sends the frame it showed last once more as PLAY starts: no extra wind for it */
    tape_t *t = tape_new(1200, 0);
    for (int k = 400; k < 404; k++) t->trans[k] = 1;
    for (int k = 900; k < 902; k++) t->trans[k] = 1;
    t->stale = 1;
    pin_retry_cfg_t c; cfg_def(&c);
    pin_retry_stats_t st;
    run(t, &c, &g_out, &st);
    CHECK(check_complete(t, &g_out, "stale") == 0, "stale frame after PLAY: mended");
    CHECK(st.rewinds == 2, "stale frame after PLAY: one wind per section");
    tape_free(t);
}

static void test_hdv_stale(void)
{
    /* HDV: a stale picture after PLAY, the deck playing on after REW: GOPs stay whole */
    tape_t *t = tape_new(1500, 1);
    for (int k = 400; k < 404; k++) t->trans[k] = 1;
    for (int k = 1000; k < 1003; k++) t->trans[k] = 2;
    t->stale = 1;
    pin_retry_cfg_t c; cfg_def(&c);
    pin_retry_stats_t st;
    run(t, &c, &g_out, &st);
    CHECK(check_complete(t, &g_out, "hdv stale") == 0, "HDV stale picture: mended, every picture once");
    CHECK(g_out.cc_breaks == 0, "HDV stale picture: continuity counters");
    tape_free(t);
}

static void test_end_of_tape(void)
{
    tape_t *t = tape_new(500, 0);
    for (int k = 494; k < 500; k++) t->trans[k] = 1;
    pin_retry_cfg_t c; cfg_def(&c);
    pin_retry_stats_t st;
    run(t, &c, &g_out, &st);
    CHECK(check_complete(t, &g_out, "end of tape") == 0, "damage at the end: mended");
    tape_free(t);
}

static void test_window(void)
{
    /* damage every 2 s for 100 s: more than the 30 s window holds */
    tape_t *t = tape_new(3500, 0);
    for (int k = 300; k < 2800; k += 50) t->trans[k] = 1;
    pin_retry_cfg_t c; cfg_def(&c);
    c.window_s = 30;
    pin_retry_stats_t st;
    run(t, &c, &g_out, &st);
    CHECK(check_complete(t, &g_out, "window") == 0, "window: mended");
    CHECK(st.sections >= 3, "window: split into window-sized sections");
    tape_free(t);
}

static void test_uncorrected_mode(void)
{
    tape_t *t = tape_new(600, 0);
    for (int k = 200; k < 205; k++) t->trans[k] = 1;
    pin_retry_cfg_t c; cfg_def(&c);
    c.trigger = PIN_RETRY_TRIGGER_UNCORRECTED;
    pin_retry_stats_t st;
    run(t, &c, &g_out, &st);
    CHECK(check_complete(t, &g_out, "uncorrected") == 0, "STA 7 is uncorrected: retried");
    CHECK(st.rewinds == 1, "uncorrected: one rewind");
    tape_free(t);
}

static void test_hdv(void)
{
    tape_t *t = tape_new(1200, 1);
    for (int k = 400; k < 410; k++) t->trans[k] = 1;
    t->lost1[700] = 0;
    pin_retry_cfg_t c; cfg_def(&c);
    pin_retry_stats_t st;
    run(t, &c, &g_out, &st);
    CHECK(check_complete(t, &g_out, "hdv") == 0, "HDV: damaged GOPs replaced");
    CHECK(st.sections_fixed == 1, "HDV: fixed");
    CHECK(g_out.cc_breaks == 0, "HDV: continuity counters re-phased at the seams");
    tape_free(t);
}

static void test_hdv_permanent(void)
{
    tape_t *t = tape_new(1200, 1);
    for (int k = 400; k < 403; k++) t->perm[k] = 1;
    pin_retry_cfg_t c; cfg_def(&c);
    pin_retry_stats_t st;
    run(t, &c, &g_out, &st);
    CHECK(check_complete(t, &g_out, "hdv permanent") == 3, "HDV crease: stays");
    CHECK(g_out.cc_breaks == 0, "HDV crease: continuity counters re-phased");
    tape_free(t);
}

static void test_block_map(void)
{
    uint8_t *f = malloc(FRAME_LEN), *g = malloc(FRAME_LEN);
    uint8_t map[NSEQ * 150];
    make_dv(f, 5, 0);
    CHECK(dv_error_block_map(f, FRAME_LEN, NULL, map) == DV_BLOCK_GOOD, "clean frame grades good");
    make_dv(g, 5, 1);
    CHECK(dv_error_block_map(g, FRAME_LEN, NULL, map) == DV_BLOCK_ERROR, "STA 7 grades error");
    /* copied audio: a block equal to another of the frame */
    make_dv(f, 5, 0);
    memcpy(f + 22 * 80 + 8, f + 6 * 80 + 8, 72);
    CHECK(dv_error_block_map(f, FRAME_LEN, NULL, map) == DV_BLOCK_SUSPECT, "copied audio is suspect");
    CHECK(map[6] == DV_BLOCK_SUSPECT && map[22] == DV_BLOCK_SUSPECT, "both copies suspect");
    /* re-encoding camera: every video block STA 14; only those equal to the previous frame count */
    make_dv(f, 5, 0);
    make_dv(g, 6, 0);
    for (unsigned i = 0; i < NSEQ * 150; i++) {
        unsigned b = i % 150;
        if (b >= 6 && (b - 6) % 16 != 0) {
            f[i * 80 + 3] = 0xE0;
            g[i * 80 + 3] = 0xE0;
        }
    }
    memcpy(g + 7 * 80 + 4, f + 7 * 80 + 4, 76);   /* one block stuck from the previous frame */
    CHECK(dv_error_block_map(g, FRAME_LEN, f, map) == DV_BLOCK_CONCEALED, "re-encoded: stuck block concealed");
    CHECK(map[7] == DV_BLOCK_CONCEALED && map[8] == DV_BLOCK_GOOD, "re-encoded: new picture data good");
    free(f);
    free(g);
}

static void test_cc_fix(void)
{
    pin_retry_cc_fix_t f;
    pin_retry_cc_fix_init(&f);
    uint8_t p[188];
    memset(p, 0xFF, sizeof(p));
    p[0] = 0x47; p[1] = 0x48; p[2] = 0x10;
    int out[6];
    int in[6] = {4, 5, 6, 11, 12, 13};
    unsigned rd[6] = {0, 0, 0, 1, 1, 1};
    for (int i = 0; i < 6; i++) {
        p[3] = (uint8_t)(0x10 | in[i]);
        pin_retry_cc_fix_apply(&f, p, 1, rd[i]);
        out[i] = p[3] & 15;
    }
    CHECK(out[0] == 4 && out[2] == 6 && out[3] == 7 && out[5] == 9, "CC continues across the seam");
}

static void test_rew_refused(void)
{
    tape_t *t = tape_new(900, 0);
    for (int k = 300; k < 304; k++) t->trans[k] = 1;
    t->ignore_rew = 1;
    pin_retry_cfg_t c; cfg_def(&c);
    pin_retry_stats_t st;
    run(t, &c, &g_out, &st);
    CHECK(check_complete(t, &g_out, "rew refused") == 4, "REW refused: nothing lost, damage kept");
    CHECK(st.sections_failed == 1, "REW refused: section given up");
    tape_free(t);
}

static void test_hdv_lost_gop(void)
{
    tape_t *t = tape_new(1200, 1);
    for (int k = 600; k < 612; k++) t->lost1[k] = 1;
    pin_retry_cfg_t c; cfg_def(&c);
    pin_retry_stats_t st;
    run(t, &c, &g_out, &st);
    CHECK(check_complete(t, &g_out, "hdv lost gop") == 0, "HDV: a GOP lost in transit is read again");
    CHECK(g_out.cc_breaks == 0, "HDV lost GOP: continuity counters re-phased");
    tape_free(t);
}

/* The release budget (writer queue room): nothing beyond it goes, and a budgeted flush lets
 * everything go in order, a piece per call. */
static void test_budget(void)
{
    memset(&g_out, 0, sizeof(g_out));
    g_out.last_cc = -1;
    pin_retry_cfg_t c; cfg_def(&c);
    pin_retry_t *r = pin_retry_new(&c, on_release, &g_out);
    uint8_t *frame = malloc(FRAME_LEN);
    pin_retry_set_release_budget(r, 0);
    for (int k = 0; k < 100; k++) {
        pin_retry_unit_t u;
        memset(&u, 0, sizeof(u));
        make_dv(frame, k, 0);
        u.data = frame; u.len = FRAME_LEN; u.fps = FPS; u.meta = &k; u.meta_len = sizeof(k);
        u.key_valid = 1; u.key = KEY_BASE + k;
        pin_retry_push(r, k / FPS, &u);
    }
    CHECK(g_out.n == 0, "budget 0: nothing released");
    int calls = 0, left;
    do {
        pin_retry_set_release_budget(r, 3 * FRAME_LEN);
        int before = g_out.n;
        left = pin_retry_flush_some(r);
        CHECK(g_out.n - before <= 3, "a budgeted flush stays within the budget");
        calls++;
    } while (left > 0 && calls < 100);
    CHECK(left == 0 && g_out.n == 100 && calls == 34, "budgeted flush: all of it, in pieces");
    for (int i = 0; i < g_out.n; i++)
        if (g_out.idx[i] != i) { CHECK(0, "budgeted flush: in order"); break; }
    pin_retry_free(r);
    free(frame);
}

int main(void)
{
    test_block_map();
    test_budget();
    test_cc_fix();
    test_clean();
    test_transient();
    test_cluster();
    test_lone_spot_quick();
    test_dense_then_sparse();
    test_whole_tape();
    test_file_storage();
    test_auto_turns_whole();
    test_apart();
    test_permanent();
    test_two_reads();
    test_attempt_limit();
    test_lost();
    test_no_timecode();
    test_slow_deck();
    test_stale_frame();
    test_hdv_stale();
    test_end_of_tape();
    test_window();
    test_uncorrected_mode();
    test_hdv();
    test_hdv_permanent();
    test_rew_refused();
    test_hdv_lost_gop();
    if (g_failures) {
        printf("%d failure(s)\n", g_failures);
        return 1;
    }
    printf("all pin_retry tests passed\n");
    return 0;
}
