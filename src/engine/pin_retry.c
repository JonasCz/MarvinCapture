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

/* Retry of bad tape sections: see pin_retry.h for the method. */

#define _FILE_OFFSET_BITS 64
#include "pin_retry.h"
#include "dv_error.h"
#include "../core/pin_fs.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define SEEK64(f, o) _fseeki64((f), (o), SEEK_SET)
#else
#define SEEK64(f, o) fseeko((f), (off_t)(o), SEEK_SET)
#endif

#define ANCHOR_S 1.0          /* good tape kept in front of a section to line a re-read up on */
#define REW_GUESS 20.0        /* wind speed before one was measured: landing short costs one
                                 more short wind, overshooting costs reading it all at 1x */
#define REW_LATENCY_S 0.5     /* from REW sent to the tape moving */
#define JOIN_MIN_S 1.0        /* the shortest join gap: one bad spot is re-read at once */
#define DENSE_GUESS_S 10.0    /* sections closer than this (or the measured retry cycle) are dense */
#define WHOLE_STREAK 3        /* AUTO: this many dense sections at the longest join gap: whole tape */
#define NOSIG_TRIGGER_S 2.0   /* a section open when the signal ends is retried after this */
#define NOSIG_WHOLE_S 15.0    /* WHOLE: the end of the recording (blank tape) */
#define REREAD_NOSIG_S 5.0    /* a re-read that stops delivering ends its attempt */
#define MAX_REW_S 180.0       /* a wind longer than this is not right: play */
#define MAX_RESEEKS 3         /* re-winds of one attempt after landing too late / too early */
#define NEAR_S 600.0          /* keys further apart are not the same recording */
#define DTS_WRAP (INT64_C(1) << 33)

typedef struct {
    uint8_t *data;          /* NULL: in the temporary file at off */
    int64_t off;
    size_t len;
    void *meta;
} unit_t;

typedef struct {
    unit_t *u;
    int n, cap;
    int key_valid;
    int64_t key;
    int dtc_valid;          /* the deck's TIME CODE when the slot arrived (HDV targets) */
    int64_t dtc;
    double pos;             /* tape seconds from the capture's first unit to this slot's start */
    double dur;             /* tape seconds in it */
    uint8_t *map;           /* DV block grades; NULL = all good */
    int grade;              /* DV worst grade */
    int pics, damaged;      /* HDV pictures, damaged ones */
    int hole;               /* data lost in transit before / in it */
    int complete;           /* HDV: closed by the next GOP */
    int ldts_valid;         /* HDV: the DTS of its last picture */
    int64_t ldts;
    int retried;            /* part of a section that was retried: never opens one again */
    unsigned read;
    int mended;
    size_t bytes;
} slot_t;

struct pin_retry {
    pin_retry_cfg_t cfg;
    pin_retry_release_fn release;
    void *user;

    slot_t **s;             /* the FIFO, oldest first */
    int n, cap;
    double pos;             /* tape seconds appended so far */
    double fps;
    int hdv;
    size_t held_bytes;
    size_t budget;

    pin_retry_phase_t phase;
    int sec_start, first_bad, last_bad, sec_end;
    int attempt;
    unsigned read;          /* the current read */
    double last_push;

    /* winding */
    pin_retry_action_t act;
    double trigger_t, rew_t, play_t, wind_start_t;
    double pos_at_rew, dist_s;
    int tc_seen;            /* the deck answered TIME CODE during this wind */
    int reseeks;
    int play_failed;
    int tail_ok;            /* units still continue the held tape (REW not effective yet) */
    int seek_tail;          /* HDV, seeking: the GOP being read continues the held tape */
    int cand_valid;         /* seeking: the last keyed unit, which the next one must follow before
                               it counts (a deck can send a stale frame right after PLAY) */
    int64_t cand_key;
    int gated;              /* PLAY of this wind waited for the deck to start winding: its landing
                               says nothing about the stop lead */
    int64_t rew_tc;         /* the deck's TIME CODE when the REW went out */
    int rew_tc_valid;
    int64_t wtc;            /* the deck's last TIME CODE while this wind runs, for its speed */
    double wtc_t;
    int wtc_valid;

    /* re-read */
    int cursor, matched;
    double seek_s;          /* tape seconds read since PLAY before the first match */
    unsigned mended_now;    /* slots mended in this attempt */
    unsigned mended_sec;    /* ... in this section */
    int fruitless;          /* attempts in a row that mended nothing */
    slot_t *pend;           /* HDV: the re-read GOP being assembled */

    int64_t deck_tc;
    double deck_tc_t;
    int deck_tc_valid;
    unsigned long lost_pending;

    int tail_dts_valid;     /* HDV: the DTS of the newest picture held */
    int64_t tail_dts;
    uint8_t *prev;          /* the previous DV frame of the running read (block grades) */
    size_t prev_len, prev_cap;
    uint8_t *mapbuf;        /* block grades of the frame being graded */
    size_t mapcap;

    /* adaptive join gap */
    double join;
    double last_sec_end;    /* tape position where the previous section ended */
    int have_last_sec;
    int dense_streak;
    int whole;              /* holding from the first damage to the end of the recording */
    double rew_from;        /* tape position when this attempt's wind was asked for */

    /* the temporary file */
    FILE *sf;
    char spath[1200];
    int64_t send;
    int spool_failed;
    uint8_t *iobuf, *mbuf;
    size_t iocap, mcap;

    pin_retry_stats_t st;
};

/* ---- small helpers ---------------------------------------------------- */

void pin_retry_cfg_defaults(pin_retry_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->attempts = PIN_RETRY_ATTEMPTS_DEFAULT;
    c->window_s = PIN_RETRY_WINDOW_DEFAULT;
    c->trigger = PIN_RETRY_TRIGGER_ANY;
    c->preroll_s = PIN_RETRY_PREROLL_DEFAULT;
    c->join_s = 0;
    c->mode = PIN_RETRY_MODE_AUTO;
    c->storage = PIN_RETRY_STORE_RAM;
}

void pin_retry_cfg_clamp(pin_retry_cfg_t *c)
{
    if (c->attempts < 1 || c->attempts > PIN_RETRY_ATTEMPTS_MAX)
        c->attempts = c->attempts > PIN_RETRY_ATTEMPTS_MAX ? PIN_RETRY_ATTEMPTS_MAX
                                                          : PIN_RETRY_ATTEMPTS_DEFAULT;
    if (!(c->window_s > 0))
        c->window_s = PIN_RETRY_WINDOW_DEFAULT;
    if (c->window_s < PIN_RETRY_WINDOW_MIN) c->window_s = PIN_RETRY_WINDOW_MIN;
    if (c->storage != PIN_RETRY_STORE_FILE)
        c->storage = PIN_RETRY_STORE_RAM;
    if (c->mode != PIN_RETRY_MODE_SECTIONS && c->mode != PIN_RETRY_MODE_WHOLE)
        c->mode = PIN_RETRY_MODE_AUTO;
    double wmax = c->storage == PIN_RETRY_STORE_FILE ? PIN_RETRY_WINDOW_MAX : PIN_RETRY_WINDOW_MAX_RAM;
    if (c->window_s > wmax) c->window_s = wmax;
    c->spool_dir[sizeof(c->spool_dir) - 1] = 0;
    if (c->trigger != PIN_RETRY_TRIGGER_UNCORRECTED)
        c->trigger = PIN_RETRY_TRIGGER_ANY;
    if (!(c->preroll_s >= 0)) c->preroll_s = PIN_RETRY_PREROLL_DEFAULT;
    if (c->preroll_s > PIN_RETRY_PREROLL_MAX) c->preroll_s = PIN_RETRY_PREROLL_MAX;
    if (!(c->join_s >= 0)) c->join_s = 0;
    if (c->join_s > c->window_s / 2) c->join_s = c->window_s / 2;
}

