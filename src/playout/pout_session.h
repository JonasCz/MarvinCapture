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
 * The playout loop, without any device: every frame period it asks the virtual deck what to send
 * (pout_deck_next_output), reads that frame from the tape, packetizes it (pout_cip) into a
 * pout_out_t, and between periods steps the script sequencer. The same code runs
 *   - with the file output (--dry-run), on a SIMULATED clock that advances one frame period per
 *     period produced (fast, deterministic: the deck's delays still behave as if the time had
 *     passed) or on the wall clock (--realtime),
 *   - on the device (pout_hw_session.c): the wall clock, the 1394 IT queue as the pacemaker, and
 *     hooks that pump the link (AV/C, plug locks), start and stop the stream and watch for bus
 *     resets.
 *
 * Output while the deck is not playing: PLAY sends the tape's frames, PAUSE repeats the paused
 * frame (DV; HDV sends empty packets, nothing repeats in a transport stream), STOP and winding
 * send empty packets. The stream keeps its cycle count going in all cases.
 *
 * HDV lag (GUESS): a receiver needs a GOP start. When the deck starts to play at frame f the
 * player starts at pout_tape_play_start(f) instead and keeps that distance (the "lag") for the
 * whole run of consecutive frames, so what goes out is up to a GOP behind the deck's timecode and
 * the last `lag` frames before the end of the tape are not sent. DV has no lag.
 *
 * Time is a double in seconds from an arbitrary origin; the session never reads the wall clock
 * unless realtime is set or a hook says so, so it is testable.
 */

#ifndef POUT_SESSION_H
#define POUT_SESSION_H

#include "pout_cip.h"
#include "pout_deck.h"
#include "pout_out.h"
#include "pout_script.h"
#include "pout_tape.h"

#ifdef __cplusplus
extern "C" {
#endif

/* All hooks are optional; `u` is cfg.user. */
typedef struct {
    /* nonzero: stop now (Ctrl-C). The run ends with POUT_EXIT_CANCELLED. */
    int (*interrupted)(void *u);
    /* nonzero: a fatal device error, described in msg. The run ends with POUT_EXIT_DEVICE. */
    int (*failed)(void *u, char *msg, size_t cap);
    /* Is anybody to receive the stream? Default: yes, always. A change from 0 to 1 calls
     * tx_changed(1), then out->start(); from 1 to 0 out->stop(), then tx_changed(0). While 0 the
     * deck keeps running (a playing tape runs on) but nothing is read or sent. */
    int (*tx_wanted)(void *u);
    /* The stream is about to start (on = 1: set the cip's sid and cycle here) or has stopped. */
    void (*tx_changed)(void *u, int on, pout_cip_t *cip);
    /* One frame period has been handed to the output (flush it). */
    void (*period_end)(void *u);
    /* After each period; the session time of the next period is `deadline`. Default with
     * realtime: sleep until the deadline; simulated: nothing. tx says whether a period was sent.
     * The device uses this to pump the link instead (its output queue paces the loop). */
    void (*between)(void *u, double deadline, int tx);
    /* One status line a second (wall time), without a newline. */
    void (*status)(void *u, const char *line);
    /* Appended to the status line ("controller node 1"). */
    void (*status_extra)(void *u, char *buf, size_t cap);
} pout_session_hooks_t;

typedef struct {
    pout_tape_t *tape;              /* not owned */
    const pout_script_t *sc;        /* timing, actions, stay; not owned */
    pout_out_t *out;                /* not owned; start/stop are called by the session */
    int realtime;                   /* wall clock instead of the simulated one */
    /* no actions: 0 = play the whole tape once from the start and finish; 1 = serve (the deck is
     * driven by hooks/AV/C) until interrupted. With actions: ignored (sc->stay decides). */
    int serve;
    double max_seconds;             /* simulated clock only: give up after this much (default 48 h) */
    pout_session_hooks_t hooks;
    void *user;
} pout_session_cfg_t;

typedef struct pout_session pout_session_t;

/* NULL with a message in err on failure (out of memory, a tape without frames). */
pout_session_t *pout_session_create(const pout_session_cfg_t *cfg, char *err, size_t err_cap);
void pout_session_destroy(pout_session_t *s);

/* Runs to the end of the script (or the tape), failure or interruption. Returns the exit code
 * (POUT_EXIT_*), with a one-line message in err when it is not 0 or 130. */
int pout_session_run(pout_session_t *s, char *err, size_t err_cap);

/* For the hooks (AV/C answers need the deck and the time). The deck is only to be touched from
 * the thread that runs the session, i.e. from inside hooks. */
pout_deck_t *pout_session_deck(pout_session_t *s);
double pout_session_time(pout_session_t *s);   /* the session clock, now */
unsigned long pout_session_frames_sent(const pout_session_t *s);   /* tape frames (also repeats) put on the stream */
/* The status line as it would be printed now. */
void pout_session_status_line(pout_session_t *s, char *buf, size_t cap);

/* Monotonic wall clock in seconds, and a sleep, for the callers that need them. */
double pout_wall_seconds(void);
void pout_sleep_seconds(double s);

#ifdef __cplusplus
}
#endif

#endif /* POUT_SESSION_H */
