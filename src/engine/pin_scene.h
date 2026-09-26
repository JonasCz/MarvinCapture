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
 * dvgrab-style scene-split detector, fed one frame (DV) or one GOP (HDV) of
 * metadata at a time. It never sees pixels: only timecode, recording
 * date/time and a "rec start" flag, all with per-frame validity bits (a
 * frame with no usable timecode or date must not itself look like a jump).
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
} pin_scene_record_t;

typedef struct {
    unsigned debounce_frames;
    double date_gap_seconds;
    double tc_jump_seconds;
    double fps; /* nominal frame rate, e.g. 29.97, 25, 23.976 */
} pin_scene_config_t;

/* debounce_frames = 5, date_gap_seconds = 1.0, tc_jump_seconds = 1.0. */
void pin_scene_config_defaults(pin_scene_config_t *cfg, double fps);

typedef struct {
    int tc_valid;
    int tc_hour, tc_minute, tc_second, tc_frame;
    int tc_drop_frame;
    int date_valid;
    int date_day, date_month, date_year;
    int time_valid;
    int time_hour, time_minute, time_second;
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
 * back-dated to its true first frame. Equal to cfg.debounce_frames. */
unsigned pin_scene_debounce_window(const pin_scene_detector_t *d);

#ifdef __cplusplus
}
#endif

#endif /* PIN_SCENE_H */