int64_t pin_retry_tc_frames(int hour, int minute, int second, int frame, double fps)
{
    int nominal = fps > 27 ? 30 : 25;
    if (fps > 45) nominal = fps > 55 ? 60 : 50;
    return (((int64_t)hour * 60 + minute) * 60 + second) * nominal + frame;
}

/* a - b in key units, for DTS modulo 2^33 */
static int64_t key_diff(const pin_retry_t *r, int64_t a, int64_t b)
{
    int64_t d = a - b;
    if (r->hdv) {
        d %= DTS_WRAP;
        if (d < 0) d += DTS_WRAP;
        if (d >= DTS_WRAP / 2) d -= DTS_WRAP;
    }
    return d;
}

/* key units per second */
static double key_rate(const pin_retry_t *r)
{
    if (r->hdv)
        return 90000.0;
    return r->fps > 27 ? 30.0 : 25.0;
}

/* TIME CODE frames per second (pin_retry_tc_frames()'s nominal rate) */
static double key_rate_tc(const pin_retry_t *r)
{
    return r->fps > 27 ? 30.0 : 25.0;
}

static double key_seconds(const pin_retry_t *r, int64_t a, int64_t b)
{
    return (double)key_diff(r, a, b) / key_rate(r);
}

static int key_near(const pin_retry_t *r, int64_t a, int64_t b)
{
    double d = key_seconds(r, a, b);
    return d > -NEAR_S && d < NEAR_S;
}

/* HDV: picture b decodes right after picture a (one picture's DTS step, 90 kHz). */
static int dts_follows(const pin_retry_t *r, int64_t a, int64_t b)
{
    int64_t d = b - a;
    d %= DTS_WRAP;
    if (d < 0) d += DTS_WRAP;
    double step = 90000.0 / (r->fps > 1 ? r->fps : 25.0);
    return d > 0 && d <= 1.6 * step;
}

static double join_gap(const pin_retry_t *r)
{
    if (r->cfg.join_s > 0)
        return r->cfg.join_s;
    return r->join > 0 ? r->join : JOIN_MIN_S;
}

static double join_max(const pin_retry_t *r)
{
    return r->cfg.window_s / 2;
}

/* Gaps between sections below this make the tape "dense": the cost of a retry cycle, when
 * measured, at least twice the join gap. */
static double dense_gap(const pin_retry_t *r)
{
    double c = r->st.cycle_s > 0 ? r->st.cycle_s : DENSE_GUESS_S;
    double j = 2 * join_gap(r);
    return c > j ? c : j;
}

static double rew_rate(const pin_retry_t *r)
{
    return r->st.rew_rate > 0 ? r->st.rew_rate : REW_GUESS;
}

static void learn(double *v, double x)
{
    if (!(x > 0) || !isfinite(x))
        return;
    *v = *v > 0 ? 0.5 * *v + 0.5 * x : x;
}

/* ---- the temporary file ----------------------------------------------- */

static int spool_open(pin_retry_t *r)
{
    if (r->sf)
        return 0;
    if (r->spool_failed)
        return -1;
    static unsigned seq;
    const char *dir = r->cfg.spool_dir[0] ? r->cfg.spool_dir : ".";
    size_t dl = strlen(dir);
    int sep = dl && (dir[dl - 1] == '/' || dir[dl - 1] == '\\');
    snprintf(r->spath, sizeof(r->spath), "%s%s.marvin-retry-%lx-%u.tmp", dir, sep ? "" : "/",
             (unsigned long)(uintptr_t)r & 0xffffffu, ++seq);
    r->sf = pin_fs_fopen(r->spath, "w+b");
    if (!r->sf) {
        r->spool_failed = 1;
        return -1;
    }
    r->send = 0;
    return 0;
}

static void spool_close(pin_retry_t *r)
{
    if (r->sf) {
        fclose(r->sf);
        r->sf = NULL;
        pin_fs_remove(r->spath);
    }
}

static int spool_wanted(const pin_retry_t *r)
{
    return !r->spool_failed && (r->cfg.storage == PIN_RETRY_STORE_FILE || r->whole);
}

/* Stores len bytes for a unit: in the file when it is wanted and works, else in RAM. */
static int unit_put(pin_retry_t *r, unit_t *d, const uint8_t *src, size_t len, int disk)
{
    d->data = NULL;
    d->off = -1;
    d->len = len;
    if (disk && spool_open(r) == 0) {
        if (SEEK64(r->sf, r->send) == 0 && fwrite(src, 1, len, r->sf) == len) {
            d->off = r->send;
            r->send += (int64_t)len;
            return 0;
        }
        /* the drive is full or failing: RAM from here on, and no more holding the whole tape */
        r->spool_failed = 1;
        r->whole = 0;
    }
    d->data = malloc(len ? len : 1);
    if (!d->data)
        return -1;
    memcpy(d->data, src, len);
    return 0;
}

/* The unit's bytes: its RAM copy, or read into buf (grown as needed). NULL on a read error. */
static uint8_t *unit_get(pin_retry_t *r, const unit_t *d, uint8_t **buf, size_t *cap)
{
    if (d->data)
        return d->data;
    if (*cap < d->len) {
        uint8_t *b = realloc(*buf, d->len);
        if (!b) return NULL;
        *buf = b;
        *cap = d->len;
    }
    if (!r->sf || SEEK64(r->sf, d->off) != 0 || fread(*buf, 1, d->len, r->sf) != d->len)
        return NULL;
    return *buf;
}

/* Writes a mended unit back where it is kept. */
static void unit_set(pin_retry_t *r, unit_t *d, const uint8_t *src)
{
    if (d->data) {
        if (d->data != src)
            memcpy(d->data, src, d->len);
        return;
    }
    if (r->sf && SEEK64(r->sf, d->off) == 0)
        fwrite(src, 1, d->len, r->sf);
}

/* ---- slots ------------------------------------------------------------ */

static slot_t *slot_new(void)
{
    return calloc(1, sizeof(slot_t));
}

static void slot_free(slot_t *s)
{
    if (!s) return;
    for (int i = 0; i < s->n; i++) {
        free(s->u[i].data);
        free(s->u[i].meta);
    }
    free(s->u);
    free(s->map);
    free(s);
}

static int slot_add(pin_retry_t *r, slot_t *s, const pin_retry_unit_t *u, int disk)
{
    if (s->n == s->cap) {
        int nc = s->cap ? s->cap * 2 : 4;
        unit_t *nu = realloc(s->u, (size_t)nc * sizeof(unit_t));
        if (!nu) return -1;
        s->u = nu;
        s->cap = nc;
    }
    unit_t *d = &s->u[s->n];
    d->meta = u->meta_len ? malloc(u->meta_len) : NULL;
    if ((u->meta_len && !d->meta) || unit_put(r, d, u->data, u->len, disk) != 0) {
        free(d->data);
        free(d->meta);
        return -1;
    }
    if (u->meta_len) memcpy(d->meta, u->meta, u->meta_len);
    s->n++;
    s->bytes += u->len;
    return 0;
}

