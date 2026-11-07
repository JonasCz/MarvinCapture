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

/* fseeko / 64-bit offsets for tapes of tens of GB */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L

#include "pout_tape.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dv_subcode.h"
#include "hdv_aux.h"

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

#define PCR_HZ 27000000.0
#define PCR_WRAP ((int64_t)8589934592LL * 300) /* 2^33 * 300 ticks */
#define SAMPLE_GAP_TICKS 2700000.0             /* keep one PCR sample per ~100 ms */
#define DV_MAX_FRAME 144000
#define DV_PROBE 50          /* frames looked at from each end of a DV file for a timecode */
#define DV_TC_READ_BUDGET 4096
#define DV_VERIFY_WINDOW 30
#define TS_MAX_STRIDE 192

typedef struct {
    int64_t first; /* first frame (in the file) of a run of consecutive timecodes */
    int64_t count; /* its timecode as a frame count */
} seg_t;

typedef struct {
    uint64_t pkt;
    int valid; /* timecode present and not all zero */
    int h, m, s, f, drop;
} gop_t;

typedef struct {
    char *path;
    uint64_t size;
    pout_media_t media;
    int is625;
    int sig; /* standard signature, to compare files */
    char fmt[24];
    int64_t frames; /* in this file */
    int64_t base;   /* first tape frame of this file */
    /* DV */
    size_t frame_bytes;
    /* HDV */
    unsigned stride, start; /* 188 or 192 (M2TS), offset of the first packet */
    uint64_t npk;           /* whole packets with sync */
    uint64_t *spk;          /* PCR samples: packet index ... */
    double *stl;            /* ... and timeline in 27 MHz ticks, byte 0 = 0 */
    int ns;
    int64_t *gop_slot; /* slot (file frame) of each GOP start */
    int ngop;
    /* timecode */
    int has_tc;
    int fdrop; /* drop-frame flag the file's own timecode counts in */
    seg_t *segs;
    int nseg, capseg;
    int64_t minc, maxc;
    int range_ok;
} file_t;

struct pout_tape {
    file_t *files;
    int nfiles;
    pout_media_t media;
    int is625;
    double fps;
    int nominal; /* 25 or 30 */
    int drop;    /* tape timecode is drop frame */
    int64_t frames;
    size_t max_bytes;
    uint8_t *scratch; /* DV frame buffer for timecode probes; HDV M2TS read buffer */
    size_t scratch_cap;
    FILE *cur;
    int cur_idx;
};

