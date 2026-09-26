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
 * Preview decode: turns whatever is arriving (analog YUYV frames, DV
 * frames, HDV pictures) into planar YUV in a triple buffer a render thread
 * can lock/unlock without ever blocking the capture path. Runs on its own
 * low-priority thread; drops a frame rather than fall behind.
 *
 *   - Analog: YUYV 4:2:2 -> planar Y/Cb/Cr, a straight byte shuffle, no
 *     decode needed.
 *   - DV: libavcodec's dvvideo decoder, one whole reassembled frame in, one
 *     AVFrame out (yuv420p for PAL, yuv411p for NTSC -- dvvideo's own
 *     output format per docs/analog.md and FFmpeg's dv.c).
 *   - HDV: libavcodec's mpeg2video decoder, fed the video PID's elementary
 *     stream bytes extracted from each picture's TS packets. A GOP-aligned
 *     decode (first request after start/seek needs an I-frame) -- until
 *     one arrives, pin_previewer_wait() simply times out and the caller sees
 *     no frame, exactly like "no signal".
 *
 * pin_preview_t is owned by pin_session_t (one per session); the functions
 * here are what src/api/pin_api.c's pin_preview_* wrappers call straight
 * through to, plus pin_preview_push_* used internally by pin_session.c's
 * capture/streaming paths to hand it new raw data.
 */

#ifndef PIN_PREVIEW_H
#define PIN_PREVIEW_H

#include "../api/pin_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pin_preview pin_preview_t;

pin_preview_t *pin_previewer_create(void);
void pin_previewer_destroy(pin_preview_t *p);

/* Called from the capture/streaming path with new raw data. Never blocks:
 * queues at most one pending unit (drop-if-busy -- a unit arriving while
 * the decode thread is still busy on the previous one replaces it, so the
 * preview is always decoding the newest data, never backing up). */
void pin_previewer_push_analog(pin_preview_t *p, const uint8_t *yuyv, unsigned width,
                              unsigned height, int is_pal);
void pin_previewer_push_dv(pin_preview_t *p, const uint8_t *frame, size_t len, int is_pal);
void pin_previewer_push_hdv(pin_preview_t *p, const uint8_t *ts_packets, size_t n_packets,
                           int video_pid);

void pin_previewer_set_aspect_override(pin_preview_t *p, pin_aspect_t aspect);

int pin_previewer_wait(pin_preview_t *p, uint64_t after_seq, int timeout_ms);
pin_status_t pin_previewer_lock(pin_preview_t *p, pin_frame_t *out);
void pin_previewer_unlock(pin_preview_t *p);
void pin_previewer_enable(pin_preview_t *p, int enabled);

#ifdef __cplusplus
}
#endif

#endif /* PIN_PREVIEW_H */
