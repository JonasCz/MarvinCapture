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

#include "pin_fmtwatch.h"
#include <stdio.h>
#include <string.h>

void pin_stream_fmt_init(pin_stream_fmt_t *f, pin_kind_t kind)
{
    memset(f, 0, sizeof(*f));
    f->kind = kind;
    f->pal = -1;
    f->wide = -1;
    f->progressive = -1;
}

/* both known (>= 0 for the tri-state fields, > 0 for the others) and different */
static int differ3(int a, int b) { return a >= 0 && b >= 0 && a != b; }
static int differ0(int a, int b) { return a > 0 && b > 0 && a != b; }

static int dv_std_differs(const pin_stream_fmt_t *a, const pin_stream_fmt_t *b)
{
    return differ3(a->pal, b->pal);
}

static int dv_audio_differs(const pin_stream_fmt_t *a, const pin_stream_fmt_t *b)
{
    return differ0(a->audio_rate, b->audio_rate) || differ0(a->audio_channels, b->audio_channels) ||
           differ0(a->audio_bits, b->audio_bits);
}

static int hdv_video_differs(const pin_stream_fmt_t *a, const pin_stream_fmt_t *b)
{
    if (differ0(a->width, b->width) || differ0(a->height, b->height) ||
        differ3(a->progressive, b->progressive))
        return 1;
    return a->fps_num > 0 && a->fps_den > 0 && b->fps_num > 0 && b->fps_den > 0 &&
           (long long)a->fps_num * b->fps_den != (long long)b->fps_num * a->fps_den;
}

int pin_stream_fmt_agrees(const pin_stream_fmt_t *a, const pin_stream_fmt_t *b)
{
    if (a->kind != b->kind)
        return 0;
    if (a->kind == PIN_KIND_HDV)
        return !hdv_video_differs(a, b);
    return !dv_std_differs(a, b) && !differ3(a->wide, b->wide) && !dv_audio_differs(a, b);
}

/* The fields dst does not know, from src (same kind). */
static void fill(pin_stream_fmt_t *dst, const pin_stream_fmt_t *src)
{
    if (dst->kind != src->kind)
        return;
    if (dst->pal < 0) dst->pal = src->pal;
    if (dst->wide < 0) dst->wide = src->wide;
    if (!dst->audio_rate) dst->audio_rate = src->audio_rate;
    if (!dst->audio_channels) dst->audio_channels = src->audio_channels;
    if (!dst->audio_bits) dst->audio_bits = src->audio_bits;
    if (!dst->width) dst->width = src->width;
    if (!dst->height) dst->height = src->height;
    if (!dst->fps_num || !dst->fps_den) {
        dst->fps_num = src->fps_num;
        dst->fps_den = src->fps_den;
    }
    if (dst->progressive < 0) dst->progressive = src->progressive;
}

void pin_stream_fmt_describe(const pin_stream_fmt_t *f, char *out, size_t cap)
{
    if (!out || !cap)
        return;
    if (f->kind == PIN_KIND_HDV) {
        if (f->width > 0 && f->fps_num > 0 && f->fps_den > 0)
            snprintf(out, cap, "HDV %dx%d %.2f fps %s", f->width, f->height,
                     (double)f->fps_num / f->fps_den,
                     f->progressive > 0 ? "progressive" : f->progressive == 0 ? "interlaced" : "");
        else
            snprintf(out, cap, "HDV");
    } else if (f->kind == PIN_KIND_DV) {
        char audio[48] = "";
        if (f->audio_rate > 0)
            snprintf(audio, sizeof(audio), ", %d kHz %d-bit %d ch", f->audio_rate / 1000,
                     f->audio_bits, f->audio_channels);
        snprintf(out, cap, "%sDV%s%s", f->pal > 0 ? "PAL " : f->pal == 0 ? "NTSC " : "",
                 f->wide > 0 ? " 16:9" : f->wide == 0 ? " 4:3" : "", audio);
    } else {
        snprintf(out, cap, "unknown");
    }
}

void pin_fmtwatch_init(pin_fmtwatch_t *w)
{
    memset(w, 0, sizeof(*w));
    w->need_dv = PIN_FMTWATCH_DV_FRAMES;
    w->need_hdv = PIN_FMTWATCH_HDV_GOPS;
    pin_stream_fmt_init(&w->cur, 0);
    pin_stream_fmt_init(&w->cand, 0);
}

/* Does this unit count towards a candidate: a DV frame that says its standard,
 * an HDV picture with a sequence header (one per GOP). */
static int decisive(const pin_stream_fmt_t *obs)
{
    return obs->kind == PIN_KIND_HDV ? obs->width > 0 : obs->pal >= 0;
}

