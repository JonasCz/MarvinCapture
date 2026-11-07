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

/* The virtual deck's transport model: see pout_deck.h for the decisions. */

#include "pout_deck.h"

#include <math.h>
#include <string.h>

void pout_deck_timing_defaults(pout_deck_timing_t *tm)
{
    tm->play_delay = 1.0;
    tm->pause_delay = 0.3;
    tm->wind_delay = 0.5;
    tm->wind_speed = 30.0;
    tm->search_settle = 1.5;
}

void pout_deck_init(pout_deck_t *d, const pout_tape_ops_t *ops, const pout_deck_timing_t *tm, double now)
{
    memset(d, 0, sizeof(*d));
    d->ops = *ops;
    if (tm)
        d->tm = *tm;
    else
        pout_deck_timing_defaults(&d->tm);
    if (d->ops.fps <= 0)
        d->ops.fps = 25.0;
    if (d->tm.wind_speed <= 0)
        d->tm.wind_speed = 30.0;
    d->mode = d->eff = POUT_DECK_STOP;
    d->last = now;
    d->last_out = -1;
}

static int is_wind(pout_deck_mode_t m)
{
    return m == POUT_DECK_FF || m == POUT_DECK_REW || m == POUT_DECK_SEARCH;
}

static double last_frame(const pout_deck_t *d)
{
    return d->ops.frames > 0 ? (double)(d->ops.frames - 1) : 0.0;
}

enum { EV_NONE = 0, EV_SWITCH, EV_REACHED, EV_SETTLED };

/* Brings the mechanism up to `now`, event by event (a mode switch, the end of
 * a wind, the end of a settle), so a long gap between two calls gives the same
 * result as many short ones. PLAY does not move the position (see the header). */
static void update(pout_deck_t *d, double now)
{
    if (now < d->last)
        now = d->last;      /* a clock that steps back changes nothing */
    for (int guard = 0; guard < 16; guard++) {
        double t = now;
        int ev = EV_NONE;
        if (d->eff != d->mode && d->switch_at <= t) {
            t = d->switch_at > d->last ? d->switch_at : d->last;
            ev = EV_SWITCH;
        }
        int winding = is_wind(d->eff) && !d->settling;
        double dir = 0, wfps = d->tm.wind_speed * d->ops.fps, limit = 0;
        if (winding) {
            dir = d->eff == POUT_DECK_REW ? -1.0 : d->eff == POUT_DECK_FF ? 1.0 : (double)d->search_dir;
            limit = d->eff == POUT_DECK_REW ? 0.0 : d->eff == POUT_DECK_FF ? last_frame(d) : (double)d->target;
            double dist = dir > 0 ? limit - d->pos : d->pos - limit;
            if (dist < 0)
                dist = 0;
            double th = d->last + dist / wfps;
            if (th <= t) {
                t = th;
                ev = EV_REACHED;
            }
        } else if (is_wind(d->eff) && d->settling && d->settle_until <= t) {
            t = d->settle_until > d->last ? d->settle_until : d->last;
            ev = EV_SETTLED;
        }
        if (winding && ev != EV_REACHED)
            d->pos += dir * wfps * (t - d->last);
        d->last = t;

        switch (ev) {
        case EV_SWITCH:
            /* play -> pause freezes on the frame that was sent last (the position of PLAY is the
             * next frame to send); resuming sends that frame once more, like a deck's still */
            if (d->eff == POUT_DECK_PLAY && d->mode == POUT_DECK_PAUSE && d->last_out >= 0 &&
                d->last_out == (int64_t)floor(d->pos) - 1)
                d->pos = (double)d->last_out;
            d->eff = d->mode;
            if (d->eff == POUT_DECK_PLAY || d->eff == POUT_DECK_PAUSE)
                d->pos = floor(d->pos);     /* a wind can leave a fraction */
            d->settling = 0;
            d->last_out = -1;
            break;
        case EV_REACHED:
            d->pos = limit;
            if (d->eff == POUT_DECK_SEARCH) {
                d->settling = 1;
                d->settle_until = d->last + d->tm.search_settle;
            } else {
                d->mode = d->eff = POUT_DECK_STOP;     /* end or start of the tape: wind stop */
            }
            break;
        case EV_SETTLED:
            d->mode = d->eff = POUT_DECK_PAUSE;
            d->settling = 0;
            d->pos = (double)d->target;
            break;
        default:
            return;
        }
    }
}

