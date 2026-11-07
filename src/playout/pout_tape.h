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
 * The "virtual tape" of MarvinPlayout: one or more .dv or .m2t/.ts files
 * (each one a recording) joined in the given order and indexed, so the virtual
 * deck can play any frame and search by timecode. Files are never loaded:
 * DV frame N is file-offset arithmetic, HDV gets a light index (PCR samples
 * and one entry per GOP) from one sequential scan at open.
 *
 * What is a guess (no real tape or device has been tried):
 *   - HDV timecode comes from the MPEG-2 GOP header time_code of the first
 *     picture of each GOP (what cameras record there), not from the aux
 *     stream, whose layout hdv_aux.h could not confirm. A file whose GOP
 *     timecodes are all 00:00:00:00 (some cameras write zeros) has no
 *     timecode and gets a made-up one.
 *   - An HDV "frame" is a slot of 1/fps seconds on the PCR timeline, fps being
 *     25 (sequence header frame rates 24/25/50) or 30000/1001 (the rest);
 *     720p50/60 therefore gives two pictures per slot. The timecode of a slot
 *     is the GOP's timecode plus the slots since the GOP's first packet; the
 *     GOP's packets arrive a little before its first picture is shown, so this
 *     can be a frame or two early against a real deck. It is consistent
 *     (frame_of_tc inverts tc_of_frame), which is what a search needs.
 *
 * Joining files: each recording keeps its own timecode; a file without one
 * continues the previous file's (00:00:00:00 at the start of the tape). HDV
 * files each restart their PCR clock, so the stream has a PCR discontinuity
 * where two files meet, like a real tape between recordings.
 *
 * Mixed tapes: the files may be of different formats (DV with HDV, PAL with
 * NTSC, 1080i with 720p), like a real tape with mixed recordings. Every file
 * is indexed and read in its own format; pout_tape_frame_fmt() tells which
 * format a frame is in, and what the player does at the change is the
 * session's business (pout_switch.h). A tape frame is always one output
 * period (1/25 or 1/29.97 s of the file it is from), the deck counts them.
 * Timecode: each file keeps its own base (25 or 30) and drop-frame flag; a file
 * without timecode continues from the label after the previous file's last
 * frame, its frame number rescaled to the new base (PAL 00:10:00:12 goes on as
 * NTSC 00:10:00:14), a dropped label moved to :02. A timecode search reads the
 * label in each file's own base, so one label can exist in a PAL and an NTSC
 * file; the earliest in tape order wins.
 *
 * Design note, analog playout later: this module is a *bitstream* source
 * (read_frame gives DV frames or TS packets). An analog output needs decoded
 * pictures, so a MOV/MKV source (demuxed and decoded by FFmpeg) would sit beside
 * it as a second tape type with the same shape: frames(), fps(), tc_of_frame /
 * frame_of_tc (pout_tape_ops_t, which is all the deck sees), and a read call
 * returning a decoded video frame plus its audio samples instead of bytes. The
 * deck would not change; only the output (analog encoder program instead of
 * the OHCI IT packetizer) picks which read call to use.
 */

#ifndef POUT_TAPE_H
#define POUT_TAPE_H

#include <stddef.h>
#include <stdint.h>

#include "pin_script.h" /* pin_tc_t */

