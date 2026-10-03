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
 * File-output layer. A pin_sink_t is the thing that turns whole units
 * (a DV frame, an HDV picture's TS packets, one analog video frame, one
 * analog audio block) into bytes on disk in some container/codec. It never
 * touches USB and is always driven from the pin_writer consumer thread (see
 * pin_writer.h), so a slow disk or a slow encoder never blocks capture --
 * pin_writer already dropped-and-counted before a sink ever sees a unit.
 *
 * One sink instance handles one output file (or, for sink_avi/sink_rewrap
 * over the size limit their underlying writer imposes, one logical
 * recording spanning several segment files -- transparent to the caller).
 */

#ifndef PIN_SINK_H
#define PIN_SINK_H

#include "../api/pin_api.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    pin_kind_t kind;             /* PIN_KIND_ANALOG / _DV / _HDV */
    int width, height;
    int fps_num, fps_den;        /* PAL 25/1, NTSC 30000/1001 */
    int interlaced;              /* analog/HDV: 1; DV: field order still applies (BB) */
    int top_field_first;         /* analog/HDV 1080i: 1 (TFF); DV: 0 (BFF) */
    pin_aspect_t aspect;         /* display aspect; PIN_ASPECT_AUTO resolved by the caller
                                    before this point -- a sink never guesses */
    int sample_aspect_num, sample_aspect_den;  /* derived from aspect + geometry, see sink_ffv1.c */
    pin_matrix_t colour_matrix;  /* BT.601 SD / BT.709 HDV */
    int audio_rate;              /* 48000, 44100 or 32000 */
    int audio_channels;          /* 2, or 4 for the 32 kHz 12-bit DV mode (two stereo pairs) */
    char title[PIN_TEXT_MAX];    /* UTF-8, "" for none */
} pin_sink_params_t;

typedef struct {
    uint64_t bytes_written;
    uint64_t units_written;
    uint64_t units_damaged;      /* e.g. a DV/HDV unit that didn't pass the sequence-count check */
    uint64_t audio_padded_samples; /* silence inserted to keep audio in sync with video */
    int encoder_behind;          /* sink_ffv1: the encode thread's queue is backing up */
    int pipe_closed;             /* stdout sinks: the reading program is gone (not a disk error) */
    pin_status_t last_error;
} pin_sink_status_t;

typedef struct pin_sink pin_sink_t;

struct pin_sink {
    /* Opens `path` (extension may be added/changed by the sink to match its
     * container). Returns PIN_OK, or an error that pin_strerror() explains. */
    pin_status_t (*open)(pin_sink_t *s, const char *path, const pin_sink_params_t *params);

    /* DV: one whole reassembled frame (dv_reassembler's `write` callback).
     * HDV: one whole picture's TS packets. Sinks that don't accept a kind
     * (e.g. sink_avi is analog-only) return PIN_ERR_ARG. See dv_reassembler.h:
     * a unit may exceptionally be longer than 10/12 DIF sequences after a
     * bogus resync; every sink here rejects anything that is not exactly
     * DV_SEQ_COUNT_NTSC (10) or DV_SEQ_COUNT_PAL (12) whole sequences and
     * counts it as damaged instead of feeding a malformed frame to a muxer
     * or codec that assumes a fixed frame size. */
    pin_status_t (*write_unit)(pin_sink_t *s, const uint8_t *data, size_t len);

    /* Analog only. */
    pin_status_t (*write_video)(pin_sink_t *s, const uint8_t *yuyv, size_t len);
    pin_status_t (*write_audio)(pin_sink_t *s, const int16_t *pcm, size_t frames);

    /* Finalises (indexes, remux-at-close fallback if the sink used one,
     * flushes an encoder thread) and frees *s. Always callable, even after
     * an earlier write failed. */
    pin_status_t (*close)(pin_sink_t *s);

    void (*get_status)(pin_sink_t *s, pin_sink_status_t *out);

    int supports_title;

    void *priv;
};

/* Allocates and returns the right pin_sink_t for `format`, not yet open()ed.
 * NULL if the format is unknown / not implemented. Free with sink->close()
 * (which frees *sink too, matching how open() failures are handled --
 * see each sink_*.c). */
pin_sink_t *pin_sink_create(pin_format_t format);

/* Wraps a file sink (from pin_sink_create(), not yet open()ed) so its file is created by the
 * first video unit instead of by open(): a capture that gets no video leaves no file and does
 * not touch one that is already there. Takes over `inner` (close() closes it); NULL if inner is NULL.
 * get_status().units_written counts the video units the real sink accepted. See sink_lazy.c. */
pin_sink_t *pin_sink_lazy(pin_sink_t *inner);

/* The sink for `--capture -` (open() it with the path "-"): DV raw DIF, HDV
 * MPEG-TS, analog NUT (rawvideo YUY2 + pcm_s16le). NULL for an unknown kind. */
pin_sink_t *pin_sink_create_stdout(pin_kind_t kind);

#ifdef __cplusplus
}
#endif

#endif /* PIN_SINK_H */
