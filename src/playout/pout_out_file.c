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

#include "pout_out.h"

#include "dv_reassembler.h"
#include "pin_log.h"
#include "pin_naming.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The ring layout of the capture path (docs/protocol.md): device addresses 0x7000..0xFFFF.
 * Messages are cut at the ring's end like the device's, and are not aligned to records. */
#define RING_ADDR 0x7000u
#define RING_BYTES 0x9000u
#define MSG_BYTES 1024u

typedef struct {
    pout_out_t base; /* first: pout_out_t * converts to file_out_t * */
    FILE *fp;
    dv_output_t sink;
    dv_reassembler_t *r;
    uint8_t msg[MSG_BYTES];
    unsigned msg_len;
    uint32_t msg_addr, ring_pos;
    unsigned long cycle;
    int started, failed;
    /* the files written: names[0] is the path given, the others follow format switches */
    char **names;
    int nnames;
    int have_media;
    pout_media_t media;     /* the kind of the file being written */
} file_out_t;

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static int msg_flush(file_out_t *f)
{
    if (f->msg_len == 0)
        return 0;
    uint8_t buf[4 + MSG_BYTES];
    put_le32(buf, (9u << 28) | ((uint32_t)f->msg_len << 16) | f->msg_addr);
    memcpy(buf + 4, f->msg, f->msg_len);
    unsigned n = 4 + f->msg_len;
    f->msg_len = 0;
    return dv_reassembler_feed(f->r, buf, n);
}

static int ring_put(file_out_t *f, const uint8_t *p, size_t n)
{
    while (n > 0) {
        if (f->msg_len == 0)
            f->msg_addr = RING_ADDR + f->ring_pos;
        size_t k = MSG_BYTES - f->msg_len;
        if (k > n)
            k = n;
        if (k > RING_BYTES - f->ring_pos)
            k = RING_BYTES - f->ring_pos;
        memcpy(f->msg + f->msg_len, p, k);
        f->msg_len += (unsigned)k;
        f->ring_pos += (uint32_t)k;
        p += k;
        n -= k;
        if (f->ring_pos == RING_BYTES) {
            f->ring_pos = 0;
            if (msg_flush(f))
                return -1;
        } else if (f->msg_len == MSG_BYTES && msg_flush(f)) {
            return -1;
        }
    }
    return 0;
}

static int file_start(pout_out_t *o)
{
    file_out_t *f = (file_out_t *)o;
    if (f->started)
        return -1;
    f->r = (dv_reassembler_t *)calloc(1, sizeof *f->r);
    if (!f->r)
        return -1;
    dv_output_file(&f->sink, f->fp);
    if (dv_reassembler_init(f->r, &f->sink) != 0) {
        free(f->r);
        f->r = NULL;
        return -1;
    }
    f->msg_len = 0;
    f->ring_pos = 0;
    f->started = 1;
    return 0;
}

/* OHCI buffer-fill record: isoch header (dataLength, tag 1, channel 63, tcode 0xA), the packet
 * padded to a quadlet, trailer (xferStatus 0x8411, 3 bits of seconds + 13 bits of cycle). */
static int file_packet(pout_out_t *o, const uint8_t *pkt, unsigned len)
{
    file_out_t *f = (file_out_t *)o;
    static const uint8_t pad[4];
    uint8_t q[4];
    if (!f->started || f->failed)
        return -1;
    put_le32(q, ((uint32_t)len << 16) | (1u << 14) | (63u << 8) | (0xAu << 4));
    if (ring_put(f, q, 4) || ring_put(f, pkt, len) || ring_put(f, pad, ((len + 3u) & ~3u) - len))
        goto fail;
    put_le32(q, (0x8411u << 16) | (uint32_t)(((f->cycle / 8000) & 7) << 13) | (uint32_t)(f->cycle % 8000));
    if (ring_put(f, q, 4))
        goto fail;
    f->cycle++;
    return 0;
fail:
    f->failed = 1;
    return -1;
}

