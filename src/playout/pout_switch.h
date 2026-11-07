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
 * What MarvinPlayout does when the tape changes format under the head: DV with HDV, DV PAL with
 * NTSC, HDV 1080i with 720p (a tape with mixed recordings, pout_tape.h). EVERYTHING HERE IS A
 * GUESS. Nobody has yet observed what a real camera sends over FireWire when its tape switches
 * format, so the whole behaviour sits in one small table, one entry per kind of switch, that is
 * easy to change once a camera has been seen (docs/testing-required.md 3.1 asks for exactly that):
 *
 *   gap_s          time with NO isochronous packets at all (the stream stops). The deck keeps
 *                  playing meanwhile: the tape frames under the head in that time are lost.
 *   blank_s        then this much time of empty CIP packets (header only) in the NEW format, before
 *                  the first frame. The deck keeps playing: these frames are lost too.
 *   bus_reset      initiate a bus reset at the switch (device only; the session then rescans and
 *                  restarts the stream, as for any bus reset, which ends the switch at once).
 *   restart_stream stop and restart the IT context and re-sync the cycle (device only); the
 *                  packet size and the oPCR payload / AV/C output plug format follow the new format.
 *
 * Defaults (and why):
 *   DV <-> HDV      gap 1.0 s, blank 0.2 s, no bus reset, restart. Consumer HDV camcorders with an
 *                   "i.LINK conversion: off / AUTO" setting switch the i.LINK stream with the tape
 *                   and their manuals warn that the signal is interrupted at the change. The
 *                   packet size and the CIP format (FMT 0 / 0x20) change, so the context restarts.
 *                   The AV/C OUTPUT PLUG SIGNAL FORMAT answer and the oPCR payload follow the new
 *                   format from the switch on.
 *   DV PAL <-> NTSC gap 0, blank 0.5 s, no reset, no restart: the stream goes on, only the CIP FDF
 *                   and the frame rate change; the deck re-locks its servo to the new frame rate,
 *                   which is where the empty packets come from. (Also used for HDV 50 <-> 60 Hz.)
 *   HDV 1080i <-> 720p gap 0.5 s, blank 0.2 s, no reset, no restart: same CIP format, but the
 *                   encoder/decoder chain restarts on a new sequence.
 * If several things change at once the switch counts as the most severe: KIND, then HDV_FMT, then STD.
 *
 * For experiments the table can be overridden (MarvinPlayout --switch, pout_switch_set()):
 *     KIND=gap:SEC,blank:SEC,reset:0|1,restart:0|1       KIND is std, hdv-format or kind
 * Keys not mentioned keep their current value.
 */

#ifndef POUT_SWITCH_H
#define POUT_SWITCH_H

#include "pout_tape.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    POUT_SWITCH_NONE = 0,   /* same format: nothing happens */
    POUT_SWITCH_STD,        /* DV PAL <-> NTSC (HDV 50 <-> 60 Hz too) */
    POUT_SWITCH_HDV_FMT,    /* HDV 1080i <-> 720p */
    POUT_SWITCH_KIND,       /* DV <-> HDV */
    POUT_SWITCH_COUNT
} pout_switch_kind_t;

typedef struct {
    double gap_s;        /* time with NO isochronous packets at all (the stream stops) */
    double blank_s;      /* then time with empty CIP packets of the NEW format before the first frame */
    int bus_reset;       /* initiate a bus reset at the switch (device only) */
    int restart_stream;  /* stop and restart the IT context / re-sync the cycle (device only) */
} pout_switch_policy_t;

/* What kind of switch from -> to is (NONE if the two are the same format). */
pout_switch_kind_t pout_switch_classify(const pout_frame_fmt_t *from, const pout_frame_fmt_t *to);
/* The policy of a kind: the defaults above unless overridden. NONE gives all zeros. Never NULL. */
const pout_switch_policy_t *pout_switch_policy(pout_switch_kind_t k);
/* "std", "hdv-format", "kind" (NONE: "none"). */
const char *pout_switch_name(pout_switch_kind_t k);

/* Parses "KIND=key:value,..." on top of the current table value of KIND (nothing is changed).
 * Returns 0, or -1 with a sentence in err. */
int pout_switch_parse(const char *spec, pout_switch_kind_t *kind, pout_switch_policy_t *policy, char *err,
                      size_t err_cap);
/* pout_switch_parse() and store the result in the table. */
int pout_switch_set(const char *spec, char *err, size_t err_cap);
/* Back to the defaults. */
void pout_switch_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* POUT_SWITCH_H */
