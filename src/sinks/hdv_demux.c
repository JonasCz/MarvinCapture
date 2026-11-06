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
 * The first stage of the HDV pipeline (hdv_demux.h has the whole picture): the transport
 * stream cut into pictures, GOPs and MP2 audio frames.
 *
 * The PAT and PMT give the video PID (MPEG-2) and the audio PID (MPEG-1 Layer II). PES packets
 * are put back together per PID; a continuity-counter gap, a transport error bit or a lost sync
 * byte marks the PES being assembled as damaged. The video elementary stream is cut into
 * pictures at its start codes (a sequence or GOP header goes with the picture after it; two
 * field pictures make one frame), each with its PES PTS (the camera clock, unwrapped) when the
 * PES started with it, and the pictures into GOPs. Audio PES payloads are cut into MP2 frames;
 * a PES with a gap keeps only the frames in front of the gap.
 */

#include "hdv_demux.h"

#include <libavutil/avutil.h>
#include <libavutil/frame.h>

#include <stdlib.h>
#include <string.h>

#define TSP 188
#define NOPTS AV_NOPTS_VALUE

/* ------------------------------------------------------------- buffers -- */

typedef struct {
    uint8_t *d;
    size_t n, cap;
} buf_t;

static int buf_add(buf_t *b, const uint8_t *src, size_t len)
{
    if (b->n + len > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->n + len)
            cap *= 2;
        uint8_t *d = realloc(b->d, cap);
        if (!d)
            return -1;
        b->d = d;
        b->cap = cap;
    }
    if (len)
        memcpy(b->d + b->n, src, len);
    b->n += len;
    return 0;
}

static void buf_free(buf_t *b)
{
    free(b->d);
    memset(b, 0, sizeof(*b));
}

/* ------------------------------------------------------------ pictures -- */

int hdv_pics_push(hdv_pics_t *l, const hdv_pic_t *pic)
{
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 32;
        hdv_pic_t *p = realloc(l->p, (size_t)cap * sizeof(*p));
        if (!p)
            return -1;
        l->p = p;
        l->cap = cap;
    }
    l->p[l->n++] = *pic;
    return 0;
}

void hdv_pics_free(hdv_pics_t *l)
{
    for (int i = 0; i < l->n; i++)
        free(l->p[i].data);
    free(l->p);
    memset(l, 0, sizeof(*l));
}

size_t hdv_pics_bytes(const hdv_pics_t *l)
{
    size_t b = 0;
    for (int i = 0; i < l->n; i++)
        b += l->p[i].len;
    return b;
}

/* ------------------------------------------------------- stream values -- */

static const int k_fr[9][2] = { { 0, 1 }, { 24000, 1001 }, { 24, 1 }, { 25, 1 }, { 30000, 1001 },
                                { 30, 1 }, { 50, 1 }, { 60000, 1001 }, { 60, 1 } };

/* Parses the sequence header at d (its start code) and the extensions after it, up to end. */
static void parse_seq(const uint8_t *d, size_t end, hdv_seq_t *si)
{
    if (end < 12)
        return;
    hdv_seq_t s = *si;
    s.width = (d[4] << 4) | (d[5] >> 4);
    s.height = ((d[5] & 0x0F) << 8) | d[6];
    s.aspect_code = d[7] >> 4;
    s.frame_rate_code = d[7] & 0x0F;
    int64_t br = ((int64_t)d[8] << 10) | (d[9] << 2) | (d[10] >> 6);
    int vbv = ((d[10] & 0x1F) << 5) | (d[11] >> 3);
    s.progressive_seq = 0;
    s.chroma = 1;
    int fr_n = 0, fr_d = 0;
    size_t stop = end;
    for (size_t i = 12; i + 4 <= end; i++) {
        if (d[i] || d[i + 1] || d[i + 2] != 1)
            continue;
        if (d[i + 3] != 0xB5) {
            stop = i;         /* the sequence header and its extensions only */
            break;
        }
        const uint8_t *e = d + i;
        if (i + 10 <= end && (e[4] >> 4) == 1) {
            s.profile_level = ((e[4] & 0x0F) << 4) | (e[5] >> 4);
            s.progressive_seq = (e[5] >> 3) & 1;
            s.chroma = (e[5] >> 1) & 3;
            s.width |= (((e[5] & 1) << 1) | (e[6] >> 7)) << 12;
            s.height |= ((e[6] >> 5) & 3) << 12;
            br |= (int64_t)(((e[6] & 0x1F) << 7) | (e[7] >> 1)) << 18;
            vbv |= e[8] << 10;
            fr_n = (e[9] >> 5) & 3;
            fr_d = e[9] & 0x1F;
        }
    }
    if (s.frame_rate_code < 1 || s.frame_rate_code > 8 || s.width <= 0 || s.height <= 0)
        return;
    s.fps_num = k_fr[s.frame_rate_code][0] * (fr_n + 1);
    s.fps_den = k_fr[s.frame_rate_code][1] * (fr_d + 1);
    s.bit_rate = br == 0x3FFFF ? 0 : br * 400;
    s.vbv_bits = vbv * 16 * 1024;
    size_t hl = stop < sizeof(s.hdr) ? stop : sizeof(s.hdr);
    memcpy(s.hdr, d, hl);
    s.hdr_len = (int)hl;
    s.valid = 1;
    *si = s;
}

