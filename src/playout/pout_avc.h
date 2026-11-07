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
 * MarvinPlayout's AV/C target: the other side of src/engine/pin_deck.c. A pure
 * function turns one FCP command frame into the response frame and applies
 * control commands to the virtual deck (pout_deck.h). The 1394 layer
 * (pout_csr / pout_hw_*) only carries the frames.
 *
 * What it answers is taken from docs/deck-control.md (what the Canon HDV and
 * the DV camcorder answered), docs/startup.md (the inquiries Windows sent) and
 * docs/hdv.md (HDV's CIP format bytes). Nothing has been tried against a real
 * AV/C controller: every byte that has no recorded camera answer behind it is
 * marked "guess" in pout_avc.c.
 */

#ifndef POUT_AVC_H
#define POUT_AVC_H

#include "pout_deck.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    POUT_AVC_DV_625 = 0,    /* DV SD, 625/50 */
    POUT_AVC_DV_525,        /* DV SD, 525/60 */
    POUT_AVC_HDV,           /* HDV, MPEG-2 TS (IEC 61883-4) */
} pout_avc_media_t;

/* AV/C response codes (ctype field of a response) */
enum {
    POUT_AVC_NOT_IMPLEMENTED = 0x08,
    POUT_AVC_ACCEPTED = 0x09,
    POUT_AVC_REJECTED = 0x0a,
    POUT_AVC_IN_TRANSITION = 0x0b,
    POUT_AVC_IMPLEMENTED = 0x0c,    /* also STABLE */
    POUT_AVC_CHANGED = 0x0d,
    POUT_AVC_INTERIM = 0x0f,
};

typedef struct {
    pout_avc_media_t media;
    /* UNIT INFO company ID. The default is 0x000000 (no registered ID); a real
     * deck answers its maker's OUI (the Canon: 0x000085). Some controllers
     * switch quirks on it, so it may have to be set. */
    uint32_t company_id;
} pout_avc_t;

void pout_avc_init(pout_avc_t *t, pout_avc_media_t media);

/* Handles one command frame (ctype 0 CONTROL, 1 STATUS, 2 SPECIFIC INQUIRY,
 * 3 NOTIFY, 4 GENERAL INQUIRY; subunit 0xFF = unit, 0x20 = tape recorder #0)
 * at time `now`. Writes the response frame into resp (the command with the
 * response code in byte 0 and, for status queries, the answer filled in) and
 * returns its length. Returns 0 when there is nothing to send (not a command
 * frame, or resp is too small). A control command is applied to the deck. */
int pout_avc_handle(pout_avc_t *t, pout_deck_t *d, double now, const uint8_t *cmd, unsigned len,
                    uint8_t *resp, unsigned cap);

#ifdef __cplusplus
}
#endif

#endif /* POUT_AVC_H */
