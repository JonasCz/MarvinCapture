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
 * MarvinPlayout's command line: the same left-to-right language as
 * MarvinCaptureCLI (docs/cli.md), reusing its timecode/duration/condition
 * parsers and its wait evaluator (pin_script.h, pin_script_eval.h), plus the
 * sequencer's pure logic: given what the deck looks like now, which deck
 * command comes next, or is the current wait done, failed, or the script over.
 *
 *   MarvinPlayout [settings] FILE... [settings and actions...]
 *
 * Settings come before the first action, files before the first action too.
 * Of the wait conditions only idle, timecode and wallclock make sense for a
 * deck that plays a file; the others are refused with a message.
 */

#ifndef POUT_SCRIPT_H
#define POUT_SCRIPT_H

#include "pout_deck.h"
#include "pin_script.h"
#include "pin_script_eval.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exit codes (a subset of MarvinCaptureCLI's, same meaning) */
#define POUT_EXIT_OK        0
#define POUT_EXIT_USAGE     1
#define POUT_EXIT_DEVICE    2
#define POUT_EXIT_DECK      3   /* a wait failed, or the deck could not do an action */
#define POUT_EXIT_CANCELLED 130

typedef enum {
    POUT_ACT_PLAY = 0, POUT_ACT_PAUSE, POUT_ACT_STOP, POUT_ACT_FF, POUT_ACT_REW,
    POUT_ACT_GOTO,      /* tc: timecode search */
    POUT_ACT_WAIT,
} pout_act_kind_t;

typedef struct {
    pout_act_kind_t kind;
    pin_tc_t tc;                /* GOTO */
    int wait_all;               /* WAIT: --wait-all, else any */
    int nconds;
    pin_script_cond_t conds[PIN_SCRIPT_MAX_CONDS];  /* WAIT: only idle, timecode, wallclock */
} pout_action_t;

typedef struct pout_script {
    int help;
    int debug;
    char device[PIN_PATH_MAX];  /* -d; "" = first device */
    int has_device;
    int channel;                /* isochronous channel 0..63, default 63 */
    int speed;                  /* bus speed in Mbit/s: 100, 200 or 400 (s100..s400), default 100 */
    int virtual_deck;           /* answer AV/C (default 1; --no-virtual-deck clears it) */
    pout_deck_timing_t timing;  /* defaults, then --wind-speed / --start-delay / --search-settle */
    char dry_run[PIN_PATH_MAX]; /* --dry-run OUT: no device, write what would be sent */
    int has_dry_run;
    int realtime;               /* with --dry-run: pace like the bus */
    int stay;                   /* keep serving the virtual deck after the actions */
    char **files;               /* the tape, in the order given */
    int nfiles;
    pout_action_t *actions;
    int nactions;
} pout_script_t;

/* Parses argv (no program name). With no arguments or -h/--help the result has
 * help set. On a usage error returns PIN_ERR_ARG with a one-line message
 * naming the offending argument. */
pin_status_t pout_script_parse(int argc, const char *const *argv, pout_script_t **out, char *err, size_t err_cap);
void pout_script_destroy(pout_script_t *sc);
/* "play", "goto 00:01:00:00", "wait-any idle=00:00:05" */
void pout_script_action_text(const pout_script_t *sc, int i, char *out, size_t cap);
const char *pout_script_help_text(void);

/* ---- the sequencer ---- */

/* What the session sees of the deck at one poll (about 10 times a second). */
typedef struct {
    double now;
    pout_deck_mode_t state;     /* pout_deck_state(): as reported, a search shows as FF or REW */
    int picture;                /* pout_deck_picture(): frames are going out (the "signal") */
    int tc_valid;
    pin_tc_t tc;                /* pout_deck_timecode() */
    double fps;                 /* the tape's, for durations with frames */
} pout_obs_t;

typedef enum {
    POUT_SEQ_WAITING = 0,       /* nothing to do now (a wait is pending) */
    POUT_SEQ_COMMAND,           /* apply out->cmd (and out->tc for GOTO) to the deck, observe again, step again */
    POUT_SEQ_DONE,              /* the script is over: exit_code 0 (stay serving if --stay or no actions) */
    POUT_SEQ_FAILED,            /* out->why says what, exit_code POUT_EXIT_DECK */
} pout_seq_status_t;

typedef struct {
    pout_seq_status_t status;
    pout_act_kind_t cmd;
    pin_tc_t tc;
    int exit_code;
    char why[200];
    int action;                 /* index of the action the status is about */
} pout_seq_out_t;

typedef struct {
    const pout_script_t *sc;
    int next;
    int in_wait;
    int finished, failed;
    pin_script_track_t track;
} pout_seq_t;

void pout_seq_init(pout_seq_t *q, const pout_script_t *sc);
/* One poll. After POUT_SEQ_COMMAND the caller applies the command, then
 * observes the deck again (a command changes the reported state at once) and
 * calls step again, until WAITING, DONE or FAILED. Calling it after DONE or
 * FAILED returns the same again. */
pout_seq_status_t pout_seq_step(pout_seq_t *q, const pout_obs_t *obs, pout_seq_out_t *out);
/* The deck refused the command just returned (GOTO to a timecode that is not on the tape). */
void pout_seq_command_failed(pout_seq_t *q, const char *why, pout_seq_out_t *out);

#ifdef __cplusplus
}
#endif

#endif /* POUT_SCRIPT_H */
