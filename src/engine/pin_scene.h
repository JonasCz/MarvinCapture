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
 * dvgrab-style scene-split detector, fed one frame (DV) or one picture (HDV)
 * of metadata at a time. It never sees pixels: only timecode, recording
 * date/time and a "rec start" flag, all with per-frame validity bits (a
 * frame with no usable timecode or date must not itself look like a jump).
 * HDV has a timecode only in the picture that opens a GOP (the GOP header);
 * its other pictures are fed without one, so there "frames" in the debounce
 * below are GOPs (pin_scene_config_hdv()).
 *
 * A cut is a *confirmed* discontinuity: it only fires once debounce_frames
 * consecutive frames after the break agree with each other (not with the
 * old run). A single glitched frame that goes right back to the old run's
 * continuity is absorbed silently. That means the caller must hold back
 * (not finalise into a file) at least pin_scene_debounce_window() frames
 * before committing them -- when a cut is confirmed, it is reported at the
 * index of the FIRST anomalous frame, which is already debounce_frames-1
 * frames in the past by the time we're sure.
 *
 * Defaults: debounce_frames = 5 (long enough that a single dropout or
 * corrupt subcode copy never causes a false split, short enough not to
 * delay a real cut noticeably). date_gap_seconds = 1.0: dvgrab's own
 * heuristic is "the wall-clock recording time moved by more than roughly a
 * second between frames that are otherwise timecode-continuous" -- exactly
 * the "camera paused and resumed without resetting timecode" case many
 * consumer camcorders exhibit, where timecode alone can't see the cut.
 * tc_jump_seconds = 1.0: absorbs the occasional multi-frame repeat/skip a
 * noisy capture produces without false-splitting, while still catching a
 * genuine edit.
 *
 * HDV (pin_scene_config_hdv()): debounce_frames = 3 GOPs and sparse_tc, see
 * there.
 */

#ifndef PIN_SCENE_H
#define PIN_SCENE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    long frame_index; /* position in the capture, 0-based; monotonically increasing */

    int tc_valid;
    int tc_hour, tc_minute, tc_second, tc_frame;
    int tc_drop_frame;

    int date_valid;
    int date_day, date_month, date_year;

    int time_valid;
    int time_hour, time_minute, time_second;

    int rec_start_valid;
    int rec_start;

    /* HDV: the camera clock (the video PES's DTS, 90 kHz, 33 bits, it wraps) of the picture.
     * Like the timecode, only on the picture that opens a GOP. A jump of it (more than
     * tc_jump_seconds ahead of where the pictures fed since put it, or back) is a break of its
     * own: another recording even where the timecode runs on. */
    int ts_valid;
    int64_t ts;
} pin_scene_record_t;

typedef struct {
    unsigned debounce_frames;
    double date_gap_seconds;
    double tc_jump_seconds;
    double fps; /* nominal frame rate, e.g. 29.97, 25, 23.976 */
    /* Records carry a timecode only now and then (HDV: once per GOP), and
     * frame_index counts frames: a timecode may run ahead of the one it is
     * compared with by the frames fed since that one, plus tc_jump_seconds.
     * So the next good GOP after one or more with a damaged or missing GOP
     * header still continues the run. Backward stays a break, and a timecode
     * that stands still (a camera that writes 00:00:00:00 in every GOP) stays
     * continuous, both as without it. 0 (DV): measured from the last record. */
    int sparse_tc;
} pin_scene_config_t;

/* debounce_frames = 5, date_gap_seconds = 1.0, tc_jump_seconds = 1.0, sparse_tc = 0. */
void pin_scene_config_defaults(pin_scene_config_t *cfg, double fps);

/* HDV, fed one record per picture with the GOP header's timecode on the
 * picture that carries it: the defaults, with debounce_frames = 3 (GOPs) and
 * sparse_tc = 1. Three GOPs are about 1-1.5 s at 1080i (12 or 15 pictures),
 * 0.3 s at 720p (6): a GOP or two with a wrong timecode (a damaged GOP header,
 * a dropout) never split, while a real jump is confirmed at the third GOP of
 * the new run, about two GOPs after it began -- no later in practice than the
 * split lookahead's 1 s minimum segment (pin_split.h) allows a new file anyway. */
void pin_scene_config_hdv(pin_scene_config_t *cfg, double fps);

typedef struct {
    int tc_valid;
    int tc_hour, tc_minute, tc_second, tc_frame;
    int tc_drop_frame;
    long tc_index; /* frame_index of the record the timecode came from (sparse_tc) */
    int date_valid;
    int date_day, date_month, date_year;
    int time_valid;
    int time_hour, time_minute, time_second;
    int ts_valid;
    int64_t ts;
    long ts_index; /* frame_index of the record the clock came from */
} pin_scene_state_t;

typedef struct {
    pin_scene_config_t cfg;

    int has_baseline;
    pin_scene_state_t baseline;

    int pending_active;
    long pending_start_index;
    pin_scene_state_t pending_candidate;
    pin_scene_state_t pending_saved_baseline;
    unsigned pending_run_length;
} pin_scene_detector_t;

void pin_scene_init(pin_scene_detector_t *d, const pin_scene_config_t *cfg);

/* Feeds one frame/GOP of metadata, in increasing frame_index order. Returns
 * 1 and sets *cut_frame_index if this call confirms a scene cut (the index
 * is the first anomalous frame, already debounce_frames-1 frames behind
 * the frame just fed); returns 0 otherwise. */
int pin_scene_feed(pin_scene_detector_t *d, const pin_scene_record_t *r, long *cut_frame_index);

/* How many trailing frames the caller must keep un-finalised (e.g. in a
 * small FIFO ahead of the file writer) so a confirmed cut can still be
 * back-dated to its true first frame. Equal to cfg.debounce_frames: records,
 * so only for a timecode in every record (DV); with sparse records (HDV) hold
 * from pin_scene_pending()'s index instead. */
unsigned pin_scene_debounce_window(const pin_scene_detector_t *d);

/* 1 while a possible break is waiting to be confirmed or dropped, with
 * *start_index = the frame_index a cut would be reported at: the caller must
 * still hold that frame and everything after it. 0 when none is pending:
 * every frame fed so far can be written (a cut is never reported at an index
 * fed before the break was first seen). */
int pin_scene_pending(const pin_scene_detector_t *d, long *start_index);

#ifdef __cplusplus
}
#endif

#endif /* PIN_SCENE_H */