/* The slot asks for a re-read. */
static int slot_bad(const pin_retry_t *r, const slot_t *s)
{
    if (s->hole) return 1;
    if (r->hdv) return s->damaged > 0;
    if (r->cfg.trigger == PIN_RETRY_TRIGGER_UNCORRECTED)
        return s->grade >= DV_BLOCK_ERROR;
    return s->grade >= DV_BLOCK_CONCEALED;
}

/* For HDV only complete GOPs are judged (the open one is still growing). */
static int slot_judged(const pin_retry_t *r, const slot_t *s)
{
    return !r->hdv || s->complete;
}

/* Moves a slot's RAM units into the temporary file, when units are kept there. */
static void slot_spool(pin_retry_t *r, slot_t *s)
{
    if (!spool_wanted(r))
        return;
    for (int i = 0; i < s->n; i++) {
        unit_t *d = &s->u[i];
        if (!d->data)
            continue;
        uint8_t *ram = d->data;
        if (unit_put(r, d, ram, d->len, 1) == 0 && !d->data)
            free(ram);
        else if (d->data != ram) {
            free(d->data);      /* unit_put fell back to a new RAM copy: keep the old one */
            d->data = ram;
        }
    }
}

static int fifo_insert(pin_retry_t *r, int at, slot_t *s)
{
    if (r->n == r->cap) {
        int nc = r->cap ? r->cap * 2 : 256;
        slot_t **ns = realloc(r->s, (size_t)nc * sizeof(slot_t *));
        if (!ns) return -1;
        r->s = ns;
        r->cap = nc;
    }
    memmove(r->s + at + 1, r->s + at, (size_t)(r->n - at) * sizeof(slot_t *));
    r->s[at] = s;
    r->n++;
    r->held_bytes += s->bytes;
    return 0;
}

/* Lets the k oldest slots go. */
static void release_front(pin_retry_t *r, int k)
{
    for (int i = 0; i < k; i++) {
        slot_t *s = r->s[i];
        for (int j = 0; j < s->n; j++) {
            uint8_t *data = unit_get(r, &s->u[j], &r->iobuf, &r->iocap);
            if (data)
                r->release(r->user, data, s->u[j].len, s->u[j].meta, s->read, s->mended);
        }
        r->held_bytes -= s->bytes;
        slot_free(s);
    }
    memmove(r->s, r->s + k, (size_t)(r->n - k) * sizeof(slot_t *));
    r->n -= k;
    r->sec_start -= k; r->first_bad -= k; r->last_bad -= k; r->sec_end -= k;
    if (r->cursor >= 0) r->cursor -= k;
    if (r->sec_start < 0) r->sec_start = 0;
    if (!r->n)
        r->send = 0;    /* nothing held: the temporary file starts over */
}

/* How many of the oldest slots may go now: everything older than the anchor margin, but not an
 * open section, not an HDV GOP that is still growing, and within the release budget. */
static void release_ready(pin_retry_t *r)
{
    if (!r->n)
        return;
    int limit = r->n;
    if (r->phase != PIN_RETRY_IDLE)
        limit = r->sec_start;
    double newest = r->pos;
    size_t spent = 0;
    int k = 0;
    while (k < limit) {
        slot_t *s = r->s[k];
        if (!slot_judged(r, s))
            break;
        if (r->phase == PIN_RETRY_IDLE && s->pos + s->dur > newest - ANCHOR_S)
            break;
        if (r->budget != SIZE_MAX && spent + s->bytes > r->budget && k > 0)
            break;
        if (r->budget != SIZE_MAX && s->bytes > r->budget)
            break;
        spent += s->bytes;
        k++;
    }
    if (k) {
        if (r->budget != SIZE_MAX)
            r->budget -= spent < r->budget ? spent : r->budget;
        release_front(r, k);
    }
}

/* ---- life cycle ------------------------------------------------------- */

pin_retry_t *pin_retry_new(const pin_retry_cfg_t *cfg, pin_retry_release_fn release, void *user)
{
    pin_retry_t *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    if (cfg) r->cfg = *cfg; else pin_retry_cfg_defaults(&r->cfg);
    pin_retry_cfg_clamp(&r->cfg);
    r->release = release;
    r->user = user;
    r->budget = SIZE_MAX;
    r->fps = 25;
    r->st.attempts = r->cfg.attempts;
    r->cursor = -1;
    r->join = JOIN_MIN_S;
    if (r->cfg.mode == PIN_RETRY_MODE_WHOLE && spool_open(r) == 0)
        r->whole = 1;
    return r;
}

void pin_retry_free(pin_retry_t *r)
{
    if (!r) return;
    for (int i = 0; i < r->n; i++)
        slot_free(r->s[i]);
    free(r->s);
    slot_free(r->pend);
    free(r->prev);
    free(r->mapbuf);
    free(r->iobuf);
    free(r->mbuf);
    spool_close(r);
    free(r);
}

void pin_retry_set_learned(pin_retry_t *r, double rew, double cycle)
{
    if (rew > 0) r->st.rew_rate = rew;
    if (cycle > 0) r->st.cycle_s = cycle;
}

void pin_retry_set_release_budget(pin_retry_t *r, size_t bytes)
{
    r->budget = bytes;
}

int pin_retry_busy(const pin_retry_t *r)
{
    return r && (r->phase == PIN_RETRY_REWINDING || r->phase == PIN_RETRY_SEEKING ||
                 r->phase == PIN_RETRY_REREADING);
}

void pin_retry_lost(pin_retry_t *r, unsigned long frames)
{
    if (r && frames && !pin_retry_busy(r))
        r->lost_pending += frames;
}

/* ---- sections --------------------------------------------------------- */

static void section_close(pin_retry_t *r)
{
    for (int i = r->sec_start; i <= r->sec_end && i < r->n; i++)
        if (i >= 0) r->s[i]->retried = 1;
    if (r->sec_end >= 0 && r->sec_end < r->n) {
        r->last_sec_end = r->s[r->sec_end]->pos + r->s[r->sec_end]->dur;
        r->have_last_sec = 1;
    }
    r->phase = PIN_RETRY_IDLE;
    r->attempt = 0;
    r->act = PIN_RETRY_ACT_NONE;
    r->matched = 0;
    r->cursor = -1;
    slot_free(r->pend);
    r->pend = NULL;
}

static unsigned section_bad(const pin_retry_t *r, int *first)
{
    unsigned n = 0;
    *first = -1;
    for (int i = r->sec_start; i <= r->sec_end && i < r->n; i++)
        if (slot_bad(r, r->s[i])) {
            if (*first < 0) *first = i;
            n++;
        }
    return n;
}

/* The first slot of a section: the anchor margin in front of `bad`. */
static int anchor_start(const pin_retry_t *r, int bad)
{
    int i = bad;
    while (i > 0 && r->s[i - 1]->pos > r->s[bad]->pos - ANCHOR_S - 1e-9 && !r->s[i - 1]->hole)
        i--;
    if (i == bad && bad > 0)
        i = bad - 1;
    return i;
}

static int section_keyed(const pin_retry_t *r)
{
    for (int i = r->sec_start; i <= r->sec_end && i < r->n; i++)
        if (r->s[i]->key_valid) return 1;
    return 0;
}