/* The picture coding extension after the picture header at d (len bytes from it). Returns
 * picture_structure (3 frame), or 3 if there is none; the other fields only if found. */
static int parse_picext(const uint8_t *d, size_t len, int *dc, int *qst, int *ivlc, int *alt,
                        int *tff)
{
    for (size_t i = 4; i + 9 <= len; i++) {
        if (d[i] || d[i + 1] || d[i + 2] != 1)
            continue;
        if (d[i + 3] != 0xB5)
            return 3;
        const uint8_t *e = d + i;
        if ((e[4] >> 4) != 8)
            continue;
        if (dc) {
            *dc = (e[6] >> 2) & 3;
            *tff = e[7] >> 7;
            *qst = (e[7] >> 4) & 1;
            *ivlc = (e[7] >> 3) & 1;
            *alt = (e[7] >> 2) & 1;
        }
        return e[6] & 3 ? e[6] & 3 : 3;
    }
    return 3;
}

/* ------------------------------------------------------------- audio --- */

static const int k_mp2_br[15] = { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384 };
static const int k_mp2_sr[3] = { 44100, 48000, 32000 };

/* An MPEG-1 Layer II frame header at d: its size, sample rate, channels, bit rate. 0 if not one. */
static int mp2_header(const uint8_t *d, int *sr, int *ch, int *kbps)
{
    if (d[0] != 0xFF || (d[1] & 0xFE) != 0xFC)    /* sync, MPEG-1, Layer II */
        return 0;
    int bri = d[2] >> 4, sri = (d[2] >> 2) & 3, pad = (d[2] >> 1) & 1;
    if (bri == 0 || bri == 15 || sri == 3)
        return 0;
    *sr = k_mp2_sr[sri];
    *kbps = k_mp2_br[bri];
    *ch = (d[3] >> 6) == 3 ? 1 : 2;
    return 144000 * *kbps / *sr + pad;
}

/* ------------------------------------------------------------- state --- */

struct hdv_demux {
    hdv_demux_cb_t cb;
    int tff_param;        /* the engine's field order, -1 none */
    int failed;
    int pmt_pid, vpid, apid;
    int cc[8192];         /* last continuity counter per PID (-1 none); only vpid/apid used */
    buf_t vpes, apes;
    int vpes_on, apes_on;
    int vpes_loss;
    long apes_loss_at;    /* payload bytes before the first loss, -1 none */
    int64_t last_ts;      /* the clock unwrap */
    buf_t es;
    size_t es_scan;       /* scanned up to here */
    int au_dmg, au_has_pic, au_fields, au_pic_off;
    int64_t au_pts;
    hdv_seq_t seq;        /* the first good sequence header */
    int picext_seen;
    hdv_pics_t gop;       /* the GOP being cut */
    int64_t next_id;      /* decoder ids handed out */
    buf_t acarry;         /* an audio frame cut by the end of a PES */
    int64_t a_next_cam;   /* camera time of the next audio frame, NOPTS */
};

