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
 * HDV audio for monitoring/metering: decodes the TS's MPEG-1 Layer II
 * ("mp2") audio PID with libavcodec, resamples to 48 kHz (pin_audio_resample.h
 * -- the project's static FFmpeg has swresample disabled) and hands the
 * result to a callback (pin_session.c's pin_session_feed_monitor_audio()).
 *
 * Runs on its own thread with a small bounded queue, fed by
 * pin_hdv_audio_push() once per picture from pin_session.c's dv_on_unit()
 * (both live capture and HDV .ts replay): decoupled from the preview video
 * decoder (which is low-priority and drop-if-busy, see pin_preview.h) so a
 * busy video decode never starves audio, and decoupled from the USB read
 * loop (push() only copies into the queue, never decodes) so a slow mp2
 * decode never stalls a capture. If the queue is ever full (the decoder
 * fell behind), the oldest queued picture's audio is dropped -- monitor
 * audio only; the captured file is untouched either way.
 */

#ifndef PIN_HDV_AUDIO_H
#define PIN_HDV_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pin_hdv_audio pin_hdv_audio_t;

/* Called (from the decode thread, never the caller of pin_hdv_audio_push())
 * with interleaved 48 kHz stereo s16 PCM whenever a chunk is decoded. */
typedef void (*pin_hdv_audio_feed_cb)(void *user, const int16_t *pcm, unsigned frames);

pin_hdv_audio_t *pin_hdv_audio_create(pin_hdv_audio_feed_cb cb, void *user);
void pin_hdv_audio_destroy(pin_hdv_audio_t *a);

/* Enqueues one picture's worth of TS packets (n_packets * 188 bytes) for
 * audio extraction once audio_pid is known (<= 0: not yet identified from
 * the PMT, silently ignored). Copies the data; never blocks the caller. */
void pin_hdv_audio_push(pin_hdv_audio_t *a, const uint8_t *ts_packets, size_t n_packets,
                         int audio_pid);

#ifdef __cplusplus
}
#endif

#endif /* PIN_HDV_AUDIO_H */