/* Wind back from where the deck is now (`at` tape seconds) to the section's start. */
static void wind_back(pin_retry_t *r, double now, double at, int tail)
{
    r->phase = PIN_RETRY_REWINDING;
    r->act = PIN_RETRY_ACT_REW;
    r->pos_at_rew = at;
    r->dist_s = at - r->s[r->sec_start]->pos + r->cfg.preroll_s;
    if (r->dist_s < r->cfg.preroll_s) r->dist_s = r->cfg.preroll_s;
    r->rew_t = r->play_t = r->wind_start_t = 0;
    r->rew_from = r->pos;
    r->tc_seen = 0;
    r->wtc_valid = 0;
    r->seek_tail = 0;
    r->gated = 0;
    r->cand_valid = 0;
    r->tail_ok = tail;
    r->matched = 0;
    r->cursor = -1;
    r->seek_s = 0;
    r->play_failed = 0;
    slot_free(r->pend);
    r->pend = NULL;
    (void)now;
}

static void attempt_begin(pin_retry_t *r, double now)
{
    r->attempt++;
    r->st.attempt = r->attempt;
    r->mended_sec += r->mended_now;
    r->mended_now = 0;
    r->reseeks = 0;
    r->sec_end = r->n - 1;
    /* With the signal gone (the end of the recording) the deck has played on into blank tape
     * since the last unit: that is further to wind back. */
    double blank = r->last_push > 0 && now - r->last_push > 0.5 ? now - r->last_push : 0;
    wind_back(r, now, r->pos + blank, 1);
}

/* The section is complete (join gap, window, signal end): retry it or let it go. */
static void section_trigger(pin_retry_t *r, double now)
{
    r->sec_end = r->n - 1;
    if (!section_keyed(r)) {
        /* nothing to line a re-read up on: keep what there is */
        section_close(r);
        return;
    }
    r->st.sections++;
    r->trigger_t = now;
    r->attempt = 0;
    r->mended_sec = 0;
    r->fruitless = 0;
    r->mended_now = 0;
    attempt_begin(r, now);
}

/* After a re-read passed the section: again, or let it go. */
static void attempt_end(pin_retry_t *r, double now)
{
    int first;
    unsigned bad = section_bad(r, &first);
    r->st.bad_slots = bad;
    if (bad == 0) {
        r->st.sections_fixed++;
        section_close(r);
        return;
    }
    /* Dirt can take more than one more read to come off; damage that two reads in a row could
     * not mend is on the tape. */
    r->fruitless = r->mended_now > 0 ? 0 : r->fruitless + 1;
    if (r->attempt < r->cfg.attempts && r->fruitless < 2) {
        /* again, from the first slot still bad; what is before it is done */
        r->sec_start = anchor_start(r, first);
        r->first_bad = first;
        r->trigger_t = now;
        attempt_begin(r, now);
        return;
    }
    if (r->mended_sec + r->mended_now > 0)
        r->st.sections_partly++;
    else
        r->st.sections_failed++;
    section_close(r);
}

/* A slot was completed while passing through or holding. */
static void judge_slot(pin_retry_t *r, int i, double now)
{
    slot_t *s = r->s[i];
    if (s->retried || !slot_bad(r, s))
        return;
    if (r->phase == PIN_RETRY_IDLE) {
        if (r->have_last_sec && r->cfg.join_s <= 0) {
            /* How dense is the damage? A section soon after the last one: wait longer for the
             * next bad spot, so one rewind takes more of them; a long clean stretch: back to
             * short waits, so a lone spot is re-read at once. */
            double gap = s->pos - r->last_sec_end;
            double dense = dense_gap(r);
            if (gap < dense) {
                r->join = r->join * 2 > join_max(r) ? join_max(r) : r->join * 2;
                if (r->join >= join_max(r))
                    r->dense_streak++;
            } else {
                r->dense_streak = 0;
                while (gap > 4 * dense && r->join > JOIN_MIN_S) {
                    r->join = r->join / 2 < JOIN_MIN_S ? JOIN_MIN_S : r->join / 2;
                    gap /= 2;
                }
            }
            if (r->cfg.mode == PIN_RETRY_MODE_AUTO && !r->whole && r->dense_streak >= WHOLE_STREAK &&
                spool_open(r) == 0) {
                /* a very poor tape: the rest of it in one go, re-read at the end */
                r->whole = 1;
            }
        }
        r->phase = PIN_RETRY_HOLDING;
        r->first_bad = r->last_bad = i;
        r->sec_start = anchor_start(r, i);
        r->sec_end = r->n - 1;
    } else if (r->phase == PIN_RETRY_HOLDING) {
        r->last_bad = i;
    }
    (void)now;
}

/* nosig: seconds the signal has been away (0 while it is there). */
static void holding_check(pin_retry_t *r, double now, double nosig)
{
    if (r->phase != PIN_RETRY_HOLDING)
        return;
    r->sec_end = r->n - 1;
    if (r->whole) {
        /* everything to the end of the recording, then the passes */
        if (nosig >= NOSIG_WHOLE_S)
            section_trigger(r, now);
        return;
    }
    slot_t *lb = r->s[r->last_bad];
    double clean = r->pos - (lb->pos + lb->dur);
    double span = r->pos - r->s[r->sec_start]->pos;
    if (clean >= join_gap(r) || span >= r->cfg.window_s || nosig >= NOSIG_TRIGGER_S)
        section_trigger(r, now);
}

/* ---- arriving units --------------------------------------------------- */

/* Grades a DV frame (data, len: the bytes as they arrived); the map is kept only for a frame with
 * something wrong (a whole tape of good frames keeps none). */
static void dv_grade(pin_retry_t *r, slot_t *s, const uint8_t *data, size_t len)
{
    size_t nb = len / 80;
    if (r->mapcap < nb) {
        uint8_t *m = realloc(r->mapbuf, nb ? nb : 1);
        if (!m) { s->grade = DV_BLOCK_ERROR; return; }
        r->mapbuf = m;
        r->mapcap = nb;
    }
    const uint8_t *prev = r->prev && r->prev_len == len ? r->prev : NULL;
    int g = dv_error_block_map(data, len, prev, r->mapbuf);
    if (g < 0) {
        memset(r->mapbuf, DV_BLOCK_ERROR, nb);
        g = DV_BLOCK_ERROR;
    }
    s->grade = g;
    free(s->map);
    s->map = NULL;
    if (g > DV_BLOCK_GOOD) {
        s->map = malloc(nb ? nb : 1);
        if (s->map)
            memcpy(s->map, r->mapbuf, nb);
        else
            s->grade = DV_BLOCK_ERROR;
    }
    if (r->prev_cap < len) {
        uint8_t *p = realloc(r->prev, len);
        if (!p) { r->prev_len = 0; return; }
        r->prev = p;
        r->prev_cap = len;
    }
    memcpy(r->prev, data, len);
    r->prev_len = len;
}

/* A new slot from one DV frame / the start of an HDV GOP, at the live end. */
static slot_t *make_slot(pin_retry_t *r, const pin_retry_unit_t *u, double now, int disk)
{
    slot_t *s = slot_new();
    if (!s) return NULL;
    if (slot_add(r, s, u, disk) != 0) { slot_free(s); return NULL; }
    s->key_valid = u->key_valid;
    s->key = u->key;
    if (r->deck_tc_valid && now - r->deck_tc_t < 2.0) {
        s->dtc_valid = 1;
        s->dtc = r->deck_tc;
    }
    s->read = r->read;
    s->hole = u->lost_before ? 1 : 0;
    if (r->hdv) {
        s->ldts_valid = u->dts_valid;
        s->ldts = u->dts;
        s->pics = u->picture ? 1 : 0;
        s->damaged = u->damaged ? 1 : 0;
        s->dur = u->picture ? 1.0 / r->fps : 0;
    } else {
        s->dur = 1.0 / r->fps;
        dv_grade(r, s, u->data, u->len);
    }
    return s;
}