static void set_mode(pout_deck_t *d, double now, pout_deck_mode_t m)
{
    update(d, now);
    if (d->mode == m && m != POUT_DECK_SEARCH)
        return;
    if (is_wind(d->eff)) {      /* a winding deck brakes at once */
        d->eff = POUT_DECK_STOP;
        d->settling = 0;
    }
    double delay = 0;
    if (d->eff != m) {
        switch (m) {
        case POUT_DECK_PLAY:  delay = d->tm.play_delay; break;
        case POUT_DECK_PAUSE: delay = d->eff == POUT_DECK_PLAY ? d->tm.pause_delay : d->tm.play_delay; break;
        case POUT_DECK_STOP:  delay = d->tm.pause_delay; break;
        default:              delay = d->tm.wind_delay; break;
        }
    }
    d->mode = m;
    d->switch_at = d->last + delay;     /* last == now here (update() ran up to it) */
    update(d, now);                     /* a zero delay takes effect now */
}

int pout_deck_play(pout_deck_t *d, double now)  { set_mode(d, now, POUT_DECK_PLAY); return 0; }
int pout_deck_pause(pout_deck_t *d, double now) { set_mode(d, now, POUT_DECK_PAUSE); return 0; }
int pout_deck_stop(pout_deck_t *d, double now)  { set_mode(d, now, POUT_DECK_STOP); return 0; }
int pout_deck_wind_stop(pout_deck_t *d, double now) { return pout_deck_stop(d, now); }
int pout_deck_ff(pout_deck_t *d, double now)    { set_mode(d, now, POUT_DECK_FF); return 0; }
int pout_deck_rew(pout_deck_t *d, double now)   { set_mode(d, now, POUT_DECK_REW); return 0; }

int pout_deck_search_frame(pout_deck_t *d, double now, int64_t frame)
{
    if (d->ops.frames <= 0)
        return -1;
    if (frame < 0)
        frame = 0;
    if (frame > d->ops.frames - 1)
        frame = d->ops.frames - 1;
    update(d, now);
    d->target = frame;
    d->search_dir = (double)frame >= d->pos ? 1 : -1;
    set_mode(d, now, POUT_DECK_SEARCH);
    return 0;
}

int pout_deck_search_tc(pout_deck_t *d, double now, const pin_tc_t *tc)
{
    if (!d->ops.frame_of_tc)
        return -1;
    int64_t f = d->ops.frame_of_tc(d->ops.ctx, tc);
    if (f < 0 || f >= d->ops.frames)
        return -1;
    return pout_deck_search_frame(d, now, f);
}

pout_deck_mode_t pout_deck_state(pout_deck_t *d, double now)
{
    update(d, now);
    if (d->mode == POUT_DECK_SEARCH)
        return d->search_dir < 0 ? POUT_DECK_REW : POUT_DECK_FF;
    return d->mode;
}

int pout_deck_searching(pout_deck_t *d, double now)
{
    update(d, now);
    return d->mode == POUT_DECK_SEARCH;
}

int pout_deck_in_transition(pout_deck_t *d, double now)
{
    update(d, now);
    return d->eff != d->mode || d->mode == POUT_DECK_SEARCH;
}

int pout_deck_moving(pout_deck_t *d, double now)
{
    pout_deck_mode_t s = pout_deck_state(d, now);
    return s == POUT_DECK_PLAY || s == POUT_DECK_FF || s == POUT_DECK_REW;
}

int pout_deck_picture(pout_deck_t *d, double now)
{
    update(d, now);
    return d->eff == POUT_DECK_PLAY;
}

int64_t pout_deck_position(pout_deck_t *d, double now)
{
    update(d, now);
    int64_t f = (int64_t)floor(d->pos);
    if (f > d->ops.frames - 1)
        f = d->ops.frames - 1;
    return f < 0 ? 0 : f;
}

int pout_deck_timecode(pout_deck_t *d, double now, pin_tc_t *tc)
{
    if (!d->ops.tc_of_frame || d->ops.frames <= 0)
        return -1;
    return d->ops.tc_of_frame(d->ops.ctx, pout_deck_position(d, now), tc) == 0 ? 0 : -1;
}

pout_out_kind_t pout_deck_next_output(pout_deck_t *d, double now, int64_t *frame)
{
    update(d, now);
    switch (d->eff) {
    case POUT_DECK_PLAY:
        if (d->pos >= (double)d->ops.frames) {      /* end of the tape: stop, nothing more to send */
            d->mode = d->eff = POUT_DECK_STOP;
            d->pos = last_frame(d);
            return POUT_OUT_BLANK;
        }
        *frame = (int64_t)floor(d->pos);
        d->last_out = *frame;
        d->pos += 1.0;
        return POUT_OUT_FRAME;
    case POUT_DECK_PAUSE:
        if (d->ops.frames <= 0)
            return POUT_OUT_BLANK;
        *frame = pout_deck_position(d, now);
        return POUT_OUT_REPEAT;
    default:
        return POUT_OUT_BLANK;
    }
}
