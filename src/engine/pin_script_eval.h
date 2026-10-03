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
 * Wait-condition evaluation for the script sequencer (docs/cli.md "Wait
 * conditions"), kept free of the session so it can be unit-tested with made-up
 * observations and a made-up clock.
 *
 * The sequencer feeds it one observation (a few fields of the status
 * snapshot plus "now") per poll, and the tracker remembers what happened
 * between polls: when the last transport command went out, whether the deck
 * moved since, since when it has been standing still, since when there has
 * been no signal.
 */

#ifndef PIN_SCRIPT_EVAL_H
#define PIN_SCRIPT_EVAL_H

#include "pin_script.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The deck must stand still this long to count as idle. */
#define PIN_SCRIPT_IDLE_STABLE_S 3.0
/* A status poll right after a transport command can still say "stopped": with
 * no motion seen, a deck that stays stopped counts as idle this long after the
 * command (the command was a no-op, e.g. REW at the start of the tape). */
#define PIN_SCRIPT_IDLE_NO_MOTION_S 5.0

typedef struct {
    double now;                 /* seconds, any monotonic clock */
    pin_deck_state_t deck;
    int camera_present;         /* pin_status_snapshot_t.camera_present: 1, 0, -1 unknown */
    int signal;
    char timecode[16];          /* deck timecode or "" */
    double fps;                 /* frame rate for durations with frames, <= 0 means 25 */
} pin_script_obs_t;

typedef struct {
    /* since the last transport command (cleared when an idle wait completes) */
    int transport_valid;
    pin_deck_cmd_t transport_cmd;
    double transport_t;
    int motion_seen;
    double still_since;         /* deck stopped/paused continuously since, -1 = not */
    int dir_down;               /* the tape runs backwards (last wind/play command was REW) */
    /* per wait */
    double wait_start;
    double nosig_since;         /* no signal continuously since, -1 = there is signal */
} pin_script_track_t;

typedef enum {
    PIN_EVAL_PENDING = 0,
    PIN_EVAL_MET,
    PIN_EVAL_ERROR,             /* deck error: *why says what (exit code 3) */
} pin_eval_result_t;

void pin_script_track_init(pin_script_track_t *t);
/* A transport command was sent at `now`. */
void pin_script_track_transport(pin_script_track_t *t, pin_deck_cmd_t cmd, double now);
/* Feeds an observation (the evaluator does this itself; call it too while
 * waiting for something else, so motion is not missed). */
void pin_script_track_observe(pin_script_track_t *t, const pin_script_obs_t *o);
/* A wait starts: durations and "nosignal" count from here. */
void pin_script_track_wait_begin(pin_script_track_t *t, const pin_script_obs_t *o);

/* Evaluates the conditions in order; the first that is met (or fails) wins.
 * *met_index is its index when MET or ERROR. An idle condition that is met
 * consumes the transport command (see pin_script_track_t). */
pin_eval_result_t pin_script_eval(const pin_script_cond_t *conds, int n, const pin_script_obs_t *o,
                                  pin_script_track_t *t, int *met_index, char *why, size_t why_cap);

#ifdef __cplusplus
}
#endif

#endif /* PIN_SCRIPT_EVAL_H */