static void nomem(hdv_demux_t *m)
{
    if (!m->failed && m->cb.error)
        m->cb.error(m->cb.user, PIN_ERR_NOMEM);
    m->failed = 1;
}

void hdv_gop_free(hdv_gop_t *g)
{
    if (!g)
        return;
    if (g->frames)
        for (int i = 0; i < g->pics.cap; i++)
            av_frame_free(&g->frames[i]);
    free(g->frames);
    hdv_pics_free(&g->pics);
    free(g);
}

/* Hands the GOP that was cut to the consumer. */
static void gop_out(hdv_demux_t *m)
{
    hdv_gop_t *g = calloc(1, sizeof(*g));
    if (g)
        g->frames = calloc((size_t)m->gop.cap + 1, sizeof(*g->frames));
    if (!g || !g->frames) {
        free(g);
        hdv_pics_free(&m->gop);
        nomem(m);
        return;
    }
    g->pics = m->gop;
    memset(&m->gop, 0, sizeof(m->gop));
    g->seq = m->seq;
    g->first_id = m->next_id;
    m->next_id += g->pics.n;
    g->bytes = hdv_pics_bytes(&g->pics);
    m->cb.gop(m->cb.user, g);
}

/* --------------------------------------------------------- GOPs out -- */

static void on_picture(hdv_demux_t *m, hdv_pic_t *p)
{
    if (m->gop.n > 0 && (p->gop_off >= 0 || p->seq || p->type == 1 || m->gop.n >= HDV_MAX_GOP_PICS))
        gop_out(m);
    if (!m->seq.valid) {
        free(p->data);    /* nothing can be written before the first sequence header */
        return;
    }
    if (hdv_pics_push(&m->gop, p) < 0)
        free(p->data);
}

/* --------------------------------------------- elementary stream -> AUs -- */

static void au_emit(hdv_demux_t *m, size_t len)
{
    const uint8_t *d = m->es.d;
    hdv_pic_t p;
    memset(&p, 0, sizeof(p));
    p.gop_off = -1;
    p.damaged = m->au_dmg;
    p.pts = m->au_pts;
    p.pic_off = m->au_has_pic ? m->au_pic_off : -1;
    if (p.pic_off < 0 || (size_t)p.pic_off + 6 > len) {
        p.damaged = 1;
        p.type = 0;
    } else {
        p.tr = (d[p.pic_off + 4] << 2) | (d[p.pic_off + 5] >> 6);
        p.type = (d[p.pic_off + 5] >> 3) & 7;
    }
    size_t hdr_end = p.pic_off >= 0 ? (size_t)p.pic_off : len;
    for (size_t i = 0; i + 8 <= hdr_end; i++) {
        if (d[i] || d[i + 1] || d[i + 2] != 1)
            continue;
        if (d[i + 3] == 0xB3) {
            p.seq = 1;
            if (!m->seq.valid && !p.damaged) {
                parse_seq(d + i, hdr_end - i, &m->seq);
            }
        } else if (d[i + 3] == 0xB8 && p.gop_off < 0) {
            p.gop_off = (int)i;
            p.closed_gop = (d[i + 7] >> 6) & 1;
            p.broken_link = (d[i + 7] >> 5) & 1;
        }
    }
    if (m->seq.valid && !m->picext_seen && !p.damaged && p.pic_off >= 0) {
        int dc = 0, qst = 0, ivlc = 0, alt = 0, tff = 1;
        parse_picext(d + p.pic_off, len - p.pic_off, &dc, &qst, &ivlc, &alt, &tff);
        m->seq.intra_dc = dc;
        m->seq.q_scale_type = qst;
        m->seq.intra_vlc = ivlc;
        m->seq.alt_scan = alt;
        m->seq.tff = m->tff_param >= 0 ? m->tff_param : tff;
        m->picext_seen = 1;
    }
    p.data = malloc(len ? len : 1);
    if (!p.data) {
        nomem(m);
        return;
    }
    memcpy(p.data, d, len);
    p.len = len;
    on_picture(m, &p);
}

