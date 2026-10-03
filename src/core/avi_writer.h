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
 * Minimal OpenDML (AVI 2.0) writer: one uncompressed YUY2 video stream and
 * one 16-bit PCM audio stream. RIFF segments are kept under 1 GiB; the
 * first one also carries a legacy idx1, so old readers see its part of the
 * file. The frame rate is given as a fraction (25/1, 30000/1001).
 */

#ifndef AVI_WRITER_H
#define AVI_WRITER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct avi_writer avi_writer_t;

/* aspect_num:aspect_den is the display aspect of the whole frame (4:3 for
 * analog SD); it goes into the OpenDML vprp header, from which players
 * derive the pixel aspect. Also takes a UTF-8 title written as a "LIST INFO"/"INAM"
 * chunk in the header list (so it appears in the first RIFF, ahead of any
 * frame data, exactly where the OpenDML spec expects file-level metadata).
 * title may be NULL or "" for no title. */
avi_writer_t *avi_open_titled(const char *path, unsigned width, unsigned height,
                              unsigned fps_num, unsigned fps_den, unsigned aspect_num,
                              unsigned aspect_den, unsigned audio_rate, unsigned audio_channels,
                              const char *title_utf8);
int avi_write_video(avi_writer_t *w, const uint8_t *frame, size_t len);
int avi_write_audio(avi_writer_t *w, const uint8_t *pcm, size_t len);
/* Finalises the headers and indexes. Returns 0 on success. */
int avi_close(avi_writer_t *w);

#ifdef __cplusplus
}
#endif

#endif /* AVI_WRITER_H */
