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
 * MarvinPlayout's virtual deck: the transport of a "tape" as a pure state
 * machine on a caller-supplied clock (double seconds, any monotonic origin),
 * so it is unit-tested with a fake clock and needs no hardware.
 *
 * The deck does not read the tape. It only knows its length, frame rate and
 * the timecode <-> frame mapping (pout_tape_ops_t). The output loop asks it,
 * once per frame period, what to send (pout_deck_next_output); controllers
 * (the AV/C target in pout_avc.c, the script sequencer in pout_script.c)
 * command it and query it. "Last command wins", whoever sent it.
 *
 * DECISION, play is driven by the output calls, not by the clock. In PLAY the
 * position advances by exactly one frame per pout_deck_next_output() call, so
 * consecutive calls give consecutive frames with no skip and no repeat even
 * when the caller's clock jitters (a late loop iteration must not drop a
 * frame; the bus packetizer decides the real pace). The wall clock drives
 * everything else: mode-change delays, winding, search and settle. The
 * consequence: with nothing calling next_output the position in PLAY does not
 * move; the timecode reported in PLAY is that of the next frame to be sent.
 *
 * Timing (docs/handoff/playout-plan.md, all of it a guess until a real
 * receiver has been tried; pout_deck_timing_t is adjustable):
 *   - A command changes the state the controller sees at once, as AV/C decks
 *     do after ACCEPTED, but what comes out follows after the delay: stop or
 *     pause -> play takes play_delay before the first frame, play -> pause or
 *     stop takes pause_delay, a wind starts after wind_delay. Until then the
 *     old behaviour continues (play keeps playing, pause keeps repeating its
 *     frame, a stopped deck stays blank). A winding deck stops at once.
 *   - FF / REW run at wind_speed times real time and stop (state STOP, like a
 *     real deck's WIND stop) at the last frame / frame 0. PLAY at the last
 *     frame ends in STOP.
 *   - Search (timecode or frame) winds to the target at the wind speed, waits
 *     search_settle, and ends PAUSED at the target (like most decks after a
 *     time code search). While it runs the deck reports FF or REW, according
 *     to the direction, and "in transition".
 *   - Pausing from play freezes on the frame sent last; PLAY after it sends that
 *     frame once more (a still, then on).
 */

#ifndef POUT_DECK_H
#define POUT_DECK_H

#include "pout_tape.h"     /* pout_tape_ops_t, pin_tc_t */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double play_delay;      /* stop, pause or wind -> play/pause, until the picture starts (1.0) */
    double pause_delay;     /* play -> pause, play -> stop (0.3) */
    double wind_delay;      /* any -> FF/REW/search, until the winding starts (0.5, a guess) */
    double wind_speed;      /* FF / REW / search, times real time (30) */
    double search_settle;   /* after a search has arrived, before it reports PAUSED (1.5) */
} pout_deck_timing_t;

/* What a controller sees. A search is reported as FF or REW. */
typedef enum {
    POUT_DECK_STOP = 0,
    POUT_DECK_PLAY,
    POUT_DECK_PAUSE,
    POUT_DECK_FF,
    POUT_DECK_REW,
    POUT_DECK_SEARCH,       /* internal only: pout_deck_state() never returns it */
} pout_deck_mode_t;

typedef enum {
    POUT_OUT_BLANK = 0,     /* nothing to send this period (stopped, winding, mode change pending) */
    POUT_OUT_FRAME,         /* send *frame, the next one of the tape */
    POUT_OUT_REPEAT,        /* paused: send *frame again */
} pout_out_kind_t;

typedef struct {
    pout_tape_ops_t ops;
    pout_deck_timing_t tm;
    pout_deck_mode_t mode;      /* private from here on */
    pout_deck_mode_t eff;
    double switch_at;
    double last;
    double pos;
    int64_t target;
    int search_dir;
    int settling;
    double settle_until;
    int64_t last_out;           /* frame sent last in PLAY, -1 none */
} pout_deck_t;

void pout_deck_timing_defaults(pout_deck_timing_t *tm);
/* tm NULL = defaults. The deck starts stopped at frame 0 at time `now`. */
void pout_deck_init(pout_deck_t *d, const pout_tape_ops_t *ops, const pout_deck_timing_t *tm, double now);

/* Commands. All return 0, or -1 if the deck cannot do it (search target not on
 * the tape). PAUSE from a wind or STOP threads the tape and pauses there. */
int pout_deck_play(pout_deck_t *d, double now);
int pout_deck_pause(pout_deck_t *d, double now);
int pout_deck_stop(pout_deck_t *d, double now);
int pout_deck_wind_stop(pout_deck_t *d, double now);    /* same as stop: AV/C stops by WIND/stop */
int pout_deck_ff(pout_deck_t *d, double now);
int pout_deck_rew(pout_deck_t *d, double now);
int pout_deck_search_frame(pout_deck_t *d, double now, int64_t frame);  /* clamped to the tape */
int pout_deck_search_tc(pout_deck_t *d, double now, const pin_tc_t *tc); /* -1: not on the tape */

/* Queries (each one brings the deck up to date at `now`). */
pout_deck_mode_t pout_deck_state(pout_deck_t *d, double now);   /* never POUT_DECK_SEARCH */
int pout_deck_searching(pout_deck_t *d, double now);
/* A mode change is pending, or a search is under way: AV/C says IN_TRANSITION. */
int pout_deck_in_transition(pout_deck_t *d, double now);
/* The tape is going somewhere: reported state is PLAY, FF or REW. */
int pout_deck_moving(pout_deck_t *d, double now);
/* Frames are coming out (the mechanism is really playing, not just told to). */
int pout_deck_picture(pout_deck_t *d, double now);
int64_t pout_deck_position(pout_deck_t *d, double now);                 /* frame under the head */
int pout_deck_timecode(pout_deck_t *d, double now, pin_tc_t *tc);       /* 0 ok, -1 none */

/* Once per frame period. */
pout_out_kind_t pout_deck_next_output(pout_deck_t *d, double now, int64_t *frame);

#ifdef __cplusplus
}
#endif

#endif /* POUT_DECK_H */