/* Appends a PES payload of the video PID and cuts pictures off the front. */
static void es_feed(hdv_demux_t *m, const uint8_t *data, size_t len, int loss, int64_t pts)
{
    size_t old = m->es.n;         /* this payload starts here */
    if (buf_add(&m->es, data, len) < 0) {
        nomem(m);
        return;
    }
    int in_cur = 1;               /* the AU being assembled gets bytes of this payload */
    int ts_pending = pts != NOPTS;
    size_t from = m->es_scan;
    size_t cut = 0;               /* the current AU starts here in es */
    uint8_t *d = m->es.d;
    for (size_t i = from; i + 3 < m->es.n; i++) {
        if (d[i] || d[i + 1] || d[i + 2] != 1)
            continue;
        uint8_t code = d[i + 3];
        int boundary = 0;
        if (code == 0xB3 || code == 0xB8) {
            boundary = m->au_has_pic;
        } else if (code == 0x00) {
            if (m->au_has_pic) {
                /* the second field of a field-picture frame stays with the first */
                int st = parse_picext(d + cut + m->au_pic_off, i - cut - m->au_pic_off, NULL, NULL,
                                      NULL, NULL, NULL);
                if (st != 3 && m->au_fields == 1)
                    m->au_fields = 2;
                else
                    boundary = 1;
            }
        }
        if (boundary) {
            size_t au_len = i - cut;
            if (in_cur && i > old)
                m->au_dmg |= loss;    /* it ends inside this payload */
            in_cur = 0;
            /* emit d[cut..i): move it to the front temporarily */
            if (cut) {
                memmove(d, d + cut, m->es.n - cut);
                m->es.n -= cut;
                i -= cut;
                old = old > cut ? old - cut : 0;
                cut = 0;
            }
            au_emit(m, au_len);
            d = m->es.d;
            cut = au_len;
            m->au_has_pic = 0;
            m->au_fields = 0;
            m->au_pts = NOPTS;
            m->au_dmg = loss;   /* it ends in this payload */
        }
        if (code == 0x00 && !m->au_has_pic) {
            m->au_has_pic = 1;
            m->au_fields = 1;
            m->au_pic_off = (int)(i - cut);
            if (ts_pending) {     /* the PES's PTS is the first picture's starting in it */
                m->au_pts = pts;
                ts_pending = 0;
            }
        }
        i += 3;
    }
    if (in_cur)
        m->au_dmg |= loss;
    if (cut) {
        memmove(m->es.d, m->es.d + cut, m->es.n - cut);
        m->es.n -= cut;
    }
    m->es_scan = m->es.n > 3 ? m->es.n - 3 : 0;
}

static void es_flush(hdv_demux_t *m)
{
    if (m->es.n && m->au_has_pic)
        au_emit(m, m->es.n);
    m->es.n = 0;
    m->es_scan = 0;
    m->au_has_pic = 0;
    m->au_dmg = 0;
    m->au_pts = NOPTS;
}

/* ----------------------------------------------------------------- PES -- */

static int64_t unwrap(hdv_demux_t *m, int64_t v)
{
    if (m->last_ts == NOPTS) {
        m->last_ts = v;
        return v;
    }
    int64_t d = (v - m->last_ts) & ((1LL << 33) - 1);
    if (d >= (1LL << 32))
        d -= 1LL << 33;
    m->last_ts += d;
    return m->last_ts;
}

/* The PES header at d: its payload offset, and its PTS (NOPTS none). -1 if it is not one. */
static int pes_header(const uint8_t *d, size_t n, int64_t *pts)
{
    *pts = NOPTS;
    if (n < 9 || d[0] || d[1] || d[2] != 1)
        return -1;
    int hl = d[8];
    if ((size_t)9 + hl > n)
        return -1;
    if ((d[7] & 0x80) && hl >= 5) {
        const uint8_t *t = d + 9;
        *pts = ((int64_t)(t[0] & 0x0E) << 29) | (t[1] << 22) | ((t[2] & 0xFE) << 14) | (t[3] << 7) |
               (t[4] >> 1);
    }
    return 9 + hl;
}