static void file_stop(pout_out_t *o)
{
    file_out_t *f = (file_out_t *)o;
    if (!f->started)
        return;
    msg_flush(f);
    dv_reassembler_finish(f->r);
    free(f->r);
    f->r = NULL;
    f->started = 0;
    fflush(f->fp);
}

static void file_destroy(pout_out_t *o)
{
    file_out_t *f = (file_out_t *)o;
    file_stop(o);
    if (f->fp)
        fclose(f->fp);
    for (int i = 0; i < f->nnames; i++)
        free(f->names[i]);
    free(f->names);
    free(f);
}

static int name_add(file_out_t *f, const char *path)
{
    char **n = (char **)realloc(f->names, (size_t)(f->nnames + 1) * sizeof(*n));
    if (!n)
        return -1;
    f->names = n;
    size_t len = strlen(path) + 1;
    n[f->nnames] = (char *)malloc(len);
    if (!n[f->nnames])
        return -1;
    memcpy(n[f->nnames], path, len);
    f->nnames++;
    return 0;
}

/* NAME-000N.EXT: the path given without a .dv/.m2t/.ts extension, the number of the file (as a
 * capture numbers scenes, pin_naming.c) and the extension of the kind. */
static int next_name(const file_out_t *f, pout_media_t media, char *out, size_t cap)
{
    char base[1024], tmp[1024];
    const char *first = f->names[0];
    if (pin_naming_strip_extension(first, "dv", base, sizeof base) != 0 ||
        pin_naming_strip_extension(base, "m2t", tmp, sizeof tmp) != 0 ||
        pin_naming_strip_extension(tmp, "ts", base, sizeof base) != 0)
        return -1;
    pin_naming_opts_t opts = { 0, 1, (unsigned)f->nnames + 1, 1 };
    return pin_naming_build(base, &opts, media == POUT_MEDIA_HDV ? "m2t" : "dv", out, cap);
}

static int file_format(pout_out_t *o, const pout_frame_fmt_t *fmt)
{
    file_out_t *f = (file_out_t *)o;
    if (!f->have_media) {
        f->have_media = 1;
        f->media = fmt->media;
        return 0;
    }
    if (fmt->media == f->media)
        return 0;       /* PAL <-> NTSC, 1080i <-> 720p: the same file */
    char path[1100];
    if (!f->started || next_name(f, fmt->media, path, sizeof path) != 0) {
        f->failed = 1;
        return -1;
    }
    /* finish the file of the old kind, start the next one with a fresh reassembler */
    file_stop(o);
    fclose(f->fp);
    f->fp = fopen(path, "wb");
    if (!f->fp || name_add(f, path) != 0) {
        f->failed = 1;
        return -1;
    }
    f->media = fmt->media;
    pin_logf(PIN_LOG_INFO, "playout: the format changes, continuing in %s\n", path);
    return file_start(o);
}

int pout_out_file_count(const pout_out_t *o)
{
    return o && o->start == file_start ? ((const file_out_t *)o)->nnames : 0;
}

const char *pout_out_file_name(const pout_out_t *o, int i)
{
    if (!o || o->start != file_start)
        return NULL;
    const file_out_t *f = (const file_out_t *)o;
    return i >= 0 && i < f->nnames ? f->names[i] : NULL;
}

pout_out_t *pout_out_file_create(const char *path, char *err, size_t err_cap)
{
    file_out_t *f = (file_out_t *)calloc(1, sizeof *f);
    if (!f) {
        if (err && err_cap)
            snprintf(err, err_cap, "out of memory");
        return NULL;
    }
    f->fp = fopen(path, "wb");
    if (!f->fp) {
        if (err && err_cap)
            snprintf(err, err_cap, "cannot create %s", path);
        free(f);
        return NULL;
    }
    f->base.start = file_start;
    f->base.packet = file_packet;
    f->base.stop = file_stop;
    f->base.destroy = file_destroy;
    f->base.format = file_format;
    f->base.priv = f;
    if (name_add(f, path) != 0) {
        fclose(f->fp);
        free(f);
        if (err && err_cap)
            snprintf(err, err_cap, "out of memory");
        return NULL;
    }
    return &f->base;
}
