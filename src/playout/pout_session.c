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

#include "pout_session.h"

#include "pin_log.h"
#include "pout_switch.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

#define DEFAULT_MAX_SECONDS (48.0 * 3600.0)
#define CYCLE_SECONDS 125e-6

struct pout_session {
    pout_session_cfg_t cfg;
    pout_deck_t deck;
    pout_cip_t cip;
    pout_seq_t seq;
    uint8_t *buf;
    size_t cap;
    int hdv;                    /* the stream's current format is HDV */
    pout_frame_fmt_t fmt;       /* the format the stream (cip) is in now */
    struct {                    /* a format switch under way, see switch_step() */
        int active;
        int phase;              /* SW_GAP or SW_BLANK */
        long remain;            /* bus cycles left in the phase */
        pout_frame_fmt_t to;
        pout_switch_policy_t pol;
    } sw;
    double t;                   /* simulated clock */
    double wall0;
    int64_t last_frame;         /* frame sent last, -1 none/discontinuity */
    int64_t lag;                /* HDV: frames behind the deck */
    unsigned long frames_sent;
    int tx;
    int read_warned;
    int out_failed;             /* the output refused a packet */
};

static int send_cb(void *user, const uint8_t *pkt, unsigned len)
{
    pout_session_t *s = (pout_session_t *)user;
    int rc = s->cfg.out->packet(s->cfg.out, pkt, len);
    if (rc != 0)
        s->out_failed = 1;
    return rc;
}


/* ---- clock --------------------------------------------------------------------------- */

