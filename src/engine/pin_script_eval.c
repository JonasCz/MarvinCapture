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

#include "pin_script_eval.h"

#include <stdio.h>
#include <string.h>

void pin_script_track_init(pin_script_track_t *t)
{
    memset(t, 0, sizeof(*t));
    t->still_since = -1;
    t->nosig_since = -1;
    t->sig_since = -1;
}

void pin_script_track_transport(pin_script_track_t *t, pin_deck_cmd_t cmd, double now)
{
    t->transport_valid = 1;
    t->transport_cmd = cmd;
    t->transport_t = now;
    t->motion_seen = 0;
    t->still_since = -1; /* what stood still before the command does not count */
    if (cmd == PIN_DECK_CMD_REW)
        t->dir_down = 1;
    else if (cmd == PIN_DECK_CMD_FF || cmd == PIN_DECK_CMD_PLAY)
        t->dir_down = 0;
}

static int deck_moving(pin_deck_state_t d)
{
    return d == PIN_DECK_PLAYING || d == PIN_DECK_FAST_FORWARD || d == PIN_DECK_REWINDING ||
           d == PIN_DECK_RECORDING;
}

static int deck_still(pin_deck_state_t d)
{
    return d == PIN_DECK_STOPPED || d == PIN_DECK_PAUSED;
}

void pin_script_track_observe(pin_script_track_t *t, const pin_script_obs_t *o)
{
    if (deck_moving(o->deck)) {
        if (t->transport_valid)
            t->motion_seen = 1;
        t->still_since = -1;
    } else if (deck_still(o->deck)) {
        if (t->still_since < 0)
            t->still_since = o->now;
    } else {
        t->still_since = -1; /* unknown / no tape: not a settled state */
    }
    if (o->signal) {
        t->nosig_since = -1;
        if (t->sig_since < 0)
            t->sig_since = o->now;
    } else {
        t->sig_since = -1;
        if (t->nosig_since < 0)
            t->nosig_since = o->now;
    }
}

void pin_script_track_wait_begin(pin_script_track_t *t, const pin_script_obs_t *o)
{
    pin_script_track_observe(t, o);
    t->wait_start = o->now;
    t->nosig_since = o->signal ? -1 : o->now;
    t->sig_since = o->signal ? o->now : -1;
    t->latched = 0;
}

/* The "idle" rule of docs/cli.md: the deck is stopped or paused, after motion that
 * followed the last transport command (or long enough after it), and has stood still
 * for `window` seconds, counted inside the current wait. */
static int idle_met(const pin_script_track_t *t, const pin_script_obs_t *o, double window)
{
    if (!deck_still(o->deck))
        return 0;
    if (!t->transport_valid)
        return 1; /* nothing was started since: already idle */
    if (t->still_since < 0)
        return 0;
    double since = t->still_since > t->wait_start ? t->still_since : t->wait_start;
    if (o->now - since < window)
        return 0;
    return t->motion_seen || o->now - t->transport_t >= PIN_SCRIPT_IDLE_NO_MOTION_S;
}

/* Deck conditions need a camera with a tape. Returns 1 and a reason if not. */
static int deck_unavailable(const pin_script_obs_t *o, char *why, size_t cap)
{
    if (o->camera_present == 0) {
        snprintf(why, cap, "no camera or deck on the FireWire bus");
        return 1;
    }
    if (o->deck == PIN_DECK_NO_TAPE) {
        snprintf(why, cap, "no tape in the deck");
        return 1;
    }
    return 0;
}

enum { C_PENDING = 0, C_MET, C_FAILED, C_FAILED_HARD };

/* A state held continuously since `since` (-1 = not held) for dur seconds, inside this wait. */
static int state_held(const pin_script_track_t *t, const pin_script_obs_t *o, double since, double dur)
{
    if (since < 0)
        return 0;
    if (since < t->wait_start)
        since = t->wait_start;
    return o->now - since >= dur;
}

