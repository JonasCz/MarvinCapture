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
 * Minimal, hardware-free MPEG2-TS (HDV) per-picture metadata parser.
 *
 * Two things are solidly specified and implemented with confidence:
 *
 *   - The MPEG-2 GOP header time_code (ISO/IEC 13818-2 section 6.2.2.6,
 *     the same bit layout ffmpeg's libavcodec/mpeg12dec.c decode_gop_header
 *     uses): 4 bytes right after the 00 00 01 B8 start code, packed as
 *     drop_frame_flag(1) hours(5) minutes(6) marker_bit(1) seconds(6)
 *     pictures(6) closed_gop(1) broken_link(1), MSB first, with 5 stuffing
 *     bits ('11111') padding out to 32 bits.
 *   - TS adaptation-field discontinuity_indicator (ISO/IEC 13818-1 2.4.3.4).
 *
 * The third thing, HDV recording date/time, is not solidly specified here:
 * HDV (Sony/Canon, informed by IEC 61834-11) carries it in a private TS
 * stream that community tools (MediaInfoLib's HDV support, dvrescue) know
 * how to read, conventionally on PID 0x811. This session's web research
 * turned up only secondhand, unverifiable summaries of that layout (a
 * forum post whose actual page could not be fetched to confirm byte
 * offsets), not the tool source or spec text itself, so no fixed-offset
 * layout is hardcoded here. Instead, per the HDV spec's use of DV-style
 * packs (the same PC0 + 4-byte-data packs dv_subcode.c reads: 0x62 = date,
 * 0x63 = time, 0x13 = timecode), hdv_aux_scan_datetime() scans the aux
 * PID's payload bytes for those pack signatures using the same BCD
 * decoding as dv_subcode.c. This is explicitly best-effort: verify PID and
 * offsets against a real HDV capture on the rig before relying on it.
 */

#ifndef HDV_AUX_H
#define HDV_AUX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HDV_TS_PACKET_SIZE 188
#define HDV_TS_SYNC_BYTE 0x47

/* Conventional PID for the HDV AUX-V/AUX-A private stream when the PMT
 * doesn't identify one explicitly (see file header comment). */
#define HDV_AUX_PID_DEFAULT 0x0811

typedef struct {
    int drop_frame;
    int hours, minutes, seconds, pictures;
    int closed_gop;
    int broken_link;
    int found; /* a GOP header was located and decoded */
} hdv_gop_time_t;

typedef struct {
    int day, month, year;
    int valid;
} hdv_aux_date_t;

typedef struct {
    int hour, minute, second;
    int valid;
} hdv_aux_time_t;

typedef struct {
    int hour, minute, second, frame;
    int drop_frame;
    int valid;
} hdv_aux_timecode_t;

typedef struct {
    hdv_aux_date_t rec_date;
    hdv_aux_time_t rec_time;
    hdv_aux_timecode_t timecode;
} hdv_aux_info_t; /* every field best-effort, see file header comment */

typedef struct {
    int pat_found;
    int pmt_pid;      /* -1 if not found */
    int pmt_found;
    int video_pid;     /* -1 if not found; stream_type 0x02 (MPEG2 video) */
    int aux_pid;        /* -1 if not identified from the PMT; caller can fall
                          * back to HDV_AUX_PID_DEFAULT */
    int audio_pid;      /* -1 if not found; stream_type 0x03 (MPEG-1 Layer II)
                          * or 0x04 (MPEG-2 audio) -- HDV carries MPEG-1 Layer
                          * II ("mp2"), decodable by the same fixed-point/
                          * float mp2 decoder either way. */
} hdv_pid_map_t;

/* Scans PAT (PID 0) and, once found, the PMT, across as many TS packets as
 * given. Safe to call repeatedly with more packets as they arrive; fields
 * already found are left alone (pass a *map already zeroed with pid -1 the
 * first time). Returns the number of packets consumed usefully (for stats
 * only); parsing is best-effort and never fails hard on garbage input. */
void hdv_scan_pat_pmt(const uint8_t *ts_packets, size_t n_packets, hdv_pid_map_t *map);

/* Extracts the elementary-stream bytes for the given PID from a run of TS
 * packets ("one picture" worth, or any short run) and looks for an MPEG-2
 * GOP header (00 00 01 B8) in them. *discontinuity_seen is OR'd with 1 if
 * any adaptation_field discontinuity_indicator is set on a packet of that
 * PID (the caller can pass a fresh int or an accumulator). Returns 0 on
 * success (regardless of whether a GOP header was found -- check
 * gop_out->found), -1 on invalid arguments. */
int hdv_parse_picture(const uint8_t *ts_packets, size_t n_packets, int video_pid,
                       hdv_gop_time_t *gop_out, int *discontinuity_seen);

/* Counts TS packets (any PID) with adaptation_field discontinuity_indicator
 * set. */
unsigned hdv_ts_discontinuity_count(const uint8_t *ts_packets, size_t n_packets);

/* A GOP time_code that is not 00:00:00:00. Cameras that do not record a timecode write
 * zeros in every GOP header (the Canon HDV does), which says nothing about the tape. */
int hdv_gop_time_nonzero(const hdv_gop_time_t *g);

/* Offset of the first MPEG-2 sequence header (00 00 01 B3) in an elementary stream
 * chunk, or (size_t)-1 if there is none. A decoder fed from before it only complains. */
size_t hdv_find_sequence_header(const uint8_t *es, size_t len);

/* Best-effort scan of aux_pid's payload bytes for DV-style date/time/TC
 * packs, see file header comment. out is zeroed then filled with whatever
 * was found; fields whose pack was never seen stay .valid = 0. */
void hdv_aux_scan_datetime(const uint8_t *ts_packets, size_t n_packets, int aux_pid,
                            hdv_aux_info_t *out);

#ifdef __cplusplus
}
#endif

#endif /* HDV_AUX_H */