/* Appends a unit at the live end (passing through, holding, the tail before a wind). */
static void append_live(pin_retry_t *r, const pin_retry_unit_t *u, double now)
{
    /* frames that never arrived: a hole in front of this unit */
    int hole = r->lost_pending ? 1 : 0;
    r->lost_pending = 0;
    if (r->hdv) {
        slot_t *cur = r->n ? r->s[r->n - 1] : NULL;
        if (!cur || (u->opens_gop && cur->n) || cur->complete) {
            if (cur && !cur->complete) {
                cur->complete = 1;
                judge_slot(r, r->n - 1, now);
            }
            slot_t *s = make_slot(r, u, now, spool_wanted(r));
            if (!s) return;
            s->pos = r->pos;
            s->hole |= hole;
            r->pos += s->dur;
            if (fifo_insert(r, r->n, s) != 0) { slot_free(s); return; }
        } else {
            if (slot_add(r, cur, u, spool_wanted(r)) != 0) return;
            r->held_bytes += u->len;
            if (u->picture) {
                cur->pics++;
                cur->dur += 1.0 / r->fps;
                r->pos += 1.0 / r->fps;
            }
            if (u->damaged) cur->damaged++;
            if (u->lost_before || hole) cur->hole = 1;
            if (u->dts_valid) {
                cur->ldts_valid = 1;
                cur->ldts = u->dts;
            }
        }
        if (u->dts_valid) {
            r->tail_dts_valid = 1;
            r->tail_dts = u->dts;
        }
    } else {
        slot_t *s = make_slot(r, u, now, spool_wanted(r));
        if (!s) return;
        s->pos = r->pos;
        s->hole |= hole;
        r->pos += s->dur;
        if (fifo_insert(r, r->n, s) != 0) { slot_free(s); return; }
        judge_slot(r, r->n - 1, now);
    }
}

/* A unit (or a GOP slot g) of the tape after the section, while the deck is about to wind: it is
 * re-read with the section next time, so the section grows by it. */
static void tail_fix(pin_retry_t *r)
{
    r->sec_end = r->n - 1;
    r->pos_at_rew = r->pos;
    r->dist_s = r->pos - r->s[r->sec_start]->pos + r->cfg.preroll_s;
}

/* Whether a unit read after REW was sent still continues the held tape (the deck has not
 * started winding yet). */
static int continues_tail(const pin_retry_t *r, const pin_retry_unit_t *u)
{
    if (!r->n)
        return 0;
    if (r->hdv && u->dts_valid && r->tail_dts_valid)
        return dts_follows(r, r->tail_dts, u->dts);
    if (r->hdv && !u->opens_gop)
        return r->tail_ok;
    if (!u->key_valid)
        return r->tail_ok;
    for (int i = r->n - 1; i >= 0 && i >= r->n - 4; i--) {
        const slot_t *s = r->s[i];
        if (!s->key_valid) continue;
        double d = key_seconds(r, u->key, s->key);
        double step = s->dur > 0 ? s->dur : 1.0 / r->fps;
        return d > 0 && d < step * (r->n - i) + 1.0;
    }
    return r->tail_ok;
}

/* The key is the very next one after the newest held slot (DV: the next frame; HDV: the next
 * GOP), allowing for drop-frame timecode. */
static int next_after_tail(const pin_retry_t *r, int64_t key)
{
    const slot_t *s = r->s[r->n - 1];
    if (!s->key_valid)
        return 0;
    double d = key_seconds(r, key, s->key);
    double step = s->dur > 0 ? s->dur : 1.0 / r->fps;
    return d > 0 && d < step + 0.5 / r->fps + (!r->hdv && r->fps > 27 ? 2.0 / 30 : 0);
}

/* Find the slot a re-read key belongs to: from `from` on, then anywhere in the section. */
static int find_key(const pin_retry_t *r, int64_t key, int from)
{
    if (from < r->sec_start) from = r->sec_start;
    for (int i = from; i <= r->sec_end && i < r->n; i++)
        if (r->s[i]->key_valid && r->s[i]->key == key)
            return i;
    for (int i = r->sec_start; i < from && i < r->n; i++)
        if (r->s[i]->key_valid && r->s[i]->key == key)
            return i;
    return -1;
}

static int64_t section_last_key(const pin_retry_t *r, int *valid)
{
    for (int i = r->sec_end; i >= r->sec_start && i >= 0; i--)
        if (i < r->n && r->s[i]->key_valid) {
            *valid = 1;
            return r->s[i]->key;
        }
    *valid = 0;
    return 0;
}

/* Merges a re-read slot (n = the re-read DV frame / HDV GOP, owned by the caller) into slot i. */
static void merge_into(pin_retry_t *r, int i, slot_t *n)
{
    slot_t *s = r->s[i];
    int better = 0;
    if (r->hdv) {
        int before = slot_bad(r, s);
        if (s->hole && !n->hole && n->damaged <= s->damaged)
            better = 1;
        else if (n->damaged < s->damaged || (n->damaged == s->damaged && n->pics > s->pics && !n->hole))
            better = 1;
        if (better) {
            /* swap the contents, keep the place */
            slot_t tmp = *s;
            double pos = s->pos;
            int retried = s->retried, was = s->mended;
            *s = *n;
            *n = tmp;
            s->pos = pos;
            s->retried = retried;
            s->mended = was == 2 ? 2 : 1;
            s->complete = 1;
            r->held_bytes = r->held_bytes - n->bytes + s->bytes;
            slot_spool(r, s);
            if (before) r->mended_now++;
            r->st.frames_mended++;
        }
        return;
    }
    /* DV: block by block. A slot without a map is all good: nothing to mend. */
    if (!s->map)
        return;
    int was_bad = slot_bad(r, s);
    uint8_t *nd = n->u[0].data;     /* a re-read is always in RAM */
    if (s->u[0].len != n->u[0].len || !nd) {
        if (n->grade < s->grade && nd && s->u[0].len == n->u[0].len) {
            unit_set(r, &s->u[0], nd);
            free(s->map);
            s->map = n->map;
            n->map = NULL;
            s->grade = n->grade;
            s->mended = s->mended ? s->mended : 1;
            r->mended_now++;
            r->st.frames_mended++;
        }
        return;
    }
    if (!n->map) {
        /* the re-read is clean everywhere: it replaces the frame */
        unit_set(r, &s->u[0], nd);
        free(s->map);
        s->map = NULL;
        s->grade = DV_BLOCK_GOOD;
        if (!s->mended) s->mended = 1;
        r->st.frames_mended++;
        if (was_bad) r->mended_now++;
        return;
    }
    uint8_t *sd = unit_get(r, &s->u[0], &r->mbuf, &r->mcap);
    if (!sd)
        return;
    size_t nb = s->u[0].len / 80;
    int changed = 0, worst = 0;
    for (size_t b = 0; b < nb; b++) {
        if (n->map[b] < s->map[b]) {
            memcpy(sd + b * 80, nd + b * 80, 80);
            s->map[b] = n->map[b];
            changed = 1;
        }
        if (s->map[b] > worst) worst = s->map[b];
    }
    s->grade = worst;
    if (changed)
        unit_set(r, &s->u[0], sd);
    if (worst == DV_BLOCK_GOOD) {
        free(s->map);
        s->map = NULL;
    }
    if (changed) {
        if (!s->mended) s->mended = 1;
        r->st.frames_mended++;
        if (was_bad) r->mended_now++;
    }
}