double pout_wall_seconds(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

void pout_sleep_seconds(double s)
{
    if (s <= 0)
        return;
#if defined(_WIN32)
    Sleep((DWORD)(s * 1000.0 + 0.5));
#else
    struct timespec ts;
    ts.tv_sec = (time_t)s;
    ts.tv_nsec = (long)((s - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
#endif
}

double pout_session_time(pout_session_t *s)
{
    return s->cfg.realtime ? pout_wall_seconds() - s->wall0 : s->t;
}

pout_deck_t *pout_session_deck(pout_session_t *s) { return &s->deck; }
unsigned long pout_session_frames_sent(const pout_session_t *s) { return s->frames_sent; }

/* ---- create / destroy ----------------------------------------------------------------- */

pout_session_t *pout_session_create(const pout_session_cfg_t *cfg, char *err, size_t err_cap)
{
    if (!cfg || !cfg->tape || !cfg->sc || !cfg->out) {
        if (err && err_cap)
            snprintf(err, err_cap, "internal error: incomplete session configuration");
        return NULL;
    }
    pout_session_t *s = (pout_session_t *)calloc(1, sizeof *s);
    if (!s)
        goto nomem;
    s->cfg = cfg[0];
    if (s->cfg.max_seconds <= 0)
        s->cfg.max_seconds = DEFAULT_MAX_SECONDS;
    s->cap = pout_tape_max_frame_bytes(cfg->tape);
    s->buf = (uint8_t *)malloc(s->cap ? s->cap : 1);
    if (!s->buf) {
        free(s);
        goto nomem;
    }
    if (pout_tape_frame_fmt(cfg->tape, 0, &s->fmt) != 0) {
        free(s->buf);
        free(s);
        if (err && err_cap)
            snprintf(err, err_cap, "the tape has no frames");
        return NULL;
    }
    s->hdv = s->fmt.media == POUT_MEDIA_HDV;
    pout_cip_init(&s->cip, s->hdv ? POUT_CIP_HDV : POUT_CIP_DV, s->fmt.is_625, 0);
    s->last_frame = -1;
    return s;
nomem:
    if (err && err_cap)
        snprintf(err, err_cap, "out of memory");
    return NULL;
}

void pout_session_destroy(pout_session_t *s)
{
    if (!s)
        return;
    free(s->buf);
    free(s);
}

/* ---- one frame period ------------------------------------------------------------------ */

static int blank_period(pout_session_t *s)
{
    int n = pout_cip_blank(&s->cip, send_cb, s);
    return s->out_failed ? -1 : n;
}

static int read_and_send(pout_session_t *s, int64_t frame)
{
    size_t len = 0;
    if (pout_tape_read_frame(s->cfg.tape, frame, s->buf, s->cap, &len) != 0) {
        if (!s->read_warned) {
            pin_logf(PIN_LOG_WARN, "playout: cannot read tape frame %lld, sending nothing for it\n",
                     (long long)frame);
            s->read_warned = 1;
        }
        return blank_period(s);
    }
    int n = pout_cip_frame(&s->cip, s->buf, len, send_cb, s);
    if (s->out_failed)
        return -1;
    if (n < 0) {
        /* malformed frame (a short DV frame, too many HDV packets for a period): the period
         * is filled with empty packets instead and the run goes on */
        if (!s->read_warned) {
            pin_logf(PIN_LOG_WARN, "playout: tape frame %lld (%zu bytes) cannot be packetized\n",
                     (long long)frame, len);
            s->read_warned = 1;
        }
        return blank_period(s);
    }
    s->frames_sent++;
    return n;
}

/* ---- format switches ------------------------------------------------------------------------- */

/* When the next frame to send is in another format than the stream (pout_switch.h), the stream
 * goes dark for the policy's gap_s (nothing is produced, only the bus cycles pass), then the cip is
 * started again in the new format and sends blank_s of empty packets, then the frames go on. The
 * deck plays on throughout (pout_deck_next_output is called every period as always): the tape
 * frames of the gap and the blank time are lost, like a real tape running under the heads. The
 * cycle count of the stream runs on through the switch; the DBC starts from 0 in the new format. */
enum { SW_GAP = 1, SW_BLANK };

static int discard_cb(void *user, const uint8_t *pkt, unsigned len)
{
    (void)user;
    (void)pkt;
    (void)len;
    return 0;
}

static void fmt_name(const pout_frame_fmt_t *f, char *buf, size_t cap)
{
    if (f->media == POUT_MEDIA_DV)
        snprintf(buf, cap, "DV %s", f->is_625 ? "PAL" : "NTSC");
    else
        snprintf(buf, cap, "HDV %s%d", f->hdv_720p ? "720p" : "1080i", f->is_625 ? 50 : 60);
}

static long cycles_of(double seconds)
{
    return seconds <= 0 ? 0 : (long)floor(seconds / CYCLE_SECONDS + 0.5);
}

static void switch_begin(pout_session_t *s, const pout_frame_fmt_t *to, pout_switch_kind_t kind)
{
    const pout_switch_policy_t *pol = pout_switch_policy(kind);
    char a[32], b[32];
    fmt_name(&s->fmt, a, sizeof a);
    fmt_name(to, b, sizeof b);
    pin_logf(PIN_LOG_INFO, "playout: the tape changes from %s to %s (%s): %.2f s without packets, then %.2f s "
             "of empty ones\n", a, b, pout_switch_name(kind), pol->gap_s, pol->blank_s);
    s->sw.active = 1;
    s->sw.phase = SW_GAP;
    s->sw.remain = cycles_of(pol->gap_s);
    s->sw.to = *to;
    s->sw.pol = *pol;
    s->last_frame = -1;
    if (s->cfg.hooks.format_switch)
        s->cfg.hooks.format_switch(s->cfg.user, 0, to, pol, 1, &s->cip);
}

/* The stream is in the new format from here on. streaming 0: the switch is cut short because the
 * stream stopped (nobody receiving, bus reset); the hook then does not restart anything. */
static void switch_apply(pout_session_t *s, int streaming)
{
    uint32_t cyc = pout_cip_cycle(&s->cip);
    unsigned sid = s->cip.sid;
    const pout_frame_fmt_t *to = &s->sw.to;
    pout_cip_init(&s->cip, to->media == POUT_MEDIA_HDV ? POUT_CIP_HDV : POUT_CIP_DV, to->is_625, sid);
    pout_cip_set_cycle(&s->cip, cyc);
    s->fmt = *to;
    s->hdv = to->media == POUT_MEDIA_HDV;
    s->last_frame = -1;
    if (s->cfg.hooks.format_switch)
        s->cfg.hooks.format_switch(s->cfg.user, 1, to, &s->sw.pol, streaming, &s->cip);
    if (s->cfg.out->format)
        s->cfg.out->format(s->cfg.out, to);
    s->sw.phase = SW_BLANK;
    s->sw.remain = cycles_of(s->sw.pol.blank_s);
}

/* One period of a switch under way. *consumed = 0: the switch is over and this period is free for
 * a frame. Returns the cycles of the period (or -1 if the output failed). */
static int switch_step(pout_session_t *s, int *consumed)
{
    *consumed = 1;
    if (s->sw.phase == SW_GAP) {
        if (s->sw.remain > 0) {
            int n = pout_cip_blank(&s->cip, discard_cb, NULL);   /* the cycles pass, nothing is sent */
            s->sw.remain -= n;
            return n;
        }
        switch_apply(s, 1);
    }
    if (s->sw.remain > 0) {
        int n = blank_period(s);
        if (n > 0)
            s->sw.remain -= n;
        return n;
    }
    s->sw.active = 0;
    *consumed = 0;
    return 0;
}

/* The stream stops (nobody receiving): a switch under way ends at once. */
static void switch_abort(pout_session_t *s)
{
    if (!s->sw.active)
        return;
    if (s->sw.phase == SW_GAP)
        switch_apply(s, 0);
    s->sw.active = 0;
}

/* Returns packets sent (or 0 when tx is off), -1 if the output failed. */
static int player_period(pout_session_t *s, double now, int tx)
{
    int64_t f = 0;
    pout_out_kind_t k = pout_deck_next_output(&s->deck, now, &f);
    if (!tx)
        return 0;
    pout_frame_fmt_t ff;
    int is_frame = 0;
    if ((k == POUT_OUT_FRAME || k == POUT_OUT_REPEAT) && pout_tape_frame_fmt(s->cfg.tape, f, &ff) == 0)
        is_frame = k == POUT_OUT_FRAME || ff.media == POUT_MEDIA_DV;
    for (int again = 0; again < 2; again++) {
        if (!s->sw.active && is_frame) {
            pout_switch_kind_t kind = pout_switch_classify(&s->fmt, &ff);
            if (kind != POUT_SWITCH_NONE)
                switch_begin(s, &ff, kind);
        }
        if (!s->sw.active)
            break;
        int consumed;
        int n = switch_step(s, &consumed);
        if (consumed || n < 0)
            return n;
        /* the switch ended with this period: check the frame against the new format */
    }
    switch (k) {
    case POUT_OUT_FRAME: {
        int64_t src = f;
        if (s->hdv) {
            if (s->last_frame < 0 || f != s->last_frame + 1)
                s->lag = f - pout_tape_play_start(s->cfg.tape, f);
            src = f - s->lag;
            if (src < 0)
                src = 0;
        }
        s->last_frame = f;
        return read_and_send(s, src);
    }
    case POUT_OUT_REPEAT:
        if (!s->hdv)
            return read_and_send(s, f);
        return blank_period(s);
    default:
        s->last_frame = -1;
        return blank_period(s);
    }
}

/* ---- the sequencer ---------------------------------------------------------------------- */

static void observe(pout_session_t *s, double now, pout_obs_t *o)
{
    memset(o, 0, sizeof(*o));
    o->now = now;
    o->state = pout_deck_state(&s->deck, now);
    o->picture = pout_deck_picture(&s->deck, now);
    o->tc_valid = pout_deck_timecode(&s->deck, now, &o->tc) == 0;
    o->fps = pout_tape_fps(s->cfg.tape);
}

/* 0 = go on, 1 = script done, -1 = failed (message in why) */
static int seq_poll(pout_session_t *s, double now, char *why, size_t why_cap)
{
    for (int guard = 0; guard < 64; guard++) {
        pout_obs_t o;
        pout_seq_out_t out;
        observe(s, now, &o);
        pout_seq_status_t st = pout_seq_step(&s->seq, &o, &out);
        if (st == POUT_SEQ_WAITING)
            return 0;
        if (st == POUT_SEQ_DONE)
            return 1;
        if (st == POUT_SEQ_FAILED) {
            snprintf(why, why_cap, "%s", out.why[0] ? out.why : "a wait failed");
            return -1;
        }
        int rc = 0;
        switch (out.cmd) {
        case POUT_ACT_PLAY: rc = pout_deck_play(&s->deck, now); break;
        case POUT_ACT_PAUSE: rc = pout_deck_pause(&s->deck, now); break;
        case POUT_ACT_STOP: rc = pout_deck_stop(&s->deck, now); break;
        case POUT_ACT_FF: rc = pout_deck_ff(&s->deck, now); break;
        case POUT_ACT_REW: rc = pout_deck_rew(&s->deck, now); break;
        case POUT_ACT_GOTO: rc = pout_deck_search_tc(&s->deck, now, &out.tc); break;
        default: break;
        }
        if (rc != 0) {
            char tcs[32] = "";
            if (out.cmd == POUT_ACT_GOTO)
                pin_tc_format(&out.tc, tcs, sizeof(tcs));
            pout_seq_out_t f;
            char msg[160];
            snprintf(msg, sizeof(msg), out.cmd == POUT_ACT_GOTO ? "timecode %s is not on the tape"
                                                                  : "the deck cannot do that",
                     tcs);
            pout_seq_command_failed(&s->seq, msg, &f);
            snprintf(why, why_cap, "%s", f.why);
            return -1;
        }
    }
    return 0;
}

/* ---- status ----------------------------------------------------------------------------- */

static const char *state_name(pout_deck_mode_t m)
{
    switch (m) {
    case POUT_DECK_PLAY: return "PLAY";
    case POUT_DECK_PAUSE: return "PAUSE";
    case POUT_DECK_FF: return "FF";
    case POUT_DECK_REW: return "REW";
    default: return "STOP";
    }
}

void pout_session_status_line(pout_session_t *s, char *buf, size_t cap)
{
    double now = pout_session_time(s);
    pin_tc_t tc;
    char tcs[32] = "--:--:--:--";
    if (pout_deck_timecode(&s->deck, now, &tc) == 0)
        pin_tc_format(&tc, tcs, sizeof(tcs));
    int idx = 0, nfiles = pout_tape_file_count(s->cfg.tape);
    size_t n = (size_t)snprintf(buf, cap, "%-5s %s", state_name(pout_deck_state(&s->deck, now)), tcs);
    if (pout_tape_file_of_frame(s->cfg.tape, pout_deck_position(&s->deck, now), &idx) == 0 && n < cap) {
        const char *p = pout_tape_file_path(s->cfg.tape, idx);
        const char *b = p ? p : "";
        for (const char *c = b; *c; c++)
            if (*c == '/' || *c == '\\')
                b = c + 1;
        n += (size_t)snprintf(buf + n, cap - n, "  file %d/%d %s", idx + 1, nfiles, b);
    }
    if (n < cap)
        n += (size_t)snprintf(buf + n, cap - n, "  frames sent %lu", s->frames_sent);
    if (n < cap && s->cfg.hooks.status_extra) {
        char extra[96] = "";
        s->cfg.hooks.status_extra(s->cfg.user, extra, sizeof(extra));
        if (extra[0])
            snprintf(buf + n, cap - n, "  %s", extra);
    }
}

/* ---- run ---------------------------------------------------------------------------------- */

static void tx_off(pout_session_t *s)
{
    if (!s->tx)
        return;
    switch_abort(s);
    s->cfg.out->stop(s->cfg.out);
    if (s->cfg.hooks.tx_changed)
        s->cfg.hooks.tx_changed(s->cfg.user, 0, &s->cip);
    s->tx = 0;
}

int pout_session_run(pout_session_t *s, char *err, size_t err_cap)
{
    const pout_script_t *sc = s->cfg.sc;
    const pout_session_hooks_t *h = &s->cfg.hooks;
    pout_tape_ops_t ops;
    pout_tape_ops(s->cfg.tape, &ops);

    s->wall0 = pout_wall_seconds();
    s->t = 0;
    double now = pout_session_time(s);
    pout_deck_init(&s->deck, &ops, &sc->timing, now);
    pout_seq_init(&s->seq, sc);
    int whole = sc->nactions == 0 && !s->cfg.serve;
    if (whole)
        pout_deck_play(&s->deck, now);

    int code = POUT_EXIT_OK;
    double deadline = now;
    double last_status = pout_wall_seconds();
    char line[320];

    for (;;) {
        now = pout_session_time(s);
        if (h->interrupted && h->interrupted(s->cfg.user)) {
            code = POUT_EXIT_CANCELLED;
            break;
        }
        if (h->failed && h->failed(s->cfg.user, err, err_cap)) {
            code = POUT_EXIT_DEVICE;
            break;
        }
        int want = h->tx_wanted ? h->tx_wanted(s->cfg.user) : 1;
        if (want && !s->tx) {
            if (h->tx_changed)
                h->tx_changed(s->cfg.user, 1, &s->cip);
            if (s->cfg.out->start(s->cfg.out) != 0) {
                snprintf(err, err_cap, "cannot start the output");
                code = POUT_EXIT_DEVICE;
                break;
            }
            s->tx = 1;
            s->last_frame = -1;
            if (s->cfg.out->format)
                s->cfg.out->format(s->cfg.out, &s->fmt);
        } else if (!want && s->tx) {
            tx_off(s);
        }

        if (sc->nactions > 0) {
            char why[200] = "";
            int r = seq_poll(s, now, why, sizeof(why));
            if (r < 0) {
                snprintf(err, err_cap, "%s", why);
                code = POUT_EXIT_DECK;
                break;
            }
            if (r > 0 && !sc->stay)
                break;
        } else if (whole && pout_deck_state(&s->deck, now) == POUT_DECK_STOP) {
            break;      /* the tape has played to its end */
        }

        unsigned predicted = pout_cip_next_period(&s->cip);
        int n = player_period(s, now, s->tx);
        int dark = s->sw.active && s->sw.phase == SW_GAP;
        /* the cycles the period really took (a switch can change the frame rate in mid-period) */
        double dt = (double)(n > 0 ? (unsigned)n : predicted) * CYCLE_SECONDS;
        if (n < 0) {
            snprintf(err, err_cap, "the output failed");
            code = POUT_EXIT_DEVICE;
            break;
        }
        if (s->tx && h->period_end)
            h->period_end(s->cfg.user);

        if (!s->cfg.realtime) {
            s->t += dt;
            if (s->t > s->cfg.max_seconds) {
                snprintf(err, err_cap, "the script did not finish within %.0f s of simulated time",
                         s->cfg.max_seconds);
                code = POUT_EXIT_DECK;
                break;
            }
        }
        deadline += dt;
        if (s->cfg.realtime && pout_session_time(s) - deadline > 0.5)
            deadline = pout_session_time(s);        /* a hiccup: do not burst to catch up */

        if (h->status && pout_wall_seconds() - last_status >= 1.0) {
            last_status = pout_wall_seconds();
            pout_session_status_line(s, line, sizeof(line));
            h->status(s->cfg.user, line);
        }
        if (h->between)
            h->between(s->cfg.user, deadline, s->tx);
        else if (s->cfg.realtime)
            pout_sleep_seconds(deadline - pout_session_time(s));
        /* no packets go out in a gap, so nothing else paces the loop (the device's queue does that
         * otherwise): wait for the deadline here, serving the hook meanwhile */
        if (dark && s->tx && s->cfg.realtime && h->between) {
            while (pout_session_time(s) < deadline) {
                h->between(s->cfg.user, deadline, 1);
                if (h->interrupted && h->interrupted(s->cfg.user))
                    break;
            }
        }
    }

    tx_off(s);
    return code;
}