static void video_pes_done(hdv_demux_t *m)
{
    if (!m->vpes_on)
        return;
    m->vpes_on = 0;
    int64_t pts;
    int off = pes_header(m->vpes.d, m->vpes.n, &pts);
    if (off < 0) {
        /* no usable header: the picture it held is lost; what is assembled so far is damaged */
        m->au_dmg = 1;
        m->vpes.n = 0;
        return;
    }
    if (pts != NOPTS)
        pts = unwrap(m, pts);
    es_feed(m, m->vpes.d + off, m->vpes.n - (size_t)off, m->vpes_loss, pts);
    m->vpes.n = 0;
}

static void audio_pes_done(hdv_demux_t *m)
{
    if (!m->apes_on)
        return;
    m->apes_on = 0;
    int64_t pts;
    int off = pes_header(m->apes.d, m->apes.n, &pts);
    long limit = (long)m->apes.n;
    if (m->apes_loss_at >= 0 && m->apes_loss_at < limit)
        limit = m->apes_loss_at;      /* only the frames in front of a gap */
    if (off < 0 || off > limit) {
        m->acarry.n = 0;
        m->a_next_cam = NOPTS;
        m->apes.n = 0;
        return;
    }
    if (pts != NOPTS)
        pts = unwrap(m, pts);
    /* a frame cut by the end of the last PES goes on here (only if nothing was lost) */
    buf_t w;
    memset(&w, 0, sizeof(w));
    int64_t cam = NOPTS;
    if (m->acarry.n && m->a_next_cam != NOPTS) {
        buf_add(&w, m->acarry.d, m->acarry.n);
        cam = m->a_next_cam;
    }
    size_t carried = w.n;
    buf_add(&w, m->apes.d + off, (size_t)(limit - off));
    m->acarry.n = 0;
    m->a_next_cam = NOPTS;
    size_t pos = 0;
    int pts_used = 0;
    while (pos + 4 <= w.n) {
        int sr, ch, kbps;
        int fl = mp2_header(w.d + pos, &sr, &ch, &kbps);
        if (!fl) {
            pos++;
            cam = NOPTS;     /* not a frame where one should be: no time for what follows */
            continue;
        }
        if (pos >= carried && !pts_used) {
            pts_used = 1;
            if (pts != NOPTS)
                cam = pts;   /* the PES's PTS is its first frame's */
        }
        if (pos + (size_t)fl > w.n) {
            if (limit == (long)m->apes.n && cam != NOPTS) {
                buf_add(&m->acarry, w.d + pos, w.n - pos);
                m->a_next_cam = cam;
            }
            break;
        }
        if (cam != NOPTS)
            m->cb.audio(m->cb.user, w.d + pos, fl, cam, sr, ch, kbps);
        if (cam != NOPTS)
            cam += 1152LL * 90000 / sr;
        pos += (size_t)fl;
    }
    buf_free(&w);
    m->apes.n = 0;
}

static void parse_psi(hdv_demux_t *m, const uint8_t *pkt, int pid)
{
    if (!(pkt[1] & 0x40))
        return;
    int off = 4;
    if (pkt[3] & 0x20)
        off += 1 + pkt[4];
    if (off >= TSP)
        return;
    off += 1 + pkt[off];          /* pointer field */
    if (off + 8 > TSP)
        return;
    const uint8_t *s = pkt + off;
    int slen = ((s[1] & 0x0F) << 8) | s[2];
    int end = off + 3 + slen - 4; /* without the CRC */
    if (end > TSP)
        end = TSP;
    if (pid == 0 && s[0] == 0x00) {
        for (int i = off + 8; i + 4 <= end; i += 4) {
            int prog = (pkt[i] << 8) | pkt[i + 1];
            if (prog != 0) {
                m->pmt_pid = ((pkt[i + 2] & 0x1F) << 8) | pkt[i + 3];
                break;
            }
        }
    } else if (pid == m->pmt_pid && s[0] == 0x02 && off + 12 <= TSP) {
        int pil = ((s[10] & 0x0F) << 8) | s[11];
        int vpid = -1, apid = -1;
        for (int i = off + 12 + pil; i + 5 <= end;) {
            int type = pkt[i], epid = ((pkt[i + 1] & 0x1F) << 8) | pkt[i + 2];
            int eil = ((pkt[i + 3] & 0x0F) << 8) | pkt[i + 4];
            if (type == 0x02 && vpid < 0)
                vpid = epid;
            else if ((type == 0x03 || type == 0x04) && apid < 0)
                apid = epid;
            i += 5 + eil;
        }
        if (vpid > 0 && vpid != m->vpid) {
            m->vpid = vpid;
            m->vpes_on = 0;
            m->vpes.n = 0;
        }
        if (apid > 0 && apid != m->apid) {
            m->apid = apid;
            m->apes_on = 0;
            m->apes.n = 0;
        }
    }
}

