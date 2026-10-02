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
 * Per-picture error classification for HDV (MPEG-2 transport stream). One
 * "unit" is what the reassembler emits: the TS packets of one video picture
 * (a PES start to the next), with the other PIDs interleaved. Stateful across
 * units (continuity counters, reference-picture damage).
 *
 * Direct damage of a unit:
 *  - transport_error_indicator set on any packet,
 *  - lost sync byte,
 *  - continuity_counter gap on any PID that carries payload (a duplicate packet
 *    is legal and ignored; adaptation-field discontinuity_indicator is NOT an
 *    error, it marks a recording boundary), a gap on the very first packet of a
 *    unit counts against that unit (the loss is between the two pictures),
 *  - the video PES does not start with 00 00 01 E0..EF, no picture header in the
 *    unit, or a non-zero PES_packet_length that the payload does not match.
 * Only video-PID damage propagates through the GOP; an audio-only gap marks the
 * unit "with error" but leaves the references alone.
 *
 * Propagation (coded order): an I picture is self-contained, a P depends on the
 * previous reference (I or P), a B on both neighbouring references (the last
 * two seen). So a damaged I/P taints later P pictures until the next I and the
 * B pictures that use it. A closed GOP header stops a B after the new I from
 * depending on the previous GOP's last reference.
 */

#ifndef HDV_ERROR_H
#define HDV_ERROR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t cc[8192];       /* last continuity counter per PID, -1 = none yet */
    int ref_dmg_last;       /* the latest reference picture (I/P) is damaged/tainted */
    int ref_dmg_prev;       /* the one before it */
    int init;
} hdv_err_state_t;

typedef struct {
    unsigned tei;           /* packets with transport_error_indicator */
    unsigned sync_lost;     /* packets without the 0x47 sync byte */
    unsigned cc_errors;     /* continuity gaps (any PID) */
    unsigned pes_errors;    /* bad PES start / no picture header / wrong length */
    int pic_type;           /* 1 I, 2 P, 3 B, 0 unknown */
    int closed_gop;         /* a GOP header with closed_gop was in the unit */
    int video_damaged;      /* the unit's own video data is damaged */
    int tainted;            /* damaged only through a reference it depends on */
    int frame_error;        /* video_damaged || tainted || any other error */
    int seq_found;          /* an MPEG-2 sequence header was in the unit: */
    int seq_width, seq_height;  /* its horizontal_size / vertical_size */
    int seq_frame_rate_code;    /* its frame_rate_code (1..8, see pin_vidfmt.h) */
} hdv_unit_errors_t;

void hdv_error_init(hdv_err_state_t *st);

/* Call once per unit, in stream order. video_pid < 0 = not known yet (video
 * checks are skipped). */
void hdv_error_analyze(hdv_err_state_t *st, const uint8_t *ts, size_t n_packets,
                       int video_pid, hdv_unit_errors_t *out);

#ifdef __cplusplus
}
#endif

#endif /* HDV_ERROR_H */