static int cond_check(const pin_script_cond_t *c, const pin_script_obs_t *o, const pin_script_track_t *t,
                      char *why, size_t cap)
{
    double dur = pin_tc_seconds(&c->tc, o->fps);
    switch (c->kind) {
    case PIN_COND_IDLE:
        if (deck_unavailable(o, why, cap))
            return C_FAILED_HARD;
        return idle_met(t, o, dur > PIN_SCRIPT_IDLE_STABLE_S ? dur : PIN_SCRIPT_IDLE_STABLE_S) ? C_MET : C_PENDING;
    case PIN_COND_SIGNAL:
        return state_held(t, o, t->sig_since, dur) ? C_MET : C_PENDING;
    case PIN_COND_NOSIGNAL:
        return state_held(t, o, t->nosig_since, dur) ? C_MET : C_PENDING;
    case PIN_COND_WALLCLOCK:
        return o->now - t->wait_start >= dur ? C_MET : C_PENDING;
    case PIN_COND_CAPTURED:
        /* frames written, as time: it stands still while no frames arrive */
        return o->capture_active && (double)o->capture_frames / (o->fps > 0 ? o->fps : 25.0) >= dur ? C_MET
                                                                                                      : C_PENDING;
    case PIN_COND_TIMECODE: {
        if (deck_unavailable(o, why, cap))
            return C_FAILED_HARD;
        pin_tc_t cur;
        if (o->timecode[0] && pin_tc_parse(o->timecode, &cur, NULL, 0)) {
            int cmp = pin_tc_compare(&cur, &c->tc);
            if (t->dir_down ? cmp <= 0 : cmp >= 0)
                return C_MET;
        }
        if (idle_met(t, o, PIN_SCRIPT_IDLE_STABLE_S)) {
            char want[16];
            pin_tc_format(&c->tc, want, sizeof(want));
            snprintf(why, cap, "the deck stopped at %s before reaching %s",
                     o->timecode[0] ? o->timecode : "an unknown position", want);
            return C_FAILED;
        }
        return C_PENDING;
    }
    }
    return C_PENDING;
}

pin_eval_result_t pin_script_eval(const pin_script_cond_t *conds, int n, int wait_all,
                                  const pin_script_obs_t *o, pin_script_track_t *t, unsigned *met_mask,
                                  char *why, size_t why_cap)
{
    char msg[128], first_soft[128] = "", first_hard[128] = "";
    unsigned met = 0, soft = 0, hard = 0;
    if (why && why_cap)
        why[0] = 0;
    if (met_mask)
        *met_mask = 0;
    pin_script_track_observe(t, o);
    for (int i = 0; i < n; i++) {
        unsigned bit = 1u << i;
        int r;
        msg[0] = 0;
        if (t->latched & bit) {
            r = C_MET;
        } else {
            r = cond_check(&conds[i], o, t, msg, sizeof(msg));
            if (r == C_MET && (conds[i].kind == PIN_COND_WALLCLOCK || conds[i].kind == PIN_COND_TIMECODE ||
                               conds[i].kind == PIN_COND_CAPTURED))
                t->latched |= bit;
        }
        if (r == C_MET) {
            met |= bit;
        } else if (r == C_FAILED_HARD) {
            if (!hard) snprintf(first_hard, sizeof(first_hard), "%s", msg);
            hard |= bit;
        } else if (r == C_FAILED) {
            if (!soft) snprintf(first_soft, sizeof(first_soft), "%s", msg);
            soft |= bit;
        }
    }
    unsigned all_bits = n >= 32 ? ~0u : (1u << n) - 1;
    unsigned failed = hard | soft;
    int done;
    if (wait_all)
        done = failed ? 0 : met == all_bits;
    else
        done = met != 0;
    if (done) {
        for (int i = 0; i < n; i++)
            if ((met & (1u << i)) && conds[i].kind == PIN_COND_IDLE) {
                t->transport_valid = 0;
                t->motion_seen = 0;
                break;
            }
        if (met_mask)
            *met_mask = met;
        return PIN_EVAL_MET;
    }
    /* ALL: any failure; ANY: a deck that is not there, or every condition failed */
    if (wait_all ? failed != 0 : (hard != 0 || failed == all_bits)) {
        if (met_mask)
            *met_mask = failed;
        if (why)
            snprintf(why, why_cap, "%s", hard ? first_hard : first_soft);
        return PIN_EVAL_ERROR;
    }
    return PIN_EVAL_PENDING;
}
