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
 * Per-frame error classification for a reassembled DV frame (the same idea as
 * dvrescue's per-frame STA / audio-error counters). Hardware-free, no state.
 *
 * What counts, per 80-byte DIF block (layout: SMPTE 306M / IEC 61834 -- block 0
 * header, 1-2 subcode, 3-5 VAUX, then groups of 16 = 1 audio + 15 video):
 *
 *  - missing block: the ID bytes (SCT, DSEQ, DBN, the "1" bits) do not match the
 *    block's position. This is what a zero-padded sequence (the reassembler's
 *    stand-in for a lost one) looks like, and also garbage from the bus.
 *  - video STA nibble (byte 3 high nibble) != 0. 7 and 15 are errors that were
 *    not concealed ("video_err"); 2/4/6 and 10/12/14 are concealments by the
 *    source ("video_concealed"). Both make the frame "with error".
 *  - audio block whose 36 sample words are all one non-zero value: the source's
 *    error fill (0x8000 per IEC 61834, vendor values like 0xFFB1 exist).
 *  - audio mute (Sony DSR-1500P style, from dvmerge's quirks): the deck writes
 *    literal 0x0000 when its audio correction fails. Digital zero is also real
 *    silence, and the unused second channel pair of 32 kHz 4-channel recordings
 *    is mostly zero, so only a PARTIAL zero pattern in the first channel pair
 *    (the first half of the sequences) counts: genuine silence zeroes all of it.
 *  - re-encoded frames (Samsung VP-D340 quirk): ~every video block stamped
 *    STA 14. Counted as concealed (honest reading; the picture data may be
 *    fine, but the camera was in trouble), and flagged separately.
 *
 * A frame is "dropped" when (almost) nothing in it is real: >= 90 % of its
 * blocks are missing.
 */

#ifndef DV_ERROR_H
#define DV_ERROR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned blocks;            /* DIF blocks examined */
    unsigned missing_blocks;
    unsigned video_blocks;      /* video blocks present (not missing) */
    unsigned video_err;         /* STA 7 / 15 */
    unsigned video_concealed;   /* other non-zero STA */
    unsigned audio_blocks;      /* audio blocks present */
    unsigned audio_err;         /* constant non-zero error fill */
    unsigned audio_mute;        /* partial all-zero pattern, see above */
    int reencoded;              /* ~all video blocks STA 14 */
    int frame_error;            /* any of the above */
    int frame_dropped;          /* >= 90 % of the blocks missing */
} dv_frame_errors_t;

/* len must be 10 or 12 whole DIF sequences. Returns 0, or -1 on a bad length
 * (out is zeroed). */
int dv_error_analyze(const uint8_t *frame, size_t len, dv_frame_errors_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DV_ERROR_H */