pin_fmtwatch_result_t pin_fmtwatch_feed(pin_fmtwatch_t *w, const pin_stream_fmt_t *obs, long index,
                                        long *start, pin_stream_fmt_t *old)
{
    if (!obs->kind)
        return PIN_FMTWATCH_SAME;
    if (w->pending &&
        index - w->cand_last > (w->cand.kind == PIN_KIND_HDV ? PIN_FMTWATCH_HDV_STALE : PIN_FMTWATCH_DV_STALE))
        w->pending = 0; /* nothing said it again for too long */
    if (!w->have) {
        w->cur = *obs;
        w->have = 1;
        w->pending = 0;
        w->provisional = 1;
        w->first_start = index;
        w->first_count = decisive(obs) ? 1 : 0;
        return PIN_FMTWATCH_FIRST;
    }
    if (w->provisional) {
        /* the first format stands once enough units agree with it (or none contradicted it
         * for as long as a candidate would be watched) */
        if (pin_stream_fmt_agrees(&w->cur, obs) && decisive(obs))
            w->first_count++;
        unsigned need = w->cur.kind == PIN_KIND_HDV ? w->need_hdv : w->need_dv;
        if (w->first_count >= (need ? need : 1) ||
            index - w->first_start > (w->cur.kind == PIN_KIND_HDV ? PIN_FMTWATCH_HDV_STALE : PIN_FMTWATCH_DV_STALE))
            w->provisional = 0;
    }
    if (pin_stream_fmt_agrees(&w->cur, obs)) {
        /* the format goes on (and learns what it did not know); a candidate
         * this unit contradicts was a glitch */
        if (w->pending && !pin_stream_fmt_agrees(&w->cand, obs))
            w->pending = 0;
        fill(&w->cur, obs);
        return PIN_FMTWATCH_SAME;
    }
    if (w->pending && pin_stream_fmt_agrees(&w->cand, obs)) {
        fill(&w->cand, obs);
    } else {
        /* a new candidate: what this unit says, the rest as before (same kind) */
        w->cand = *obs;
        fill(&w->cand, &w->cur);
        w->pending = 1;
        w->cand_count = 0;
        w->cand_start = index;
        w->cand_last = index;
    }
    if (decisive(obs)) {
        w->cand_count++;
        w->cand_last = index;
    }
    unsigned need = w->cand.kind == PIN_KIND_HDV ? w->need_hdv : w->need_dv;
    if (w->cand_count < (need ? need : 1))
        return PIN_FMTWATCH_SAME;
    if (old)
        *old = w->cur;
    if (start)
        *start = w->provisional ? w->first_start : w->cand_start;
    w->cur = w->cand;
    w->pending = 0;
    w->provisional = 0;
    return PIN_FMTWATCH_CHANGED;
}

int pin_fmtwatch_pending(const pin_fmtwatch_t *w, long *start)
{
    if (w->pending && start)
        *start = w->cand_start;
    return w->pending;
}

int pin_fmtwatch_hold_from(const pin_fmtwatch_t *w, long *start)
{
    if (w->have && w->provisional) {
        if (start)
            *start = w->first_start;
        return 1;
    }
    return pin_fmtwatch_pending(w, start);
}

pin_fmtchg_t pin_format_change_action(pin_format_t active, int to_stdout, int aspect_forced,
                                      const pin_stream_fmt_t *o, const pin_stream_fmt_t *n)
{
    if (o->kind != n->kind)
        return to_stdout ? PIN_FMTCHG_STOP : PIN_FMTCHG_NEW_FILE;
    if (to_stdout)
        return PIN_FMTCHG_KEEP;
    if (n->kind == PIN_KIND_HDV)
        return hdv_video_differs(o, n) ? PIN_FMTCHG_NEW_FILE : PIN_FMTCHG_KEEP;
    if (dv_std_differs(o, n))
        return PIN_FMTCHG_NEW_FILE;
    if (active == PIN_FMT_DV_RAW)
        return PIN_FMTCHG_KEEP;
    if ((!aspect_forced && differ3(o->wide, n->wide)) || dv_audio_differs(o, n))
        return PIN_FMTCHG_NEW_FILE;
    return PIN_FMTCHG_KEEP;
}

static const char *kind_word(pin_kind_t k)
{
    return k == PIN_KIND_HDV ? "HDV" : k == PIN_KIND_DV ? "DV" : "analog";
}

void pin_format_change_stop_text(pin_kind_t from, pin_kind_t to, char *out, size_t cap)
{
    if (out && cap)
        snprintf(out, cap, "the input changed from %s to %s; a pipe carries one format only",
                 kind_word(from), kind_word(to));
}