#ifdef __cplusplus
extern "C" {
#endif

/* The deck's view of the tape: the only definition of this type (pout_deck.h includes this
 * header for it). */
typedef struct {
    void *ctx;
    int64_t frames;
    double fps;
    int (*tc_of_frame)(void *ctx, int64_t frame, pin_tc_t *tc);
    int64_t (*frame_of_tc)(void *ctx, const pin_tc_t *tc);
} pout_tape_ops_t;

typedef enum { POUT_MEDIA_DV = 0, POUT_MEDIA_HDV } pout_media_t;
typedef struct pout_tape pout_tape_t;

/* Largest isochronous packet of a format (CIP header included): DV 8 + 480, HDV 8 + 3 * 192. */
#define POUT_DV_MAX_PACKET 488u
#define POUT_HDV_MAX_PACKET 584u

/* The format of a stretch of tape. Two frames are in the same format when media, is_625 and
 * hdv_720p are equal (fps and max_packet follow from them). */
typedef struct {
    pout_media_t media;
    int is_625;         /* PAL / 50 Hz family */
    double fps;         /* 25 or 30000/1001 (HDV 720p counted in 25 / 29.97 slots, as pout_tape_fps) */
    int hdv_720p;       /* HDV only: 720p, else 1080i (any other size) */
    unsigned max_packet;/* POUT_DV_MAX_PACKET or POUT_HDV_MAX_PACKET */
} pout_frame_fmt_t;

/* Opens and indexes the files, which may be of different formats (DV 625 or 525, HDV of either
 * picture size and rate). Returns 0, or -1 with a message naming the file in err (a file that is
 * empty, unreadable or neither DV nor HDV). */
int pout_tape_open(pout_tape_t **out, const char *const *paths, int n, char *err, size_t err_cap);
void pout_tape_close(pout_tape_t *t);

/* media, is_625 and fps are those of the FIRST file; they are all there is to say about a tape of
 * one format. For a mixed tape use pout_tape_frame_fmt(). The deck's wind speed uses the first file's
 * fps (approximate for the rest). */
pout_media_t pout_tape_media(const pout_tape_t *t);
int pout_tape_is_625(const pout_tape_t *t); /* PAL / 50 Hz family */
/* 25 or 30000/1001 (HDV 720p50/59.94 are counted in 25 / 29.97 slots). */
double pout_tape_fps(const pout_tape_t *t);
/* The format of a frame; 0, or -1 outside the tape. */
int pout_tape_frame_fmt(const pout_tape_t *t, int64_t frame, pout_frame_fmt_t *out);
/* 1 if the files are not all of one format (pout_tape_frame_fmt differs somewhere). */
int pout_tape_mixed(const pout_tape_t *t);
int64_t pout_tape_frames(const pout_tape_t *t);

/* Timecode of a frame; sep is ';' (drop frame) or ':' . Returns 0, or -1 if
 * frame is outside the tape. DV reads the frame's own subcode when the file
 * has one. */
int pout_tape_tc_of_frame(const pout_tape_t *t, int64_t frame, pin_tc_t *tc);
/* The first frame, in tape order, whose timecode is tc; else the first frame
 * of the earliest file whose timecode range contains tc; else -1. Heuristic
 * for speed: each file keeps a few linear runs of timecode (found by
 * bisecting between sampled frames), so a search is a lookup, not a scan; for
 * DV the found frame's real timecode is then verified, searching +-30
 * frames around it if it differs. */
int64_t pout_tape_frame_of_tc(const pout_tape_t *t, const pin_tc_t *tc);

/* DV: the DIF frame (120000 or 144000 bytes). HDV: the whole TS packets
 * (n * 188 bytes, M2TS prefixes dropped) whose position falls in this
 * frame's 1/fps slot; the slots of all frames together hold every packet of
 * the files once, in order. May be empty. Returns 0 on success. buf must hold
 * pout_tape_max_frame_bytes() (the largest frame of any file). */
int pout_tape_read_frame(pout_tape_t *t, int64_t frame, uint8_t *buf, size_t cap, size_t *len);
size_t pout_tape_max_frame_bytes(const pout_tape_t *t);
/* Where to start playing to show frame: HDV the frame holding the start of the
 * GOP at or before it (the first frame of its file if none); DV frame itself. */
int64_t pout_tape_play_start(const pout_tape_t *t, int64_t frame);

/* Fills the deck's view. */
void pout_tape_ops(pout_tape_t *t, pout_tape_ops_t *ops);

/* Status display. */
int pout_tape_file_count(const pout_tape_t *t);
int pout_tape_file_of_frame(const pout_tape_t *t, int64_t frame, int *index); /* 0, or -1 */
const char *pout_tape_file_path(const pout_tape_t *t, int i);                 /* NULL if out of range */
/* One line, e.g. "DV PAL, 3 files, 00:02:00:00" or "mixed DV PAL + HDV 1080i50, 3 files, 00:02:00:00"
 * (the last item is the length, counted in tape frames at the first file's base). */
void pout_tape_describe(const pout_tape_t *t, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* POUT_TAPE_H */
