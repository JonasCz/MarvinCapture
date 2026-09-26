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

#define _FILE_OFFSET_BITS 64

#include "avi_writer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define SEGMENT_LIMIT (1000u * 1024 * 1024)
#define SUPER_ENTRIES 1024          /* x 1 GB segments */
#define NSTREAMS 2

typedef struct {
    uint32_t offset;                /* chunk data, relative to the segment's RIFF */
    uint32_t size;
} ix_entry_t;

typedef struct {
    ix_entry_t *e;
    size_t n, cap;
} ix_list_t;

typedef struct {
    uint64_t offset;
    uint32_t size, duration;
} super_entry_t;

struct avi_writer {
    FILE *f;
    unsigned width, height, fps_num, fps_den, aspect_num, aspect_den, rate, channels;
    uint64_t frames, samples;
    uint64_t first_riff_frames;
    /* patch points */
    off_t avih_total, strh_len[NSTREAMS], dmlh_total, indx_at[NSTREAMS];
    /* current segment */
    off_t riff_at, movi_at;
    char title[256];        /* UTF-8, "" if none */
    int segment;
    uint64_t seg_samples;
    ix_list_t ix[NSTREAMS];
    super_entry_t super[NSTREAMS][SUPER_ENTRIES];
    unsigned nsuper[NSTREAMS];
    /* idx1 for the first segment: (stream, offset from 'movi', size) */
    struct { uint8_t s; uint32_t off, size; } *idx1;
    size_t nidx1, capidx1;
    int error;
};

static void put32(FILE *f, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    fwrite(b, 1, 4, f);
}

static void put16(FILE *f, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    fwrite(b, 1, 2, f);
}

static void put64(FILE *f, uint64_t v)
{
    put32(f, (uint32_t)v);
    put32(f, (uint32_t)(v >> 32));
}

static void fcc(FILE *f, const char *s)
{
    fwrite(s, 1, 4, f);
}

static void patch32(FILE *f, off_t at, uint32_t v)
{
    off_t here = ftello(f);
    fseeko(f, at, SEEK_SET);
    put32(f, v);
    fseeko(f, here, SEEK_SET);
}

/* Opens a chunk or list; returns where its size field is. */
static off_t begin(FILE *f, const char *id, const char *list_type)
{
    fcc(f, id);
    off_t at = ftello(f);
    put32(f, 0);
    if (list_type)
        fcc(f, list_type);
    return at;
}

static void end(FILE *f, off_t size_at)
{
    off_t here = ftello(f);
    patch32(f, size_at, (uint32_t)(here - size_at - 4));
    if ((here - size_at) & 1)
        fputc(0, f);   /* chunks are word aligned */
}

static const char *const chunk_id[NSTREAMS] = { "00dc", "01wb" };

static void write_super_index(avi_writer_t *w, int s)
{
    off_t at = begin(w->f, "indx", NULL);
    w->indx_at[s] = at - 4;
    put16(w->f, 4);                 /* wLongsPerEntry */
    fputc(0, w->f);                 /* bIndexSubType */
    fputc(0, w->f);                 /* bIndexType: AVI_INDEX_OF_INDEXES */
    put32(w->f, 0);                 /* nEntriesInUse, patched */
    fcc(w->f, chunk_id[s]);
    put32(w->f, 0);
    put32(w->f, 0);
    put32(w->f, 0);
    static const uint8_t zero[16];
    for (int i = 0; i < SUPER_ENTRIES; i++)
        fwrite(zero, 1, sizeof(zero), w->f);
    end(w->f, at);
}