static void set_err(char *err, size_t cap, const char *fmt, ...)
{
    if (!err || cap == 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

static int seek64(FILE *f, uint64_t off)
{
#ifdef _WIN32
    return _fseeki64(f, (long long)off, SEEK_SET);
#else
    return fseeko(f, (off_t)off, SEEK_SET);
#endif
}

static int file_size64(FILE *f, uint64_t *size)
{
#ifdef _WIN32
    if (_fseeki64(f, 0, SEEK_END) != 0)
        return -1;
    long long p = _ftelli64(f);
#else
    if (fseeko(f, 0, SEEK_END) != 0)
        return -1;
    long long p = (long long)ftello(f);
#endif
    if (p < 0)
        return -1;
    *size = (uint64_t)p;
    return 0;
}

static int64_t mod64(int64_t a, int64_t m)
{
    a %= m;
    return a < 0 ? a + m : a;
}

/* Timecode label <-> frame count. Drop frame (29.97 only) skips labels :00 and
 * :01 of every minute except each tenth. */
static int64_t day_frames(int nominal, int drop)
{
    if (drop && nominal == 30)
        return 2589408;
    return (int64_t)nominal * 86400;
}

static int64_t label_to_count(int h, int m, int s, int f, int nominal, int drop)
{
    int64_t c = ((int64_t)h * 3600 + m * 60 + s) * nominal + f;
    if (drop && nominal == 30) {
        int64_t tm = (int64_t)h * 60 + m;
        c -= 2 * (tm - tm / 10);
    }
    return c;
}

static void count_to_label(int64_t c, int nominal, int drop, int *h, int *m, int *s, int *f)
{
    c = mod64(c, day_frames(nominal, drop));
    if (drop && nominal == 30) {
        int64_t d = c / 17982, r = c % 17982;
        c += 18 * d + (r >= 2 ? 2 * ((r - 2) / 1798) : 0);
    }
    *f = (int)(c % nominal);
    *s = (int)((c / nominal) % 60);
    *m = (int)((c / ((int64_t)nominal * 60)) % 60);
    *h = (int)((c / ((int64_t)nominal * 3600)) % 24);
}

static int label_ok(int h, int m, int s, int f, int nominal)
{
    return h >= 0 && h < 24 && m >= 0 && m < 60 && s >= 0 && s < 60 && f >= 0 && f < nominal;
}

/* Appends a run, dropping it if it just continues the previous one. */
static int seg_push(file_t *f, int64_t first, int64_t count, int nominal)
{
    int64_t day = day_frames(nominal, f->fdrop);
    count = mod64(count, day);
    if (f->nseg > 0) {
        seg_t *p = &f->segs[f->nseg - 1];
        if (first == p->first) {
            p->count = count;
            return 0;
        }
        if (mod64(p->count + (first - p->first), day) == count)
            return 0;
    }
    if (f->nseg == f->capseg) {
        int nc = f->capseg ? f->capseg * 2 : 8;
        seg_t *n = (seg_t *)realloc(f->segs, (size_t)nc * sizeof(seg_t));
        if (!n)
            return -1;
        f->segs = n;
        f->capseg = nc;
    }
    f->segs[f->nseg].first = first;
    f->segs[f->nseg].count = count;
    f->nseg++;
    return 0;
}

/* ------------------------------------------------------------------ */
/* DV                                                                  */
/* ------------------------------------------------------------------ */

/* Reads one frame's subcode: only the first three DIF blocks (header + the two
 * subcode blocks) of each sequence, into a zeroed frame buffer that
 * dv_subcode_parse_frame accepts. Returns 1 with the label, 0 if the frame has
 * no valid timecode, -1 on a read error. */
static int dv_tc_at(FILE *fp, const file_t *f, uint8_t *buf, int64_t local, int nominal, int *h, int *m,
                    int *s, int *fr, int *drop)
{
    unsigned seqs = (unsigned)(f->frame_bytes / DV_SEQ_SIZE);
    uint64_t off = (uint64_t)local * f->frame_bytes;
    memset(buf, 0, f->frame_bytes);
    for (unsigned q = 0; q < seqs; q++) {
        if (seek64(fp, off + (uint64_t)q * DV_SEQ_SIZE) != 0)
            return -1;
        if (fread(buf + (size_t)q * DV_SEQ_SIZE, 1, 3 * DV_BLOCK_SIZE, fp) != 3 * DV_BLOCK_SIZE)
            return -1;
    }
    dv_frame_info_t info;
    if (dv_subcode_parse_frame(buf, f->frame_bytes, &info) != 0 || !info.timecode.valid)
        return 0;
    if (!label_ok(info.timecode.hour, info.timecode.minute, info.timecode.second, info.timecode.frame,
                  nominal))
        return 0;
    *h = info.timecode.hour;
    *m = info.timecode.minute;
    *s = info.timecode.second;
    *fr = info.timecode.frame;
    *drop = (nominal == 30 && info.timecode.drop_frame) ? 1 : 0;
    return 1;
}

typedef struct {
    FILE *fp;
    file_t *f;
    uint8_t *buf;
    int nominal;
    int budget;
    int fail;
} dv_ctx_t;

/* Count of the frame's own timecode in the file's drop domain, or -1. */
static int64_t dv_count_at(dv_ctx_t *c, int64_t local)
{
    int h, m, s, fr, drop;
    if (--c->budget < 0) {
        c->fail = 1;
        return -1;
    }
    if (dv_tc_at(c->fp, c->f, c->buf, local, c->nominal, &h, &m, &s, &fr, &drop) != 1)
        return -1;
    return label_to_count(h, m, s, fr, c->nominal, c->f->fdrop);
}

/* Fills runs for frames [a, b) given valid counts at a and b: if the two ends
 * are as far apart in timecode as in frames the file is taken as linear in
 * between, else bisected. A bounded number of probes; the caller gives up on
 * the file's timecode if the budget runs out (timecode all over the place). */
static void dv_runs(dv_ctx_t *c, int64_t a, int64_t ca, int64_t b, int64_t cb)
{
    int64_t day = day_frames(c->nominal, c->f->fdrop);
    int64_t span = b - a;
    if (c->fail)
        return;
    if (span <= 1 || mod64(cb - ca, day) == span % day) {
        if (seg_push(c->f, a, ca, c->nominal) != 0)
            c->fail = 1;
        return;
    }
    int64_t m = a + span / 2, cm = -1;
    for (int64_t i = m; i < b && cm < 0 && !c->fail; i++) {
        cm = dv_count_at(c, i);
        if (cm >= 0)
            m = i;
    }
    for (int64_t i = a + span / 2 - 1; i > a && cm < 0 && !c->fail; i--) {
        cm = dv_count_at(c, i);
        if (cm >= 0)
            m = i;
    }
    if (c->fail)
        return;
    if (cm < 0) { /* no usable frame between: take it as linear */
        if (seg_push(c->f, a, ca, c->nominal) != 0)
            c->fail = 1;
        return;
    }
    dv_runs(c, a, ca, m, cm);
    dv_runs(c, m, cm, b, cb);
}

static int dv_index_tc(FILE *fp, file_t *f, uint8_t *buf, int nominal)
{
    dv_ctx_t c = {fp, f, buf, nominal, DV_TC_READ_BUDGET, 0};
    int h = 0, m = 0, s = 0, fr = 0, drop = 0;
    int64_t a = -1, b = -1, ca = 0, cb = 0;

    for (int64_t i = 0; i < f->frames && i < DV_PROBE && a < 0; i++)
        if (dv_tc_at(fp, f, buf, i, nominal, &h, &m, &s, &fr, &drop) == 1) {
            a = i;
            f->fdrop = drop;
        }
    if (a < 0)
        return 0;
    ca = label_to_count(h, m, s, fr, nominal, f->fdrop);
    for (int64_t i = f->frames - 1; i >= a && i > f->frames - 1 - DV_PROBE && b < 0; i--)
        if (dv_tc_at(fp, f, buf, i, nominal, &h, &m, &s, &fr, &drop) == 1)
            b = i;
    if (b < 0)
        b = a, cb = ca;
    else
        cb = label_to_count(h, m, s, fr, nominal, f->fdrop);
    /* the same timecode at both ends of a long file: stuck, not a timecode */
    if (b > a && cb == ca)
        return 0;
    if (b == a) {
        if (seg_push(f, a, ca, nominal) != 0)
            return 0;
    } else {
        dv_runs(&c, a, ca, b, cb);
        if (!c.fail)
            seg_push(f, b, cb, nominal);
    }
    if (c.fail || f->nseg == 0) {
        free(f->segs);
        f->segs = NULL;
        f->nseg = f->capseg = 0;
        return 0;
    }
    /* the first run reaches back to frame 0 */
    f->segs[0].count = mod64(f->segs[0].count - f->segs[0].first, day_frames(nominal, f->fdrop));
    f->segs[0].first = 0;
    return 1;
}

static int dv_detect(const uint8_t *d, size_t n, int *is625)
{
    if (n < 5 * DV_BLOCK_SIZE)
        return 0;
    /* block 0 header section (ID0 bits 7-5 = 0, block number 0), blocks 1-2
     * subcode (1), block 3 VAUX (2); DSF is bit 7 of the 4th byte of the header */
    if ((d[0] >> 5) != 0 || d[2] != 0)
        return 0;
    if ((d[DV_BLOCK_SIZE] >> 5) != 1 || (d[2 * DV_BLOCK_SIZE] >> 5) != 1 || (d[3 * DV_BLOCK_SIZE] >> 5) != 2)
        return 0;
    *is625 = (d[3] & 0x80) ? 1 : 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* HDV                                                                 */
/* ------------------------------------------------------------------ */

static int ts_detect(const uint8_t *b, size_t n, unsigned *stride, unsigned *start)
{
    static const unsigned strides[2] = {188, 192};
    for (int k = 0; k < 2; k++) {
        unsigned st = strides[k], so = (st == 192) ? 4 : 0;
        for (unsigned s = 0; s < st; s++) {
            int ok = 1;
            for (unsigned i = 0; i < 5 && ok; i++) {
                size_t pos = (size_t)s + so + (size_t)i * st;
                if (pos >= n || b[pos] != HDV_TS_SYNC_BYTE)
                    ok = 0;
            }
            if (ok) {
                *stride = st;
                *start = s;
                return 1;
            }
        }
    }
    return 0;
}

/* time on the file's timeline -> first packet of the slot starting at frame k */
static uint64_t hdv_boundary(const file_t *f, int64_t k, double fps)
{
    if (k <= 0)
        return 0;
    if (k >= f->frames)
        return f->npk;
    double t = (double)k * PCR_HZ / fps;
    int lo = 0, hi = f->ns - 1;
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (f->stl[mid] <= t)
            lo = mid;
        else
            hi = mid;
    }
    double dtl = f->stl[lo + 1] - f->stl[lo];
    uint64_t dpk = f->spk[lo + 1] - f->spk[lo];
    double x = ceil((t - f->stl[lo]) / dtl * (double)dpk);
    uint64_t r = f->spk[lo] + (x < 0 ? 0 : (uint64_t)x);
    return r > f->spk[lo + 1] ? f->spk[lo + 1] : r;
}

/* the frame (slot) a packet falls in: the last k whose slot starts at or before it */
static int64_t hdv_slot_of_packet(const file_t *f, uint64_t pkt, double fps)
{
    int64_t lo = 0, hi = f->frames - 1;
    while (lo < hi) {
        int64_t mid = (lo + hi + 1) / 2;
        if (hdv_boundary(f, mid, fps) <= pkt)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

typedef struct {
    gop_t *g;
    int n, cap;
    int pending; /* entry waiting for its GOP header, or -1 */
    uint8_t carry[8];
    int carry_len;
} es_scan_t;

static gop_t *gop_add(es_scan_t *e, uint64_t pkt)
{
    if (e->n == e->cap) {
        int nc = e->cap ? e->cap * 2 : 256;
        gop_t *n = (gop_t *)realloc(e->g, (size_t)nc * sizeof(gop_t));
        if (!n)
            return NULL;
        e->g = n;
        e->cap = nc;
    }
    gop_t *g = &e->g[e->n++];
    memset(g, 0, sizeof(*g));
    g->pkt = pkt;
    return g;
}

/* MPEG-2 GOP header time_code, 4 bytes after 00 00 01 B8, as hdv_aux.h
 * describes it. Filled into hdv_gop_time_t so its zero test is reused. */
static void gop_time_from_bytes(const uint8_t *b, hdv_gop_time_t *g)
{
    memset(g, 0, sizeof(*g));
    g->drop_frame = b[0] >> 7;
    g->hours = (b[0] >> 2) & 31;
    g->minutes = ((b[0] & 3) << 4) | (b[1] >> 4);
    g->seconds = ((b[1] & 7) << 3) | (b[2] >> 5);
    g->pictures = ((b[2] & 31) << 1) | (b[3] >> 7);
    g->found = 1;
}

/* Looks at a few video packets from the start of each PES for the sequence
 * header (00 00 01 B3: a GOP start for play_start) and the GOP header after it
 * (00 00 01 B8: timecode). Patterns that straddle two packets are caught with
 * an 7-byte carry. */
static int es_scan(es_scan_t *e, const uint8_t *pl, int len, uint64_t pkt, int pusi)
{
    uint8_t t[8 + 184];
    int cl = pusi ? 0 : e->carry_len;
    if (pusi)
        e->pending = (e->pending >= 0 && pkt - e->g[e->pending].pkt > 3) ? -1 : e->pending;
    memcpy(t, e->carry, (size_t)cl);
    memcpy(t + cl, pl, (size_t)len);
    int total = cl + len;
    for (int i = 0; i + 4 <= total; i++) {
        if (t[i] || t[i + 1] || t[i + 2] != 1)
            continue;
        if (t[i + 3] == 0xB3 && i + 3 >= cl) {
            if (!gop_add(e, pkt))
                return -1;
            e->pending = e->n - 1;
        } else if (t[i + 3] == 0xB8 && i + 8 <= total && i + 7 >= cl) {
            hdv_gop_time_t gt;
            gop_time_from_bytes(t + i + 4, &gt);
            gop_t *g;
            if (e->pending >= 0 && !e->g[e->pending].valid && pkt - e->g[e->pending].pkt <= 3) {
                g = &e->g[e->pending];
            } else {
                g = gop_add(e, pkt);
                if (!g)
                    return -1;
            }
            e->pending = -1;
            g->valid = hdv_gop_time_nonzero(&gt) && gt.hours < 24 && gt.minutes < 60 && gt.seconds < 60;
            g->h = gt.hours;
            g->m = gt.minutes;
            g->s = gt.seconds;
            g->f = gt.pictures;
            g->drop = gt.drop_frame;
        }
    }
    e->carry_len = total < 7 ? total : 7;
    memcpy(e->carry, t + total - e->carry_len, (size_t)e->carry_len);
    return 0;
}

/* Reads the first sequence header after packet `pkt` to learn the picture size
 * and frame rate code. */
static int hdv_seq_info(FILE *fp, const file_t *f, uint64_t pkt, int vpid, int *w, int *h, int *frc)
{
    uint8_t raw[TS_MAX_STRIDE], es[8 * 184];
    size_t n = 0;
    if (seek64(fp, f->start + pkt * f->stride) != 0)
        return -1;
    for (int i = 0; i < 8; i++) {
        if (fread(raw, 1, f->stride, fp) != f->stride)
            break;
        const uint8_t *p = raw + (f->stride == 192 ? 4 : 0);
        int pid = ((p[1] & 0x1F) << 8) | p[2];
        int afc = (p[3] >> 4) & 3;
        int off = 4 + ((afc & 2) ? 1 + p[4] : 0);
        if (pid != vpid || !(afc & 1) || off >= 188)
            continue;
        memcpy(es + n, p + off, (size_t)(188 - off));
        n += (size_t)(188 - off);
    }
    size_t at = hdv_find_sequence_header(es, n);
    if (at == (size_t)-1 || at + 8 > n)
        return -1;
    const uint8_t *s = es + at + 4;
    *w = (s[0] << 4) | (s[1] >> 4);
    *h = ((s[1] & 15) << 8) | s[2];
    *frc = s[3] & 15;
    return 0;
}

static int hdv_open_file(file_t *f, FILE *fp, const uint8_t *head, size_t head_n, char *err, size_t cap)
{
    if (!ts_detect(head, head_n, &f->stride, &f->start)) {
        set_err(err, cap, "%s: not a DV or HDV (MPEG-2 transport stream) file", f->path);
        return -1;
    }
    const unsigned so = f->stride == 192 ? 4 : 0;
    const size_t chunk = 8192;
    uint8_t *buf = (uint8_t *)malloc(chunk * f->stride);
    if (!buf) {
        set_err(err, cap, "%s: out of memory", f->path);
        return -1;
    }
    es_scan_t es;
    memset(&es, 0, sizeof(es));
    es.pending = -1;
    uint64_t idx = 0;
    int stop = 0, pcr_pid = -1, vpid = -1, scan_left = 0, have_prev = 0, rc = -1;
    int64_t prev_pcr = 0;
    uint64_t prev_pi = 0, first_pi = 0, last_pi = 0;
    double tl = 0, last_stored = 0, last_tl = 0;
    int cap_s = 0;
    f->ns = 0;
    if (seek64(fp, f->start) != 0)
        goto done;
    while (!stop) {
        size_t n = fread(buf, f->stride, chunk, fp);
        if (n == 0)
            break;
        for (size_t i = 0; i < n; i++) {
            const uint8_t *p = buf + i * f->stride + so;
            if (p[0] != HDV_TS_SYNC_BYTE) { /* lost sync: the rest is not tape */
                stop = 1;
                break;
            }
            uint64_t pi = idx++;
            int pid = ((p[1] & 0x1F) << 8) | p[2];
            int pusi = (p[1] & 0x40) != 0;
            int afc = (p[3] >> 4) & 3;
            int off = 4;
            if (afc & 2) {
                int al = p[4];
                if (al > 183)
                    continue;
                if (al >= 7 && (p[5] & 0x10) && (pcr_pid < 0 || pid == pcr_pid)) {
                    int64_t base = ((int64_t)p[6] << 25) | ((int64_t)p[7] << 17) | ((int64_t)p[8] << 9) |
                                   ((int64_t)p[9] << 1) | (p[10] >> 7);
                    int64_t pcr = base * 300 + (((p[10] & 1) << 8) | p[11]);
                    pcr_pid = pid;
                    if (!have_prev) {
                        tl = 0;
                        first_pi = pi;
                    } else if (pi > prev_pi) {
                        int64_t d = pcr - prev_pcr;
                        if (d < -PCR_WRAP / 2)
                            d += PCR_WRAP;
                        if (d <= 0 || d > (int64_t)(5 * PCR_HZ)) {
                            /* a PCR discontinuity: carry on by the byte rate so far */
                            double rate = (pi > first_pi && tl > 0) ? tl / (double)(prev_pi - first_pi + 1) : 1692.0;
                            tl += rate * (double)(pi - prev_pi);
                        } else {
                            tl += (double)d;
                        }
                    }
                    if (!have_prev || pi > prev_pi) {
                        prev_pcr = pcr;
                        prev_pi = pi;
                        have_prev = 1;
                        last_pi = pi;
                        last_tl = tl;
                        if (f->ns == 0 || (tl - last_stored >= SAMPLE_GAP_TICKS && pi > f->spk[f->ns - 1])) {
                            if (f->ns == cap_s) {
                                cap_s = cap_s ? cap_s * 2 : 1024;
                                uint64_t *a = (uint64_t *)realloc(f->spk, (size_t)cap_s * sizeof(uint64_t));
                                if (a)
                                    f->spk = a;
                                double *b = (double *)realloc(f->stl, (size_t)cap_s * sizeof(double));
                                if (b)
                                    f->stl = b;
                                if (!a || !b)
                                    goto done;
                            }
                            f->spk[f->ns] = pi;
                            f->stl[f->ns] = tl;
                            f->ns++;
                            last_stored = tl;
                        }
                    }
                }
                off = 5 + al;
            }
            if (!(afc & 1) || off >= 188)
                continue;
            if (vpid < 0 && pusi && off + 4 <= 188 && p[off] == 0 && p[off + 1] == 0 && p[off + 2] == 1 &&
                (p[off + 3] & 0xF0) == 0xE0)
                vpid = pid;
            if (pid == vpid) {
                if (pusi)
                    scan_left = 3;
                if (scan_left > 0) {
                    scan_left--;
                    if (es_scan(&es, p + off, 188 - off, pi, pusi) != 0)
                        goto done;
                }
            }
        }
    }
    f->npk = idx;
    if (have_prev && last_pi > f->spk[f->ns - 1]) { /* the last PCR as the last sample */
        if (f->ns == cap_s) {
            cap_s += 2;
            f->spk = (uint64_t *)realloc(f->spk, (size_t)cap_s * sizeof(uint64_t));
            f->stl = (double *)realloc(f->stl, (size_t)cap_s * sizeof(double));
            if (!f->spk || !f->stl)
                goto done;
        }
        f->spk[f->ns] = last_pi;
        f->stl[f->ns] = last_tl;
        f->ns++;
    }
    if (f->ns < 2 || f->spk[f->ns - 1] <= f->spk[0]) {
        set_err(err, cap, "%s: no usable PCR in the transport stream (cannot time it)", f->path);
        goto done;
    }
    if (es.n == 0 || vpid < 0) {
        set_err(err, cap, "%s: no MPEG-2 video (no sequence or GOP header found)", f->path);
        goto done;
    }

    /* Timeline: byte 0 = 0 (extend the first slope back), end = last sample
     * extended by the last slope to the end of the last whole packet. */
    {
        double slope0 = (f->stl[1] - f->stl[0]) / (double)(f->spk[1] - f->spk[0]);
        double shift = (double)f->spk[0] * slope0;
        for (int i = 0; i < f->ns; i++)
            f->stl[i] += shift;
        if (f->spk[0] > 0) {
            if (f->ns + 2 > cap_s) {
                cap_s = f->ns + 2;
                f->spk = (uint64_t *)realloc(f->spk, (size_t)cap_s * sizeof(uint64_t));
                f->stl = (double *)realloc(f->stl, (size_t)cap_s * sizeof(double));
                if (!f->spk || !f->stl)
                    goto done;
            }
            memmove(f->spk + 1, f->spk, (size_t)f->ns * sizeof(uint64_t));
            memmove(f->stl + 1, f->stl, (size_t)f->ns * sizeof(double));
            f->spk[0] = 0;
            f->stl[0] = 0;
            f->ns++;
        }
        if (f->npk > f->spk[f->ns - 1]) {
            if (f->ns + 1 > cap_s) {
                cap_s = f->ns + 1;
                f->spk = (uint64_t *)realloc(f->spk, (size_t)cap_s * sizeof(uint64_t));
                f->stl = (double *)realloc(f->stl, (size_t)cap_s * sizeof(double));
                if (!f->spk || !f->stl)
                    goto done;
            }
            double sl = (f->stl[f->ns - 1] - f->stl[f->ns - 2]) / (double)(f->spk[f->ns - 1] - f->spk[f->ns - 2]);
            f->stl[f->ns] = f->stl[f->ns - 1] + sl * (double)(f->npk - f->spk[f->ns - 1]);
            f->spk[f->ns] = f->npk;
            f->ns++;
        }
    }

    int w, h, frc;
    if (hdv_seq_info(fp, f, es.g[0].pkt, vpid, &w, &h, &frc) != 0) {
        /* the GOP header was found but not the sequence header after it */
        w = 1440, h = 1080, frc = 3;
    }
    if (frc < 1 || frc > 8) {
        set_err(err, cap, "%s: unsupported MPEG-2 frame rate code %d", f->path, frc);
        goto done;
    }
    f->is625 = (frc == 2 || frc == 3 || frc == 6);
    f->sig = (h << 8) | frc;
    if (h >= 1000) {
        snprintf(f->fmt, sizeof(f->fmt), "HDV 1080i%d", f->is625 ? 50 : 60);
    } else if (h == 720) {
        static const char *const r[9] = {"", "24", "24", "25", "30", "30", "50", "60", "60"};
        snprintf(f->fmt, sizeof(f->fmt), "HDV 720p%s", r[frc]);
    } else {
        snprintf(f->fmt, sizeof(f->fmt), "HDV %dx%d", w, h);
    }
    {
        double fps = f->is625 ? 25.0 : 30000.0 / 1001.0;
        double dur = f->stl[f->ns - 1] / PCR_HZ;
        f->frames = (int64_t)(dur * fps + 0.5);
        if (f->frames < 1)
            f->frames = 1;
        f->gop_slot = (int64_t *)malloc((size_t)es.n * sizeof(int64_t));
        if (!f->gop_slot)
            goto done;
        for (int i = 0; i < es.n; i++)
            f->gop_slot[i] = hdv_slot_of_packet(f, es.g[i].pkt, fps);
        f->ngop = es.n;

        /* Timecode of each GOP (all-zero ones count as missing), then runs of
         * consecutive timecode. */
        int nominal = f->is625 ? 25 : 30, any = 0;
        for (int i = 0; i < es.n; i++)
            if (es.g[i].valid) {
                if (!any)
                    f->fdrop = (nominal == 30 && es.g[i].drop) ? 1 : 0;
                any = 1;
            }
        if (any) {
            int64_t *cnt = (int64_t *)malloc((size_t)es.n * sizeof(int64_t));
            if (!cnt)
                goto done;
            int64_t day = day_frames(nominal, f->fdrop);
            int fv = -1;
            for (int i = 0; i < es.n; i++)
                if (es.g[i].valid) {
                    if (fv < 0)
                        fv = i;
                    cnt[i] = label_to_count(es.g[i].h, es.g[i].m, es.g[i].s, es.g[i].f, nominal, f->fdrop);
                }
            for (int i = fv - 1; i >= 0; i--)
                cnt[i] = mod64(cnt[i + 1] - (f->gop_slot[i + 1] - f->gop_slot[i]), day);
            for (int i = fv + 1; i < es.n; i++)
                if (!es.g[i].valid)
                    cnt[i] = mod64(cnt[i - 1] + (f->gop_slot[i] - f->gop_slot[i - 1]), day);
            for (int i = 0; i < es.n; i++)
                if (seg_push(f, i == 0 ? 0 : f->gop_slot[i],
                             i == 0 ? cnt[0] - f->gop_slot[0] : cnt[i], nominal) != 0) {
                    free(cnt);
                    goto done;
                }
            free(cnt);
            f->has_tc = 1;
        }
    }
    rc = 0;
done:
    if (rc != 0 && err && err[0] == 0)
        set_err(err, cap, "%s: cannot index the transport stream", f->path);
    free(es.g);
    free(buf);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Tape                                                                */
/* ------------------------------------------------------------------ */

static void file_free(file_t *f)
{
    free(f->path);
    free(f->spk);
    free(f->stl);
    free(f->gop_slot);
    free(f->segs);
}

void pout_tape_close(pout_tape_t *t)
{
    if (!t)
        return;
    if (t->cur)
        fclose(t->cur);
    for (int i = 0; i < t->nfiles; i++)
        file_free(&t->files[i]);
    free(t->files);
    free(t->scratch);
    free(t);
}

static int open_file(pout_tape_t *t, file_t *f, char *err, size_t cap)
{
    FILE *fp = fopen(f->path, "rb");
    if (!fp) {
        set_err(err, cap, "%s: cannot open", f->path);
        return -1;
    }
    int rc = -1;
    uint8_t head[2048];
    size_t hn;
    if (file_size64(fp, &f->size) != 0 || seek64(fp, 0) != 0) {
        set_err(err, cap, "%s: cannot read", f->path);
        goto out;
    }
    if (f->size == 0) {
        set_err(err, cap, "%s: file is empty", f->path);
        goto out;
    }
    hn = fread(head, 1, sizeof(head), fp);
    if (hn == 0) {
        set_err(err, cap, "%s: cannot read", f->path);
        goto out;
    }
    int is625;
    if (dv_detect(head, hn, &is625)) {
        f->media = POUT_MEDIA_DV;
        f->is625 = is625;
        f->frame_bytes = is625 ? 144000 : 120000;
        f->frames = (int64_t)(f->size / f->frame_bytes);
        f->sig = is625;
        snprintf(f->fmt, sizeof(f->fmt), "DV %s", is625 ? "PAL" : "NTSC");
        if (f->frames < 1) {
            set_err(err, cap, "%s: shorter than one %s DV frame (%zu bytes)", f->path, f->fmt, f->frame_bytes);
            goto out;
        }
        f->has_tc = dv_index_tc(fp, f, t->scratch, is625 ? 25 : 30);
        rc = 0;
    } else {
        f->media = POUT_MEDIA_HDV;
        if (hdv_open_file(f, fp, head, hn, err, cap) != 0)
            goto out;
        rc = 0;
    }
out:
    fclose(fp);
    return rc;
}

int pout_tape_open(pout_tape_t **out, const char *const *paths, int n, char *err, size_t err_cap)
{
    if (err && err_cap)
        err[0] = 0;
    if (out)
        *out = NULL;
    if (!out || !paths || n < 1) {
        set_err(err, err_cap, "no files given");
        return -1;
    }
    pout_tape_t *t = (pout_tape_t *)calloc(1, sizeof(*t));
    if (!t) {
        set_err(err, err_cap, "out of memory");
        return -1;
    }
    t->cur_idx = -1;
    t->scratch_cap = DV_MAX_FRAME;
    t->scratch = (uint8_t *)malloc(t->scratch_cap);
    t->files = (file_t *)calloc((size_t)n, sizeof(file_t));
    if (!t->scratch || !t->files) {
        set_err(err, err_cap, "out of memory");
        pout_tape_close(t);
        return -1;
    }
    for (int i = 0; i < n; i++) {
        file_t *f = &t->files[i];
        size_t pl = strlen(paths[i]);
        f->path = (char *)malloc(pl + 1);
        if (!f->path) {
            set_err(err, err_cap, "out of memory");
            pout_tape_close(t);
            return -1;
        }
        memcpy(f->path, paths[i], pl + 1);
        t->nfiles = i + 1; /* so close frees it on any error below */
        if (open_file(t, f, err, err_cap) != 0) {
            pout_tape_close(t);
            return -1;
        }
        if (i > 0) {
            const file_t *f0 = &t->files[0];
            if (f->media != f0->media || f->sig != f0->sig) {
                set_err(err, err_cap, "%s: is %s, but %s is %s: a tape holds one kind of video", f->path,
                        f->fmt, f0->path, f0->fmt);
                pout_tape_close(t);
                return -1;
            }
        }
        f->base = t->frames;
        t->frames += f->frames;
    }
    const file_t *f0 = &t->files[0];
    t->media = f0->media;
    t->is625 = f0->is625;
    t->nominal = f0->is625 ? 25 : 30;
    t->fps = f0->is625 ? 25.0 : 30000.0 / 1001.0;

    /* Tape timecode: drop frame as the first file that has a timecode says (a
     * 29.97 tape without any is drop frame); files without one continue the
     * previous file's. */
    t->drop = (t->nominal == 30);
    for (int i = 0; i < n; i++)
        if (t->files[i].has_tc) {
            t->drop = t->files[i].fdrop;
            break;
        }
    int64_t day = day_frames(t->nominal, t->drop), next = 0;
    for (int i = 0; i < n; i++) {
        file_t *f = &t->files[i];
        if (!f->has_tc) {
            free(f->segs);
            f->segs = (seg_t *)malloc(sizeof(seg_t));
            if (!f->segs) {
                set_err(err, err_cap, "out of memory");
                pout_tape_close(t);
                return -1;
            }
            f->segs[0].first = 0;
            f->segs[0].count = next;
            f->nseg = f->capseg = 1;
            f->fdrop = t->drop;
        } else if (f->fdrop != t->drop) {
            for (int s = 0; s < f->nseg; s++) {
                int h, m, sec, fr;
                count_to_label(f->segs[s].count, t->nominal, f->fdrop, &h, &m, &sec, &fr);
                f->segs[s].count = label_to_count(h, m, sec, fr, t->nominal, t->drop);
            }
            f->fdrop = t->drop;
        }
        f->minc = INT64_MAX;
        f->maxc = INT64_MIN;
        f->range_ok = 1;
        for (int s = 0; s < f->nseg; s++) {
            int64_t len = (s + 1 < f->nseg ? f->segs[s + 1].first : f->frames) - f->segs[s].first;
            int64_t lo = f->segs[s].count, hi = lo + len - 1;
            if (hi >= day) /* runs over midnight: no plain range */
                f->range_ok = 0;
            if (lo < f->minc)
                f->minc = lo;
            if (hi > f->maxc)
                f->maxc = hi;
        }
        const seg_t *ls = &f->segs[f->nseg - 1];
        next = mod64(ls->count + (f->frames - ls->first), day);
    }

    if (t->media == POUT_MEDIA_DV) {
        t->max_bytes = f0->frame_bytes;
    } else {
        size_t mx = 0;
        for (int i = 0; i < n; i++) {
            const file_t *f = &t->files[i];
            uint64_t a = 0;
            for (int64_t k = 0; k < f->frames; k++) {
                uint64_t b = hdv_boundary(f, k + 1, t->fps);
                if ((size_t)(b - a) * 188 > mx)
                    mx = (size_t)(b - a) * 188;
                a = b;
            }
        }
        t->max_bytes = mx;
        t->scratch_cap = mx / 188 * TS_MAX_STRIDE + TS_MAX_STRIDE;
        uint8_t *s = (uint8_t *)realloc(t->scratch, t->scratch_cap);
        if (!s) {
            set_err(err, err_cap, "out of memory");
            pout_tape_close(t);
            return -1;
        }
        t->scratch = s;
    }
    *out = t;
    return 0;
}

pout_media_t pout_tape_media(const pout_tape_t *t) { return t->media; }
int pout_tape_is_625(const pout_tape_t *t) { return t->is625; }
double pout_tape_fps(const pout_tape_t *t) { return t->fps; }
int64_t pout_tape_frames(const pout_tape_t *t) { return t->frames; }
size_t pout_tape_max_frame_bytes(const pout_tape_t *t) { return t->max_bytes; }
int pout_tape_file_count(const pout_tape_t *t) { return t->nfiles; }

const char *pout_tape_file_path(const pout_tape_t *t, int i)
{
    return (i >= 0 && i < t->nfiles) ? t->files[i].path : NULL;
}

/* Index of the file holding a tape frame. */
static int file_index(const pout_tape_t *t, int64_t frame)
{
    int lo = 0, hi = t->nfiles - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (t->files[mid].base <= frame)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

int pout_tape_file_of_frame(const pout_tape_t *t, int64_t frame, int *index)
{
    if (frame < 0 || frame >= t->frames)
        return -1;
    if (index)
        *index = file_index(t, frame);
    return 0;
}

/* The one open file is a cache; reading is logically const. */
static FILE *use_file(pout_tape_t *t, int i)
{
    if (t->cur && t->cur_idx == i)
        return t->cur;
    if (t->cur)
        fclose(t->cur);
    t->cur = fopen(t->files[i].path, "rb");
    t->cur_idx = t->cur ? i : -1;
    return t->cur;
}

static void fill_tc(const pout_tape_t *t, int h, int m, int s, int f, pin_tc_t *tc)
{
    tc->h = h;
    tc->m = m;
    tc->s = s;
    tc->f = f;
    tc->sep = t->drop ? ';' : ':';
}

/* Model timecode (runs) of a frame as a count. */
static int64_t model_count(const file_t *f, int64_t local, int64_t day)
{
    int lo = 0, hi = f->nseg - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (f->segs[mid].first <= local)
            lo = mid;
        else
            hi = mid - 1;
    }
    return mod64(f->segs[lo].count + (local - f->segs[lo].first), day);
}

int pout_tape_tc_of_frame(const pout_tape_t *ct, int64_t frame, pin_tc_t *tc)
{
    pout_tape_t *t = (pout_tape_t *)ct;
    if (!tc || frame < 0 || frame >= t->frames)
        return -1;
    int fi = file_index(t, frame);
    const file_t *f = &t->files[fi];
    int64_t local = frame - f->base, day = day_frames(t->nominal, t->drop);
    int h, m, s, fr;
    if (t->media == POUT_MEDIA_DV && f->has_tc) {
        FILE *fp = use_file(t, fi);
        int drop;
        if (fp && dv_tc_at(fp, f, t->scratch, local, t->nominal, &h, &m, &s, &fr, &drop) == 1) {
            fill_tc(t, h, m, s, fr, tc);
            return 0;
        }
    }
    count_to_label(model_count(f, local, day), t->nominal, t->drop, &h, &m, &s, &fr);
    fill_tc(t, h, m, s, fr, tc);
    return 0;
}

int64_t pout_tape_frame_of_tc(const pout_tape_t *ct, const pin_tc_t *tc)
{
    pout_tape_t *t = (pout_tape_t *)ct;
    if (!tc || !label_ok(tc->h, tc->m, tc->s, tc->f, t->nominal))
        return -1;
    int64_t day = day_frames(t->nominal, t->drop);
    int64_t want = mod64(label_to_count(tc->h, tc->m, tc->s, tc->f, t->nominal, t->drop), day);
    int64_t found = -1;
    int found_file = -1;
    for (int i = 0; i < t->nfiles && found < 0; i++) {
        const file_t *f = &t->files[i];
        for (int s = 0; s < f->nseg; s++) {
            int64_t len = (s + 1 < f->nseg ? f->segs[s + 1].first : f->frames) - f->segs[s].first;
            int64_t d = mod64(want - f->segs[s].count, day);
            if (d < len) {
                found = f->base + f->segs[s].first + d;
                found_file = i;
                break;
            }
        }
    }
    if (found < 0) { /* no exact match: the earliest file whose range holds it */
        for (int i = 0; i < t->nfiles; i++) {
            const file_t *f = &t->files[i];
            if (f->range_ok && want >= f->minc && want <= f->maxc)
                return f->base;
        }
        return -1;
    }
    if (t->media == POUT_MEDIA_DV && t->files[found_file].has_tc) {
        /* verify against the frame's own timecode, +-DV_VERIFY_WINDOW around it */
        const file_t *f = &t->files[found_file];
        FILE *fp = use_file(t, found_file);
        int64_t lo = found - f->base - DV_VERIFY_WINDOW, hi = found - f->base + DV_VERIFY_WINDOW;
        int64_t cl = found - f->base;
        if (lo < 0)
            lo = 0;
        if (hi >= f->frames)
            hi = f->frames - 1;
        int h, m, s, fr, drop, ok = 0;
        if (fp && dv_tc_at(fp, f, t->scratch, cl, t->nominal, &h, &m, &s, &fr, &drop) == 1 &&
            h == tc->h && m == tc->m && s == tc->s && fr == tc->f)
            ok = 1;
        for (int64_t i = lo; fp && !ok && i <= hi; i++)
            if (dv_tc_at(fp, f, t->scratch, i, t->nominal, &h, &m, &s, &fr, &drop) == 1 && h == tc->h &&
                m == tc->m && s == tc->s && fr == tc->f) {
                found = f->base + i;
                ok = 1;
            }
    }
    return found;
}

int pout_tape_read_frame(pout_tape_t *t, int64_t frame, uint8_t *buf, size_t cap, size_t *len)
{
    if (len)
        *len = 0;
    if (!buf || frame < 0 || frame >= t->frames)
        return -1;
    int fi = file_index(t, frame);
    const file_t *f = &t->files[fi];
    int64_t local = frame - f->base;
    FILE *fp = use_file(t, fi);
    if (!fp)
        return -1;
    if (t->media == POUT_MEDIA_DV) {
        if (cap < f->frame_bytes || seek64(fp, (uint64_t)local * f->frame_bytes) != 0 ||
            fread(buf, 1, f->frame_bytes, fp) != f->frame_bytes)
            return -1;
        if (len)
            *len = f->frame_bytes;
        return 0;
    }
    uint64_t a = hdv_boundary(f, local, t->fps), b = hdv_boundary(f, local + 1, t->fps);
    size_t n = (size_t)(b - a);
    if (n * 188 > cap)
        return -1;
    if (n > 0) {
        if (seek64(fp, f->start + a * f->stride) != 0)
            return -1;
        if (f->stride == 188) {
            if (fread(buf, 188, n, fp) != n)
                return -1;
        } else { /* M2TS: drop the 4-byte time stamp before every packet */
            if (fread(t->scratch, f->stride, n, fp) != n)
                return -1;
            for (size_t i = 0; i < n; i++)
                memcpy(buf + i * 188, t->scratch + i * f->stride + 4, 188);
        }
    }
    if (len)
        *len = n * 188;
    return 0;
}

int64_t pout_tape_play_start(const pout_tape_t *t, int64_t frame)
{
    if (frame < 0)
        return 0;
    if (frame >= t->frames)
        frame = t->frames - 1;
    if (t->media == POUT_MEDIA_DV)
        return frame;
    const file_t *f = &t->files[file_index(t, frame)];
    int64_t local = frame - f->base, best = 0;
    int lo = 0, hi = f->ngop - 1;
    while (lo <= hi) { /* last GOP slot <= local */
        int mid = (lo + hi) / 2;
        if (f->gop_slot[mid] <= local) {
            best = f->gop_slot[mid];
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return f->base + best;
}

static int ops_tc_of_frame(void *ctx, int64_t frame, pin_tc_t *tc)
{
    return pout_tape_tc_of_frame((const pout_tape_t *)ctx, frame, tc);
}

static int64_t ops_frame_of_tc(void *ctx, const pin_tc_t *tc)
{
    return pout_tape_frame_of_tc((const pout_tape_t *)ctx, tc);
}

void pout_tape_ops(pout_tape_t *t, pout_tape_ops_t *ops)
{
    ops->ctx = t;
    ops->frames = t->frames;
    ops->fps = t->fps;
    ops->tc_of_frame = ops_tc_of_frame;
    ops->frame_of_tc = ops_frame_of_tc;
}

void pout_tape_describe(const pout_tape_t *t, char *buf, size_t cap)
{
    if (!buf || cap == 0)
        return;
    int64_t n = t->frames;
    int nom = t->nominal;
    snprintf(buf, cap, "%s, %d file%s, %02d:%02d:%02d:%02d", t->files[0].fmt, t->nfiles,
             t->nfiles == 1 ? "" : "s", (int)(n / ((int64_t)nom * 3600)), (int)(n / ((int64_t)nom * 60) % 60),
             (int)(n / nom % 60), (int)(n % nom));
}