/* Clears the hole flag of slot i when its key follows the previous slot's without a gap. */
static void hole_check(pin_retry_t *r, int i)
{
    if (i <= 0 || i >= r->n || !r->s[i]->hole)
        return;
    const slot_t *p = r->s[i - 1], *s = r->s[i];
    if (!p->key_valid || !s->key_valid)
        return;
    double d = key_seconds(r, s->key, p->key);
    double step = p->dur > 0 ? p->dur : 1.0 / r->fps;
    if (d > 0 && d < step + 0.5 / r->fps + (r->fps > 27 && !r->hdv ? 2.0 / 30 : 0)) {
        r->s[i]->hole = 0;
        if (!slot_bad(r, r->s[i]))
            r->mended_now++;
    }
}

/* A re-read slot (DV frame or complete HDV GOP) while seeking or re-reading. Returns 1 when it
 * is past the section (new tape): the caller appends it live. Takes ownership of n otherwise. */
static int reread_slot(pin_retry_t *r, slot_t *n, double now)
{
    if (!r->matched) {
        r->seek_s += n->dur;
        if (!n->key_valid) {
            slot_free(n);
            return 0;
        }
        /* Where the deck is counts only once two units in a row say so: a stale frame (the one
         * shown before the wind, sent again as PLAY starts) must not move anything. */
        {
            double gap = r->cand_valid ? key_seconds(r, n->key, r->cand_key) : -1;
            double step = r->hdv ? 1.0 : 1.5 / r->fps;
            int follows = r->cand_valid && gap > 0 && gap <= step;
            r->cand_key = n->key;
            r->cand_valid = 1;
            if (!follows) {
                slot_free(n);
                return 0;
            }
        }
        int i = find_key(r, n->key, r->sec_start);
        if (i < 0) {
            /* not in the section: before it (read on), or past its end (wind again) */
            int lv;
            int64_t last = section_last_key(r, &lv);
            if (lv && key_near(r, n->key, last) && key_diff(r, n->key, last) > 0) {
                if (r->reseeks++ >= MAX_RESEEKS) {
                    /* past the section and cannot get back to it: carry on from here */
                    r->st.sections_failed++;
                    section_close(r);
                    return 1;
                }
                double over = key_seconds(r, n->key, last);
                double at = r->s[r->sec_end]->pos + r->s[r->sec_end]->dur + over;
                double wound = r->pos_at_rew - (at - r->seek_s);
                if (r->play_t - r->wind_start_t > 0.5 && r->wind_start_t > 0 && wound > 0) {
                    double v = wound / (r->play_t - r->wind_start_t);
                    if (v > 1.5 && v < 400)
                        learn(&r->st.rew_rate, v);
                }
                wind_back(r, now, at, 0);
            }
            slot_free(n);
            return 0;
        }
        /* landed: learn the wind speed, and how much earlier PLAY must go out to land a preroll
         * before the section (positive: it wound too far, negative: not far enough) */
        double landing = r->s[i]->pos - (r->seek_s - n->dur);
        double wound = r->pos_at_rew - landing;
        if (r->play_t - r->wind_start_t > 0.5 && r->wind_start_t > 0 && wound > 0) {
            double v = wound / (r->play_t - r->wind_start_t);
            if (v > 1.5 && v < 400)
                learn(&r->st.rew_rate, v);
        }
        if (!r->gated) {
            double want = r->s[r->sec_start]->pos - r->cfg.preroll_s;
            double err = want - landing;
            double l = r->st.stop_lead_s + 0.7 * err / rew_rate(r);
            r->st.stop_lead_s = l < -1.0 ? -1.0 : l > 3.0 ? 3.0 : l;
        }
        if (i > r->first_bad && i > r->sec_start) {
            /* too late: the start of the damage went by unread */
            slot_free(n);
            if (r->reseeks++ >= MAX_RESEEKS) {
                attempt_end(r, now);
                return 0;
            }
            wind_back(r, now, r->s[i]->pos, 0);
            return 0;
        }
        double reading = r->s[i]->pos - landing;
        learn(&r->st.cycle_s, (now - r->trigger_t) - (reading > 0 ? reading : 0));
        r->matched = 1;
        r->phase = PIN_RETRY_REREADING;
        r->cursor = i;
        merge_into(r, i, n);
        hole_check(r, i);
        slot_free(n);
        return 0;
    }

    /* lined up: the next slot, by key or by position */
    int i = -1;
    if (n->key_valid) {
        i = find_key(r, n->key, r->cursor + 1);
        if (i < 0 && r->s[r->cursor]->key_valid) {
            /* held slots without a key of their own (damage hid it): by the distance from the
             * last keyed one */
            const slot_t *c = r->s[r->cursor];
            double d = key_seconds(r, n->key, c->key);
            double step = c->dur > 0 ? c->dur : 1.0 / r->fps;
            int j = (int)floor(d / step + 0.5);
            if (j >= 1 && r->cursor + j <= r->sec_end && !r->s[r->cursor + j]->key_valid) {
                i = r->cursor + j;
                r->s[i]->key = n->key;
                r->s[i]->key_valid = 1;
            }
        }
        if (i < 0) {
            const slot_t *c = r->s[r->cursor];
            const slot_t *nx = r->cursor + 1 <= r->sec_end ? r->s[r->cursor + 1] : NULL;
            if (c->key_valid && key_near(r, n->key, c->key) && key_diff(r, n->key, c->key) > 0 &&
                nx && nx->key_valid && key_diff(r, nx->key, n->key) > 0) {
                /* between two held slots: a frame / GOP lost in the first read */
                n->pos = c->pos + c->dur;
                n->dur = n->dur > 0 ? n->dur : 1.0 / r->fps;
                n->retried = 1;
                n->mended = 2;      /* a unit the first read lost: the session counted it dropped */
                slot_spool(r, n);
                if (fifo_insert(r, r->cursor + 1, n) == 0) {
                    r->sec_end++;
                    if (r->first_bad > r->cursor) r->first_bad++;
                    if (r->last_bad > r->cursor) r->last_bad++;
                    r->cursor++;
                    r->mended_now++;
                    r->st.frames_mended++;
                    hole_check(r, r->cursor + 1);
                    return 0;
                }
                slot_free(n);
                return 0;
            }
            int lv;
            int64_t last = section_last_key(r, &lv);
            if (r->cursor >= r->sec_end || (lv && key_near(r, n->key, last) && key_diff(r, n->key, last) > 0))
                return 1;   /* past the section */
            slot_free(n);   /* a stray key: drop it */
            return 0;
        }
    } else {
        if (r->cursor + 1 > r->sec_end)
            return 1;
        i = r->cursor + 1;
    }
    merge_into(r, i, n);
    if (i > r->cursor)
        r->cursor = i;
    hole_check(r, i);
    slot_free(n);
    return 0;
}