static void write_headers(avi_writer_t *w)
{
    FILE *f = w->f;
    uint32_t frame_bytes = w->width * w->height * 2;
    uint32_t block = w->channels * 2;

    w->riff_at = begin(f, "RIFF", "AVI ");
    off_t hdrl = begin(f, "LIST", "hdrl");

    off_t avih = begin(f, "avih", NULL);
    put32(f, (uint32_t)(1000000ull * w->fps_den / w->fps_num));
    put32(f, (uint32_t)((uint64_t)frame_bytes * w->fps_num / w->fps_den + w->rate * block));
    put32(f, 0);
    put32(f, 0x10 | 0x100);         /* AVIF_HASINDEX | AVIF_ISINTERLEAVED */
    w->avih_total = ftello(f);
    put32(f, 0);                    /* dwTotalFrames (first RIFF), patched */
    put32(f, 0);
    put32(f, NSTREAMS);
    put32(f, frame_bytes + 8);
    put32(f, w->width);
    put32(f, w->height);
    for (int i = 0; i < 4; i++)
        put32(f, 0);
    end(f, avih);

    /* video */
    off_t strl = begin(f, "LIST", "strl");
    off_t strh = begin(f, "strh", NULL);
    fcc(f, "vids");
    fcc(f, "YUY2");
    put32(f, 0);
    put16(f, 0);
    put16(f, 0);
    put32(f, 0);
    put32(f, w->fps_den);           /* dwScale */
    put32(f, w->fps_num);           /* dwRate */
    put32(f, 0);
    w->strh_len[0] = ftello(f);
    put32(f, 0);                    /* dwLength, patched */
    put32(f, frame_bytes);
    put32(f, 0xffffffff);
    put32(f, 0);
    put16(f, 0);
    put16(f, 0);
    put16(f, (uint16_t)w->width);
    put16(f, (uint16_t)w->height);
    end(f, strh);
    off_t strf = begin(f, "strf", NULL);
    put32(f, 40);
    put32(f, w->width);
    put32(f, w->height);
    put16(f, 1);
    put16(f, 16);
    fcc(f, "YUY2");
    put32(f, frame_bytes);
    put32(f, 0);
    put32(f, 0);
    put32(f, 0);
    put32(f, 0);
    end(f, strf);
    /* OpenDML video properties: frame aspect and the two fields. */
    int pal = w->fps_num == 25 * w->fps_den;
    off_t vprp = begin(f, "vprp", NULL);
    put32(f, 0);                    /* VideoFormatToken: unknown */
    put32(f, pal ? 1 : 2);          /* VideoStandard: PAL / NTSC */
    put32(f, pal ? 50 : 60);        /* dwVerticalRefreshRate */
    put32(f, w->width);             /* dwHTotalInT */
    put32(f, pal ? 625 : 525);      /* dwVTotalInLines */
    put32(f, (w->aspect_num << 16) | w->aspect_den);
    put32(f, w->width);
    put32(f, w->height);
    put32(f, 2);                    /* nbFieldPerFrame */
    for (int i = 0; i < 2; i++) {
        put32(f, w->height / 2);    /* CompressedBMHeight */
        put32(f, w->width);         /* CompressedBMWidth */
        put32(f, w->height / 2);    /* ValidBMHeight */
        put32(f, w->width);         /* ValidBMWidth */
        put32(f, 0);                /* ValidBMXOffset */
        put32(f, 0);                /* ValidBMYOffset */
        put32(f, 0);                /* VideoXOffsetInT */
        put32(f, 0);                /* VideoYValidStartLine */
    }
    end(f, vprp);
    write_super_index(w, 0);
    end(f, strl);

    /* audio */
    strl = begin(f, "LIST", "strl");
    strh = begin(f, "strh", NULL);
    fcc(f, "auds");
    put32(f, 0);
    put32(f, 0);
    put16(f, 0);
    put16(f, 0);
    put32(f, 0);
    put32(f, 1);                    /* dwScale: one sample frame */
    put32(f, w->rate);              /* dwRate */
    put32(f, 0);
    w->strh_len[1] = ftello(f);
    put32(f, 0);                    /* dwLength in sample frames, patched */
    put32(f, w->rate * block / 10);
    put32(f, 0xffffffff);
    put32(f, block);                /* dwSampleSize */
    for (int i = 0; i < 4; i++)
        put16(f, 0);
    end(f, strh);
    strf = begin(f, "strf", NULL);
    put16(f, 1);                    /* PCM */
    put16(f, (uint16_t)w->channels);
    put32(f, w->rate);
    put32(f, w->rate * block);
    put16(f, (uint16_t)block);
    put16(f, 16);
    put16(f, 0);
    end(f, strf);
    write_super_index(w, 1);
    end(f, strl);

    off_t odml = begin(f, "LIST", "odml");
    off_t dmlh = begin(f, "dmlh", NULL);
    w->dmlh_total = ftello(f);
    put32(f, 0);
    for (int i = 0; i < 61; i++)
        put32(f, 0);
    end(f, dmlh);
    end(f, odml);
    end(f, hdrl);

    if (w->title[0]) {
        /* LIST INFO/INAM: the conventional place a title lives in a RIFF
         * file (Explorer/VLC/ffprobe all read it from here). Placed after
         * hdrl and before movi, so it is file-level metadata rather than
         * per-frame data. */
        off_t info = begin(f, "LIST", "INFO");
        size_t len = strlen(w->title) + 1;   /* NUL included, per RIFF text chunks */
        off_t inam = begin(f, "INAM", NULL);
        fwrite(w->title, 1, len, f);
        end(f, inam);
        end(f, info);
    }

    w->movi_at = begin(f, "LIST", "movi");
}

