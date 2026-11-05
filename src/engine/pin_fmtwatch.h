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
 * The DV / HDV stream's format, as the session follows it on a tape that mixes
 * formats, and what a change does to the open output file. Pure (no session).
 *
 * The watcher is fed what each unit says (a DV frame: standard, aspect, audio;
 * an HDV picture: its kind, and the video format if it carries a sequence
 * header). A field a unit does not carry (no VAUX / AAUX pack, no sequence
 * header) is unknown, and an unknown field agrees with anything. A new format
 * is confirmed only when it persists, like the scene split's debounce: DV
 * PIN_FMTWATCH_DV_FRAMES frames in a row, HDV PIN_FMTWATCH_HDV_GOPS GOPs (units
 * of the new format with no unit of another format between them; the pictures
 * without a sequence header in between agree with it but do not count). A
 * single frame or GOP with a corrupt or different value never confirms a
 * change. A confirmed change is reported at the index of the first unit of
 * the run, so the caller can back-date it (pin_holdback.h: hold from
 * pin_fmtwatch_pending()'s index while one is pending). A candidate that goes
 * PIN_FMTWATCH_*_STALE units without a unit that counts is dropped.
 */

#ifndef PIN_FMTWATCH_H
#define PIN_FMTWATCH_H

#include "../api/pin_api.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* DV: frames in a row (0.2 s PAL, 0.17 s NTSC), as the DV scene split's debounce: a
 * dropout or a corrupt pack rarely spans more than a frame or two. */
#define PIN_FMTWATCH_DV_FRAMES 5
/* HDV: GOPs in a row (sequence headers; about 1-1.5 s at 1080i, 0.3 s at 720p), as the
 * HDV scene split's debounce. */
#define PIN_FMTWATCH_HDV_GOPS 3
/* A candidate that goes this many units without one that counts for it is dropped: a
 * single odd value followed only by units that do not say (no pack) must not keep the
 * caller holding units back. DV 1 s; HDV 60 pictures, four GOPs of the longest (15). */
#define PIN_FMTWATCH_DV_STALE 25
#define PIN_FMTWATCH_HDV_STALE 60

typedef struct {
    pin_kind_t kind;      /* PIN_KIND_DV / PIN_KIND_HDV; 0 = nothing known */
    /* DV */
    int pal;              /* 1 = 625/50, 0 = 525/60, -1 = unknown */
    int wide;             /* 1 = 16:9, 0 = 4:3, -1 = unknown (no VAUX source control pack) */
    int audio_rate;       /* 48000 / 44100 / 32000, 0 = unknown (no AAUX source pack) */
    int audio_channels;   /* 2 or 4 (two stereo pairs), 0 = unknown */
    int audio_bits;       /* 16 or 12, 0 = unknown */
    /* HDV: from a sequence header (and its extension) */
    int width, height;    /* 0 = unknown */
    int fps_num, fps_den; /* 0 = unknown */
    int progressive;      /* progressive_sequence, -1 = unknown */
} pin_stream_fmt_t;

/* Everything unknown, of this kind (0 = none). */
void pin_stream_fmt_init(pin_stream_fmt_t *f, pin_kind_t kind);

/* 1 if a and b are of the same kind and no field known in both differs. */
int pin_stream_fmt_agrees(const pin_stream_fmt_t *a, const pin_stream_fmt_t *b);

/* "PAL DV 16:9, 32 kHz 12-bit 4 ch" / "HDV 1440x1080 25 fps interlaced", for the log. */
void pin_stream_fmt_describe(const pin_stream_fmt_t *f, char *out, size_t cap);

typedef struct {
    unsigned need_dv, need_hdv; /* PIN_FMTWATCH_DV_FRAMES / _HDV_GOPS by default */
    int have;                   /* a format is known */
    pin_stream_fmt_t cur;       /* the confirmed format */
    int pending;                /* another format is being watched */
    pin_stream_fmt_t cand;      /* it */
    unsigned cand_count;        /* units that counted towards it */
    long cand_start;            /* index of its first unit */
    long cand_last;             /* index of the last unit that counted (or of the first) */
} pin_fmtwatch_t;

void pin_fmtwatch_init(pin_fmtwatch_t *w);

typedef enum {
    PIN_FMTWATCH_SAME = 0, /* nothing to do (a pending candidate may have started or ended) */
    PIN_FMTWATCH_FIRST,    /* the first unit: its format is the format, at once */
    PIN_FMTWATCH_CHANGED,  /* a change was confirmed: w->cur is the new format */
} pin_fmtwatch_result_t;

/* One unit, in stream order (index increasing). On CHANGED, *start is the
 * index of the first unit of the new format and *old the format before it
 * (both may be NULL). */
pin_fmtwatch_result_t pin_fmtwatch_feed(pin_fmtwatch_t *w, const pin_stream_fmt_t *obs, long index,
                                        long *start, pin_stream_fmt_t *old);

/* 1 while another format is being watched, with *start = the index a change
 * would be reported at: hold that unit and everything after it. */
int pin_fmtwatch_pending(const pin_fmtwatch_t *w, long *start);

/* What a confirmed change does to the open output. */
typedef enum {
    PIN_FMTCHG_KEEP = 0,  /* the same file goes on: it carries the change per frame */
    PIN_FMTCHG_NEW_FILE,  /* the file ends and a new one starts with the new format */
    PIN_FMTCHG_STOP,      /* standard output cannot start a new file: the capture ends */
} pin_fmtchg_t;

/* active: the open file's format; to_stdout: it is the pipe ("-");
 * aspect_forced: the file's aspect is the user's, not the stream's.
 *   - DV <-> HDV: a new file (with the other kind's format), on a pipe a stop.
 *   - A pipe otherwise keeps going: raw DIF / TS carry standard, aspect, audio
 *     and video format in every frame or sequence header.
 *   - Raw DV (.dv): a new file on a PAL / NTSC change only (aspect and audio
 *     are per frame in raw DV).
 *   - Any other DV format (AVI, MOV, H.264, HEVC): a new file on a change of
 *     standard, aspect (unless forced) or audio (rate, channels, bits), which
 *     they fix when the file is opened.
 *   - HDV: a new file on a change of the video format (size, frame rate,
 *     progressive). */
pin_fmtchg_t pin_format_change_action(pin_format_t active, int to_stdout, int aspect_forced,
                                      const pin_stream_fmt_t *old_fmt, const pin_stream_fmt_t *new_fmt);

/* The stop detail for PIN_FMTCHG_STOP: "the input changed from DV to HDV; a pipe carries one
 * format only" (pin_stop_message() makes the sentence). */
void pin_format_change_stop_text(pin_kind_t from, pin_kind_t to, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* PIN_FMTWATCH_H */