static void reread_push(pin_retry_t *r, const pin_retry_unit_t *u, double now)
{
    if (!r->matched && r->phase == PIN_RETRY_SEEKING) {
        /* A unit that continues the held tape is one the deck sent before it started winding (it
         * plays on for a moment after REW), arriving late: it is the next frame of the tape, not
         * where the deck landed. */
        int cont = 0;
        if (!r->tail_ok || !r->n)
            cont = 0;
        else if (!r->hdv)
            cont = u->key_valid && next_after_tail(r, u->key);
        else if (u->dts_valid && r->tail_dts_valid)
            cont = r->seek_tail = dts_follows(r, r->tail_dts, u->dts);
        else if (u->opens_gop)
            cont = r->seek_tail = u->key_valid && next_after_tail(r, u->key);
        else
            cont = r->seek_tail;
        if (cont) {
            append_live(r, u, now);
            tail_fix(r);
            return;
        }
    }
    if (!r->hdv) {
        slot_t *n = make_slot(r, u, now, 0);
        if (!n) return;
        n->read = r->read;
        if (reread_slot(r, n, now)) {
            /* the first frame after the section: wind again, or carry on */
            slot_free(n);
            if (r->phase != PIN_RETRY_IDLE)
                attempt_end(r, now);
            append_live(r, u, now);
            if (r->phase == PIN_RETRY_REWINDING)
                tail_fix(r);
            else
                holding_check(r, now, 0.0);
        }
        return;
    }
    /* HDV: assemble the re-read GOP first */
    if (r->pend && u->opens_gop && r->pend->n) {
        slot_t *g = r->pend;
        r->pend = NULL;
        g->complete = 1;
        int past = reread_slot(r, g, now);
        if (past) {
            if (r->phase != PIN_RETRY_IDLE)
                attempt_end(r, now);
            /* the GOP is new tape (or the tail before the next wind): it goes live */
            g->pos = r->pos;
            g->retried = 0;
            r->pos += g->dur;
            slot_spool(r, g);
            if (fifo_insert(r, r->n, g) != 0)
                slot_free(g);
            else
                judge_slot(r, r->n - 1, now);
            append_live(r, u, now);
            if (r->phase == PIN_RETRY_REWINDING)
                tail_fix(r);
            return;
        }
        if (r->phase == PIN_RETRY_REWINDING) {
            /* reread_slot() decided to wind again: this unit was read before the wind */
            return;
        }
    }
    if (r->pend && u->dts_valid && r->pend->ldts_valid && !dts_follows(r, r->pend->ldts, u->dts)) {
        /* the deck jumped (a stale picture, the wind taking effect): what was assembled is not
         * one stretch of tape */
        slot_free(r->pend);
        r->pend = NULL;
        if (!u->opens_gop)
            return;
    }
    if (!r->pend) {
        r->pend = make_slot(r, u, now, 0);
        if (r->pend) r->pend->read = r->read;
        return;
    }
    if (slot_add(r, r->pend, u, 0) != 0)
        return;
    if (u->picture) {
        r->pend->pics++;
        r->pend->dur += 1.0 / r->fps;
    }
    if (u->damaged) r->pend->damaged++;
    if (u->lost_before) r->pend->hole = 1;
    if (u->dts_valid) {
        r->pend->ldts_valid = 1;
        r->pend->ldts = u->dts;
    }
}

void pin_retry_push(pin_retry_t *r, double now, const pin_retry_unit_t *u)
{
    if (!r || !u || !u->data)
        return;
    if (u->fps > 1 && u->fps < 100)
        r->fps = u->fps;
    if (u->hdv != r->hdv) {
        /* DV <-> HDV: a new stream, nothing to line up across it */
        if (r->phase != PIN_RETRY_IDLE)
            section_close(r);
        pin_retry_flush(r);
        r->hdv = u->hdv;
        r->prev_len = 0;
    }
    r->last_push = now;
    switch (r->phase) {
    case PIN_RETRY_IDLE:
    case PIN_RETRY_HOLDING:
        append_live(r, u, now);
        holding_check(r, now, 0.0);
        break;
    case PIN_RETRY_REWINDING:
        if (continues_tail(r, u)) {
            append_live(r, u, now);
            if (r->pos - r->rew_from > (r->whole ? 120.0 : r->cfg.window_s * 0.5 + 30.0)) {
                /* the deck goes on playing (a REW it ignored): give up this section */
                r->st.sections_failed++;
                section_close(r);
            } else {
                tail_fix(r);
            }
        } else {
            r->tail_ok = 0;
            r->prev_len = 0;
        }
        break;
    case PIN_RETRY_SEEKING:
    case PIN_RETRY_REREADING:
        reread_push(r, u, now);
        break;
    }
    release_ready(r);
}

/* ---- the clock and the deck ------------------------------------------- */