static void ix_add(ix_list_t *l, uint32_t off, uint32_t size)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 1024;
        l->e = realloc(l->e, l->cap * sizeof(*l->e));
    }
    l->e[l->n++] = (ix_entry_t){ off, size };
}

/* Standard index chunks for this segment, inside its movi list, then
 * registered in the super index. */
static void close_segment(avi_writer_t *w)
{
    FILE *f = w->f;
    for (int s = 0; s < NSTREAMS; s++) {
        ix_list_t *l = &w->ix[s];
        if (!l->n)
            continue;
        char id[5] = { 'i', 'x', chunk_id[s][0], chunk_id[s][1], 0 };
        off_t chunk = ftello(f);
        off_t at = begin(f, id, NULL);
        put16(f, 2);
        fputc(0, f);
        fputc(1, f);                /* AVI_INDEX_OF_CHUNKS */
        put32(f, (uint32_t)l->n);
        fcc(f, chunk_id[s]);
        put64(f, (uint64_t)w->riff_at - 4);   /* qwBaseOffset: the segment's RIFF */
        put32(f, 0);
        for (size_t i = 0; i < l->n; i++) {
            put32(f, l->e[i].offset);
            put32(f, l->e[i].size);
        }
        end(f, at);
        if (w->nsuper[s] < SUPER_ENTRIES) {
            super_entry_t *e = &w->super[s][w->nsuper[s]++];
            e->offset = (uint64_t)chunk;
            e->size = (uint32_t)(ftello(f) - chunk);
            e->duration = s == 0 ? (uint32_t)l->n : (uint32_t)w->seg_samples;
        } else {
            w->error = 1;
        }
        l->n = 0;
    }
    end(f, w->movi_at);

    if (w->segment == 0) {
        off_t at = begin(f, "idx1", NULL);
        for (size_t i = 0; i < w->nidx1; i++) {
            fcc(f, chunk_id[w->idx1[i].s]);
            put32(f, 0x10);         /* AVIIF_KEYFRAME */
            put32(f, w->idx1[i].off);
            put32(f, w->idx1[i].size);
        }
        end(f, at);
        patch32(f, w->avih_total, (uint32_t)w->first_riff_frames);
    }
    end(f, w->riff_at);
    w->seg_samples = 0;
}

static void open_segment(avi_writer_t *w)
{
    w->segment++;
    w->riff_at = begin(w->f, "RIFF", "AVIX");
    w->movi_at = begin(w->f, "LIST", "movi");
}

