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
    int fps_num, fps_den;        /* PAL 25/1, NTSC 30000/1001; HDV: the stream's */
    int interlaced;              /* analog: 1; DV: field order still applies (BB);
                                    HDV: from the stream, 0 for 720p / 1080p */
    int top_field_first;         /* analog, HDV 1080i: 1 (TFF); DV: 0 (BFF) */
    pin_aspect_t aspect;         /* display aspect; PIN_ASPECT_AUTO resolved by the caller
                                    before this point -- a sink never guesses */
    int sample_aspect_num, sample_aspect_den;  /* derived from aspect + geometry, see sink_ffv1.c */
    pin_matrix_t colour_matrix;  /* BT.601 SD / BT.709 HDV */
    int audio_rate;              /* 48000, 44100 or 32000 */
    int audio_channels;          /* 2, or 4 for the 32 kHz 12-bit DV mode (two stereo pairs) */
    char title[PIN_TEXT_MAX];    /* UTF-8, "" for none */
    /* The format settings of pin_capture_opts_t (pin_format_options()); a sink ignores the
     * ones that are not its own. A zeroed struct is FFV1 "fast" and keep-fields, but quality
     * 0 = the best (largest) H.264 / HEVC: set video_quality. */
    int deinterlace;             /* sink_hwenc: a pin_deinterlace_t */
    int video_quality;           /* sink_hwenc: 0..51, lower = better (pin_hwenc.h) */
    int ffv1_compression;        /* sink_ffv1: a pin_ffv1_compression_t */
} pin_sink_params_t;

typedef struct {
    uint64_t bytes_written;
    uint64_t units_written;
    uint64_t units_damaged;      /* e.g. a DV/HDV unit that didn't pass the sequence-count check */
    uint64_t audio_padded_samples; /* silence inserted to keep audio in sync with video */
    int encoder_behind;          /* sink_ffv1, sink_hwenc: the encode thread's queue is backing up */
    int pipe_closed;             /* stdout sinks: the reading program is gone (not a disk error) */
    int reencode_behind;         /* sink_hdvmux: repairing damaged GOPs cannot keep up; the sink
                                    failed with PIN_ERR_CODEC and the capture ends */
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

/* Numbered output (pin_capture_opts_t.first_number >= 1): what a lazy sink does when its file
 * cannot be created under its name because the name is taken (Android shared storage: a file
 * this app cannot see; see sink_lazy.c). next(ctx, k, out, cap) writes the name k numbers further
 * on (k = 1, 2, ...) and returns 0, or -1 if there is none; at most PIN_SINK_LAZY_MAX_SKIP are
 * tried. The sink owns ctx from here on and calls ctx_free(ctx) when it is closed (at once if s
 * is not a lazy sink). Call before open(). */
#define PIN_SINK_LAZY_MAX_SKIP 1000
typedef int (*pin_sink_name_fn)(void *ctx, unsigned k, char *out, size_t cap);
void pin_sink_lazy_set_renumber(pin_sink_t *s, pin_sink_name_fn next, void *ctx,
                                void (*ctx_free)(void *ctx));

/* How many numbers a lazy sink skipped for its file (0: it kept its name, or it is no lazy
 * sink, or the file does not exist yet) and, if that is not 0, the name it used in out. Safe from
 * any thread while the writer feeds the sink. */
unsigned pin_sink_lazy_renumbered(pin_sink_t *s, char *out, size_t cap);

/* Where a sink would write a temporary file: NULL or "" = next to the output file (the default),
 * else this directory. Process-wide (pin_set_app_dirs()), for sinks opened afterwards. -1 if dir
 * is too long. No sink writes one at present (HDV to MOV / MKV is written live). */
int pin_sink_set_temp_dir(const char *dir);

/* The sink for `--capture -` (open() it with the path "-"): DV raw DIF, HDV
 * MPEG-TS, analog NUT (rawvideo YUY2 + pcm_s16le). NULL for an unknown kind. */
pin_sink_t *pin_sink_create_stdout(pin_kind_t kind);

/* Routes FFmpeg's own log messages into the core log (pin_log.h) instead of stderr: once per
 * process, from whoever uses FFmpeg first (sink_avlog.c). With pin_sink_av_log_quiet(1) the
 * calling thread's messages go to the debug level (an encoder probe whose attempts may fail);
 * pin_sink_av_log_quiet(0) ends that. */
void pin_sink_av_log_install(void);
void pin_sink_av_log_quiet(int quiet);

#ifdef __cplusplus
}
#endif

#endif /* PIN_SINK_H */