pin_retry_action_t pin_retry_tick(pin_retry_t *r, double now, const pin_retry_deck_t *d)
{
    if (!r)
        return PIN_RETRY_ACT_NONE;
    pin_retry_deck_t none;
    if (!d) {
        memset(&none, 0, sizeof(none));
        none.signal = 1;
        none.can_send = 1;
        d = &none;
    }
    if (d->tc_valid) {
        /* the deck answers TIME CODE while it winds: it has started moving back */
        if (r->phase == PIN_RETRY_REWINDING && r->rew_t > 0 && r->rew_tc_valid &&
            d->tc_frames < r->rew_tc)
            r->tc_seen = 1;
        if (r->phase == PIN_RETRY_REWINDING && r->rew_t > 0) {
            /* the deck says where it is while it winds: its speed, measured */
            if (r->wtc_valid && d->tc_frames < r->wtc && now - r->wtc_t > 0.2) {
                double v = (double)(r->wtc - d->tc_frames) / key_rate_tc(r) / (now - r->wtc_t);
                if (v > 1.5 && v < 400)
                    learn(&r->st.rew_rate, v);
            }
            if (!r->wtc_valid || d->tc_frames != r->wtc) {
                r->wtc = d->tc_frames;
                r->wtc_t = now;
                r->wtc_valid = 1;
            }
        }
        r->deck_tc = d->tc_frames;
        r->deck_tc_t = now;
        r->deck_tc_valid = 1;
    }

    switch (r->phase) {
    case PIN_RETRY_IDLE:
        break;
    case PIN_RETRY_HOLDING:
        if (r->n && !d->signal && now - r->last_push > NOSIG_TRIGGER_S && r->hdv &&
            !r->s[r->n - 1]->complete) {
            /* the last GOP will get no successor: judge it now */
            r->s[r->n - 1]->complete = 1;
            judge_slot(r, r->n - 1, now);
        }
        holding_check(r, now, d->signal ? 0.0 : now - r->last_push);
        break;
    case PIN_RETRY_REWINDING:
        if (r->act != PIN_RETRY_ACT_NONE)
            break;
        if (d->cmd_failed) {
            if (r->play_t > 0 && !r->play_failed) {
                /* PLAY straight from winding refused: stop first */
                r->play_failed = 1;
                r->act = PIN_RETRY_ACT_STOP;
                break;
            }
            r->st.sections_failed++;
            section_close(r);
            break;
        }
        if (r->rew_t <= 0)
            break;
        if (r->wind_start_t <= 0)
            r->wind_start_t = r->rew_t + REW_LATENCY_S;
        {
            int go = 0;
            const slot_t *t = r->s[r->sec_start];
            int64_t target = 0;
            int tv = 0;
            if (!r->hdv && t->key_valid) { target = t->key; tv = 1; }
            else if (t->dtc_valid) { target = t->dtc; tv = 1; }
            /* PLAY goes out early by the stop lead (the deck winds on until it takes it) */
            double lead = r->st.stop_lead_s * rew_rate(r);
            if (r->tc_seen && tv && d->tc_valid && now - r->deck_tc_t < 1.0) {
                double ahead = (double)(d->tc_frames - target) / key_rate_tc(r);
                /* where it will be at the next answer: only with a measured speed (a guess that
                 * is too high stops it short, which costs another wind) */
                double v = r->st.rew_rate > 0 ? r->st.rew_rate : 0;
                double step = (now - r->deck_tc_t + 0.2) * v;
                if (v <= 0)
                    lead = 0;
                if (ahead - lead - step <= -r->cfg.preroll_s)
                    go = 1;
            } else if (now > r->wind_start_t &&
                       (now - r->wind_start_t) * rew_rate(r) + lead >= r->dist_s) {
                go = 1;
            }
            double max_rew = r->dist_s / 2 + 60 > MAX_REW_S ? r->dist_s / 2 + 60 : MAX_REW_S;
            /* Not before the deck is winding: it plays on for a moment after REW, and a PLAY in that
             * moment leaves the tape where it is. Its TIME CODE going back shows it; a deck that
             * does not answer while winding gets a second. */
            if (go && !r->tc_seen && (r->rew_tc_valid || now - r->rew_t < 1.0) && now - r->rew_t < 3.0) {
                go = 0;
                r->gated = 1;
            }
            if (d->at_start || now - r->rew_t > max_rew)
                go = 1;
            if (go) {
                r->act = PIN_RETRY_ACT_PLAY;
                r->phase = PIN_RETRY_SEEKING;
                r->seek_s = 0;
                r->prev_len = 0;
            }
        }
        break;
    case PIN_RETRY_SEEKING:
        if (r->act == PIN_RETRY_ACT_NONE && d->cmd_failed && r->play_t > 0 && !r->play_failed) {
            r->play_failed = 1;
            r->act = PIN_RETRY_ACT_STOP;
            break;
        }
        if (r->play_t > 0 && r->seek_s == 0 && now - r->play_t > 6.0 && r->act == PIN_RETRY_ACT_NONE &&
            !d->signal) {
            /* Nothing at all since PLAY: the deck landed in blank tape after the recording (it
             * played on into it before the wind). Back again, by that much more. */
            if (r->reseeks++ >= MAX_RESEEKS) {
                r->st.sections_failed++;
                section_close(r);
                break;
            }
            const slot_t *e = r->s[r->sec_end];
            wind_back(r, now, e->pos + e->dur + (now - r->play_t) + r->dist_s, 0);
            break;
        }
        if (r->play_t > 0 && now - r->play_t > 30.0 + 2 * r->dist_s) {
            /* never found the section: let it go as it is */
            r->st.sections_failed++;
            section_close(r);
        }
        break;
    case PIN_RETRY_REREADING:
        if (now - r->last_push > REREAD_NOSIG_S)
            attempt_end(r, now);
        break;
    }

    pin_retry_action_t a = PIN_RETRY_ACT_NONE;
    if (r->act != PIN_RETRY_ACT_NONE && d->can_send) {
        a = r->act;
        r->act = PIN_RETRY_ACT_NONE;
        if (a == PIN_RETRY_ACT_REW) {
            r->rew_t = now;
            r->rew_tc = r->deck_tc;
            r->rew_tc_valid = r->deck_tc_valid && now - r->deck_tc_t < 3.0;
            r->st.rewinds++;
            r->read++;
        } else if (a == PIN_RETRY_ACT_PLAY) {
            r->play_t = now;
            r->last_push = now;
        } else if (a == PIN_RETRY_ACT_STOP) {
            r->act = PIN_RETRY_ACT_PLAY;   /* PLAY follows once STOP went out */
        }
    }
    release_ready(r);
    return a;
}

void pin_retry_flush(pin_retry_t *r)
{
    if (!r)
        return;
    if (r->phase != PIN_RETRY_IDLE)
        section_close(r);
    size_t b = r->budget;
    r->budget = SIZE_MAX;
    if (r->n && r->hdv)
        r->s[r->n - 1]->complete = 1;
    release_front(r, r->n);
    r->budget = b;
    r->pos = 0;
    r->prev_len = 0;
    r->lost_pending = 0;
}

int pin_retry_flush_some(pin_retry_t *r)
{
    if (!r)
        return 0;
    if (r->phase != PIN_RETRY_IDLE)
        section_close(r);
    if (r->n && r->hdv)
        r->s[r->n - 1]->complete = 1;
    size_t spent = 0;
    int k = 0;
    while (k < r->n) {
        size_t b = r->s[k]->bytes;
        if (r->budget != SIZE_MAX && spent + b > r->budget)
            break;
        spent += b;
        k++;
    }
    if (k)
        release_front(r, k);
    if (r->budget != SIZE_MAX)
        r->budget -= spent < r->budget ? spent : r->budget;
    if (!r->n) {
        r->pos = 0;
        r->prev_len = 0;
        r->lost_pending = 0;
    }
    return r->n;
}

void pin_retry_get_stats(const pin_retry_t *r, pin_retry_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!r)
        return;
    *out = r->st;
    out->phase = r->phase;
    out->attempt = r->phase == PIN_RETRY_IDLE || r->phase == PIN_RETRY_HOLDING ? 0 : r->attempt;
    out->attempts = r->cfg.attempts;
    out->held_s = r->n ? r->pos - r->s[0]->pos : 0;
    if (r->phase != PIN_RETRY_IDLE && r->n && r->sec_start < r->n) {
        int e = r->sec_end < r->n ? r->sec_end : r->n - 1;
        out->section_s = r->s[e]->pos + r->s[e]->dur - r->s[r->sec_start]->pos;
        int f;
        out->bad_slots = section_bad(r, &f);
    }
    out->join_s = join_gap(r);
    out->whole = r->whole;
    out->spooling = r->sf != NULL && spool_wanted(r);
    out->spool_bytes = r->send > 0 ? (uint64_t)r->send : 0;
    out->spool_failed = r->spool_failed;
}

/* ---- HDV continuity counters ------------------------------------------ */

void pin_retry_cc_fix_init(pin_retry_cc_fix_t *f)
{
    memset(f->last, -1, sizeof(f->last));
    memset(f->off, 0, sizeof(f->off));
    memset(f->gen, 0, sizeof(f->gen));
    f->cur = 0;
}

void pin_retry_cc_fix_apply(pin_retry_cc_fix_t *f, uint8_t *ts, size_t n, unsigned read)
{
    uint32_t g = read + 1;
    if (f->cur != g)
        f->cur = g;     /* a seam: each PID takes a new offset at its next packet */
    for (size_t i = 0; i < n; i++) {
        uint8_t *p = ts + i * 188;
        if (p[0] != 0x47)
            continue;
        int pid = ((p[1] & 0x1F) << 8) | p[2];
        if (pid == 0x1FFF)
            continue;
        int afc = (p[3] >> 4) & 3;
        int payload = afc & 1;
        int cc = p[3] & 0x0F;
        if (f->gen[pid] != g) {
            /* first packet of this PID in this read: continue from what was written */
            if (f->last[pid] >= 0 && payload)
                f->off[pid] = (int8_t)(((f->last[pid] + 1) - cc) & 0x0F);
            else if (f->last[pid] >= 0)
                f->off[pid] = (int8_t)((f->last[pid] - cc) & 0x0F);
            else
                f->off[pid] = 0;
            f->gen[pid] = g;
        }
        int ncc = (cc + f->off[pid]) & 0x0F;
        p[3] = (uint8_t)((p[3] & 0xF0) | ncc);
        f->last[pid] = (int8_t)ncc;
    }
}
