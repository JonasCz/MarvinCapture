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
    if (o->signal)
        t->nosig_since = -1;
    else if (t->nosig_since < 0)
        t->nosig_since = o->now;
}

void pin_script_track_wait_begin(pin_script_track_t *t, const pin_script_obs_t *o)
{
    pin_script_track_observe(t, o);
    t->wait_start = o->now;
    t->nosig_since = o->signal ? -1 : o->now;
}

/* The "idle" rule of docs/cli.md. */
static int idle_met(const pin_script_track_t *t, const pin_script_obs_t *o)
{
    if (!deck_still(o->deck))
        return 0;
    if (!t->transport_valid)
        return 1; /* nothing was started since: already idle */
    if (t->still_since < 0 || o->now - t->still_since < PIN_SCRIPT_IDLE_STABLE_S)
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

pin_eval_result_t pin_script_eval(const pin_script_cond_t *conds, int n, const pin_script_obs_t *o,
                                  pin_script_track_t *t, int *met_index, char *why, size_t why_cap)
{
    char buf[128];
    if (why && why_cap)
        why[0] = 0;
    pin_script_track_observe(t, o);
    for (int i = 0; i < n; i++) {
        const pin_script_cond_t *c = &conds[i];
        if (met_index)
            *met_index = i;
        switch (c->kind) {
        case PIN_COND_IDLE:
            if (deck_unavailable(o, buf, sizeof(buf))) {
                if (why) snprintf(why, why_cap, "%s", buf);
                return PIN_EVAL_ERROR;
            }
            if (idle_met(t, o)) {
                t->transport_valid = 0;
                t->motion_seen = 0;
                return PIN_EVAL_MET;
            }
            break;
        case PIN_COND_SIGNAL:
            if (o->signal)
                return PIN_EVAL_MET;
            break;
        case PIN_COND_NOSIGNAL:
            if (t->nosig_since >= 0 &&
                o->now - t->nosig_since >= pin_tc_seconds(&c->tc, o->fps))
                return PIN_EVAL_MET;
            break;
        case PIN_COND_DURATION:
            if (o->now - t->wait_start >= pin_tc_seconds(&c->tc, o->fps))
                return PIN_EVAL_MET;
            break;
        case PIN_COND_TIMECODE: {
            if (deck_unavailable(o, buf, sizeof(buf))) {
                if (why) snprintf(why, why_cap, "%s", buf);
                return PIN_EVAL_ERROR;
            }
            pin_tc_t cur;
            if (o->timecode[0] && pin_tc_parse(o->timecode, &cur, NULL, 0)) {
                int cmp = pin_tc_compare(&cur, &c->tc);
                if (t->dir_down ? cmp <= 0 : cmp >= 0)
                    return PIN_EVAL_MET;
            }
            if (idle_met(t, o)) {
                char want[16];
                pin_tc_format(&c->tc, want, sizeof(want));
                if (why)
                    snprintf(why, why_cap, "the deck stopped at %s before reaching %s",
                             o->timecode[0] ? o->timecode : "an unknown position", want);
                return PIN_EVAL_ERROR;
            }
            break;
        }
        }
    }
    return PIN_EVAL_PENDING;
}