static void ts_packet(hdv_demux_t *m, const uint8_t *pkt)
{
    if (pkt[0] != 0x47) {
        /* a lost sync: whatever PID it was, both streams may have lost a packet */
        m->vpes_loss = 1;
        if (m->apes_loss_at < 0)
            m->apes_loss_at = (long)m->apes.n;
        return;
    }
    int pid = ((pkt[1] & 0x1F) << 8) | pkt[2];
    int tei = pkt[1] & 0x80, pusi = pkt[1] & 0x40;
    int afc = (pkt[3] >> 4) & 3, cc = pkt[3] & 0x0F;
    if (pid == 0 || (m->pmt_pid > 0 && pid == m->pmt_pid)) {
        if (!tei)
            parse_psi(m, pkt, pid);
        return;
    }
    int is_v = pid == m->vpid, is_a = pid == m->apid;
    if (!is_v && !is_a)
        return;
    int off = 4;
    if (afc & 2)
        off += 1 + pkt[4];
    int has_payload = (afc & 1) && off < TSP;
    int loss = tei != 0;
    if (has_payload) {
        int last = m->cc[pid];
        if (last >= 0 && cc == last && !tei)
            return;                   /* a duplicate packet */
        if (last >= 0 && cc != ((last + 1) & 0x0F))
            loss = 1;
        m->cc[pid] = cc;
    }
    if (tei) {
        /* its payload cannot be trusted: as if it were lost */
        if (is_v)
            m->vpes_loss = 1;
        else if (m->apes_loss_at < 0)
            m->apes_loss_at = (long)m->apes.n;
        return;
    }
    if (!has_payload)
        return;
    if (is_v) {
        if (loss)
            m->vpes_loss = 1;
        if (pusi) {
            video_pes_done(m);
            m->vpes_on = 1;
            m->vpes_loss = 0;
            m->vpes.n = 0;
        } else if (!m->vpes_on) {
            m->au_dmg = 1;            /* the start of this PES is lost */
            return;
        }
        if (buf_add(&m->vpes, pkt + off, (size_t)(TSP - off)) < 0)
            nomem(m);
    } else {
        if (loss && m->apes_loss_at < 0)
            m->apes_loss_at = (long)m->apes.n;
        if (pusi) {
            audio_pes_done(m);
            m->apes_on = 1;
            m->apes_loss_at = -1;
            m->apes.n = 0;
        } else if (!m->apes_on) {
            return;
        }
        if (buf_add(&m->apes, pkt + off, (size_t)(TSP - off)) < 0)
            nomem(m);
    }
}

/* --------------------------------------------------------------- API --- */

hdv_demux_t *hdv_demux_create(const hdv_demux_cb_t *cb, int tff)
{
    hdv_demux_t *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->cb = *cb;
    m->tff_param = tff;
    m->pmt_pid = m->vpid = m->apid = -1;
    for (int i = 0; i < 8192; i++)
        m->cc[i] = -1;
    m->apes_loss_at = -1;
    m->last_ts = NOPTS;
    m->au_pts = NOPTS;
    m->a_next_cam = NOPTS;
    return m;
}

void hdv_demux_feed(hdv_demux_t *m, const uint8_t *ts, size_t len)
{
    for (size_t i = 0; i + TSP <= len; i += TSP)
        ts_packet(m, ts + i);
}

void hdv_demux_flush(hdv_demux_t *m)
{
    video_pes_done(m);
    audio_pes_done(m);
    es_flush(m);
    if (m->gop.n)
        gop_out(m);
}

void hdv_demux_free(hdv_demux_t *m)
{
    if (!m)
        return;
    hdv_pics_free(&m->gop);
    buf_free(&m->vpes);
    buf_free(&m->apes);
    buf_free(&m->es);
    buf_free(&m->acarry);
    free(m);
}