static int write_chunk(avi_writer_t *w, int s, const uint8_t *data, size_t len)
{
    if (!w || w->error)
        return -1;
    if ((uint64_t)(ftello(w->f) - w->riff_at) + len > SEGMENT_LIMIT) {
        close_segment(w);
        open_segment(w);
    }
    off_t at = begin(w->f, chunk_id[s], NULL);
    off_t data_at = ftello(w->f);
    fwrite(data, 1, len, w->f);
    end(w->f, at);
    ix_add(&w->ix[s], (uint32_t)(data_at - (w->riff_at - 4)), (uint32_t)len);
    if (w->segment == 0) {
        if (w->nidx1 == w->capidx1) {
            w->capidx1 = w->capidx1 ? w->capidx1 * 2 : 4096;
            w->idx1 = realloc(w->idx1, w->capidx1 * sizeof(*w->idx1));
        }
        w->idx1[w->nidx1].s = (uint8_t)s;
        w->idx1[w->nidx1].off = (uint32_t)(at - 4 - (w->movi_at + 4));
        w->idx1[w->nidx1].size = (uint32_t)len;
        w->nidx1++;
    }
    if (ferror(w->f))
        w->error = 1;
    return w->error ? -1 : 0;
}

avi_writer_t *avi_open_titled(const char *path, unsigned width, unsigned height,
                              unsigned fps_num, unsigned fps_den, unsigned aspect_num,
                              unsigned aspect_den, unsigned audio_rate, unsigned audio_channels,
                              const char *title_utf8)
{
    avi_writer_t *w = calloc(1, sizeof(*w));
    if (!w)
        return NULL;
    w->f = fopen(path, "wb");
    if (!w->f) {
        free(w);
        return NULL;
    }
    setvbuf(w->f, NULL, _IOFBF, 4 << 20);
    w->width = width;
    w->height = height;
    w->fps_num = fps_num;
    w->fps_den = fps_den;
    w->aspect_num = aspect_num;
    w->aspect_den = aspect_den;
    w->rate = audio_rate;
    w->channels = audio_channels;
    if (title_utf8 && title_utf8[0]) {
        size_t n = strlen(title_utf8);
        if (n >= sizeof(w->title))
            n = sizeof(w->title) - 1;
        memcpy(w->title, title_utf8, n);
        w->title[n] = '\0';
    }
    write_headers(w);
    return w;
}

avi_writer_t *avi_open(const char *path, unsigned width, unsigned height, unsigned fps_num,
                       unsigned fps_den, unsigned aspect_num, unsigned aspect_den,
                       unsigned audio_rate, unsigned audio_channels)
{
    return avi_open_titled(path, width, height, fps_num, fps_den, aspect_num, aspect_den,
                           audio_rate, audio_channels, NULL);
}

int avi_write_video(avi_writer_t *w, const uint8_t *frame, size_t len)
{
    int r = write_chunk(w, 0, frame, len);
    if (r == 0) {
        w->frames++;
        if (w->segment == 0)
            w->first_riff_frames++;
    }
    return r;
}

int avi_write_audio(avi_writer_t *w, const uint8_t *pcm, size_t len)
{
    int r = write_chunk(w, 1, pcm, len);
    if (r == 0) {
        uint64_t n = len / (w->channels * 2);
        w->samples += n;
        w->seg_samples += n;
    }
    return r;
}

int avi_close(avi_writer_t *w)
{
    if (!w)
        return -1;
    FILE *f = w->f;
    close_segment(w);

    patch32(f, w->strh_len[0], (uint32_t)w->frames);
    patch32(f, w->strh_len[1], (uint32_t)w->samples);
    patch32(f, w->dmlh_total, (uint32_t)w->frames);
    for (int s = 0; s < NSTREAMS; s++) {
        off_t here = ftello(f);
        /* indx chunk: id, size, wLongsPerEntry(2) subtype(1) type(1), nEntriesInUse at +12 */
        fseeko(f, w->indx_at[s] + 12, SEEK_SET);
        put32(f, w->nsuper[s]);
        fseeko(f, w->indx_at[s] + 32, SEEK_SET);
        for (unsigned i = 0; i < w->nsuper[s]; i++) {
            put64(f, w->super[s][i].offset);
            put32(f, w->super[s][i].size);
            put32(f, w->super[s][i].duration);
        }
        fseeko(f, here, SEEK_SET);
    }
    int err = w->error || ferror(f);
    if (fclose(f) != 0)
        err = 1;
    for (int s = 0; s < NSTREAMS; s++)
        free(w->ix[s].e);
    free(w->idx1);
    free(w);
    return err ? -1 : 0;
}
