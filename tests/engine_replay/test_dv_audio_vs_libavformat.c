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
 * ctest: validates dv_audio.c's real SMPTE 314M/IEC 61834 deshuffle
 * sample-for-sample against FFmpeg's own "dv" demuxer, on real captures
 * from tests/data/ -- not synthetic data. This is the actual correctness
 * check for the shuffle/expansion dv_audio.h documents as "transcribed
 * from FFmpeg, validated against real captures"; tests/engine/test_dv_audio.c
 * only checks the algorithm is *wired up* correctly (synthetic data at
 * hand-picked shuffle-table positions).
 *
 * Method, per real DV frame in the file: run it through dv_audio_extract()
 * and, separately, through FFmpeg's "dv" demuxer opened on just that one
 * frame's bytes in memory (the exact technique src/sinks/sink_rewrap.c's
 * rewrap_dv_extract_audio() uses for the real output file, including its
 * documented quirk: demuxing one isolated DV frame at a time occasionally
 * yields no packet, or two packets covering roughly two frames' worth, for
 * a frame whose header doesn't match one of libavformat's known
 * camera/deck profiles -- see that function's comment). Frames where
 * FFmpeg's isolated-frame demux misbehaves that way are counted and
 * skipped rather than failed (that is a known artifact of probing one
 * frame at a time, not a disagreement about the shuffle); every frame
 * where it *didn't* misbehave must match dv_audio_extract()'s output byte
 * count exactly, and every sample value in it must match exactly. The
 * fraction of skipped frames is asserted small so this still validates
 * virtually the whole file.
 *
 * Traces are the developer's own recordings (tests/data/, .gitignore'd);
 * this test exits 0 doing nothing if none are present, like the other
 * trace-dependent tests (test_replay_hdv etc).
 *
 * Coverage today: NTSC only (see tests/data/README.md), 48 kHz/16-bit
 * (dv-ntsc.dv) and 32 kHz/12-bit nonlinear (dv-ntsc-32k.dv, which covers
 * dv_audio_12to16()). No PAL audio.
 */

#include "dv_audio.h"

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);    \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

typedef struct { const uint8_t *base; size_t size, pos; } mem_ctx_t;

static int mem_read(void *opaque, uint8_t *buf, int want)
{
    mem_ctx_t *m = opaque;
    size_t left = m->size - m->pos;
    if (!left)
        return AVERROR_EOF;
    size_t n = (size_t)want < left ? (size_t)want : left;
    memcpy(buf, m->base + m->pos, n);
    m->pos += n;
    return (int)n;
}

static int64_t mem_seek(void *opaque, int64_t offset, int whence)
{
    mem_ctx_t *m = opaque;
    int64_t base;
    if (whence == AVSEEK_SIZE)
        return (int64_t)m->size;
    if (whence == SEEK_SET) base = 0;
    else if (whence == SEEK_CUR) base = (int64_t)m->pos;
    else if (whence == SEEK_END) base = (int64_t)m->size;
    else return -1;
    int64_t np = base + offset;
    if (np < 0 || np > (int64_t)m->size)
        return -1;
    m->pos = (size_t)np;
    return np;
}

/* Demuxes one DV frame's bytes with FFmpeg's own "dv" demuxer (same
 * approach as sink_rewrap.c's rewrap_dv_extract_audio(), see that
 * function's comment for exactly why avformat_find_stream_info() is
 * deliberately not called). Returns the number of distinct packets seen on
 * the first ("pair 1") audio stream and their total byte count; the bytes
 * themselves are copied into *out (caller-owned, freed by the caller),
 * which is NULL if there was no audio stream at all. */
static int demux_one_frame_audio(const AVInputFormat *dv_fmt, const uint8_t *data, size_t len,
                                  uint8_t **out, size_t *out_len, int *out_npkts)
{
    *out = NULL;
    *out_len = 0;
    *out_npkts = 0;

    mem_ctx_t mem = { .base = data, .size = len, .pos = 0 };
    uint8_t *iobuf = av_malloc(4096);
    AVIOContext *avio = avio_alloc_context(iobuf, 4096, 0, &mem, mem_read, NULL, mem_seek);
    AVFormatContext *in = avformat_alloc_context();
    in->pb = avio;

    if (avformat_open_input(&in, NULL, dv_fmt, NULL) < 0) {
        avio_context_free(&avio);
        return 0; /* this isolated frame didn't even open -- treat as "no audio" */
    }

    AVPacket *bufpkt[16];
    int nbuf = 0;
    AVPacket *spare = av_packet_alloc();
    while (nbuf < 16 && av_read_frame(in, spare) >= 0) {
        bufpkt[nbuf++] = spare;
        spare = av_packet_alloc();
    }
    av_packet_free(&spare);

    int audio_idx = -1;
    for (unsigned i = 0; i < in->nb_streams; i++)
        if (in->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) { audio_idx = (int)i; break; }

    if (audio_idx >= 0) {
        size_t total = 0;
        for (int i = 0; i < nbuf; i++)
            if (bufpkt[i]->stream_index == audio_idx) { total += (size_t)bufpkt[i]->size; (*out_npkts)++; }
        if (total > 0) {
            uint8_t *buf = malloc(total);
            size_t off = 0;
            for (int i = 0; i < nbuf; i++)
                if (bufpkt[i]->stream_index == audio_idx) {
                    memcpy(buf + off, bufpkt[i]->data, (size_t)bufpkt[i]->size);
                    off += (size_t)bufpkt[i]->size;
                }
            *out = buf;
            *out_len = total;
        }
    }

    for (int i = 0; i < nbuf; i++)
        av_packet_free(&bufpkt[i]);
    avformat_close_input(&in);
    return 1;
}

typedef struct {
    const char *path;
    long max_frames; /* 0 = no cap */
    int strict;       /* apply the skip-rate threshold (0 for known-corrupt
                          files, e.g. 20260922-test_clean.dv, where a
                          higher artifact rate is expected and unrelated to
                          dv_audio.c's correctness -- the exact-match check
                          on whatever *is* comparable still always applies) */
} candidate_t;

static int check_file(const AVInputFormat *dv_fmt, const candidate_t *cand)
{
    FILE *f = fopen(cand->path, "rb");
    if (!f) {
        printf("SKIP %s: not present\n", cand->path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long total = ftell(f);
    fseek(f, 0, SEEK_SET);

    /* Every .dv file in tests/data/ is NTSC (see tests/data/README.md) --
     * unlike pin_session.c's replay_run() (which checks 144000/PAL first
     * and has no other way to tell), try 120000/NTSC first here since a
     * file size that happens to be divisible by both (e.g.
     * 20260922-clean-3s.dv: 90 NTSC frames, coincidentally also 75*144000)
     * must not be misread as PAL -- that misaligns every "frame" this test
     * reads and makes both extractors fail to find audio at all, which
     * would otherwise show up as a false 100%-skipped result. */
    long frame_size = 0;
    if (total % 120000 == 0) frame_size = 120000;
    else if (total % 144000 == 0) frame_size = 144000;
    if (!frame_size) {
        printf("SKIP %s: size %ld isn't a whole number of DV frames\n", cand->path, total);
        fclose(f);
        return 0;
    }

    uint8_t *frame = malloc((size_t)frame_size);
    long nframes = total / frame_size;
    if (cand->max_frames > 0 && nframes > cand->max_frames)
        nframes = cand->max_frames;

    long compared = 0, skipped_quirk = 0, byte_mismatches = 0, sample_mismatches = 0;
    long total_samples_checked = 0;

    for (long fi = 0; fi < nframes; fi++) {
        if (fread(frame, 1, (size_t)frame_size, f) != (size_t)frame_size)
            break;

        dv_audio_pcm_t pcm;
        int rc = dv_audio_extract(frame, (size_t)frame_size, &pcm);
        CHECK(rc == 0, "dv_audio_extract must not reject a real frame");
        if (rc != 0)
            continue;

        uint8_t *ref = NULL;
        size_t ref_len = 0;
        int npkts = 0;
        demux_one_frame_audio(dv_fmt, frame, (size_t)frame_size, &ref, &ref_len, &npkts);

        if (!pcm.valid) {
            /* No AAUX SOURCE pack this frame: FFmpeg's isolated demux
             * should agree there's no audio either. */
            if (ref_len != 0) {
                skipped_quirk++;
            } else {
                compared++;
            }
            free(ref);
            continue;
        }

        size_t expect_bytes = (size_t)pcm.pair1_samples * 4; /* 2ch, 2 bytes */
        if (npkts != 1 || ref_len != expect_bytes) {
            /* The documented isolated-per-frame-demux artifact (0 or 2
             * packets, or a byte count that doesn't match this one frame)
             * -- not something dv_audio_extract() can be judged against. */
            skipped_quirk++;
            free(ref);
            continue;
        }

        compared++;
        total_samples_checked += pcm.pair1_samples;
        if (memcmp(ref, pcm.pair1, expect_bytes) != 0) {
            byte_mismatches++;
            /* Count how many individual 16-bit samples actually differ,
             * for a more informative failure message. */
            const int16_t *r = (const int16_t *)ref;
            for (int i = 0; i < pcm.pair1_samples * 2; i++)
                if (r[i] != pcm.pair1[i])
                    sample_mismatches++;
            if (byte_mismatches <= 3)
                fprintf(stderr, "  mismatch in %s frame %ld: %ld/%d samples differ\n",
                        cand->path, fi, sample_mismatches, pcm.pair1_samples * 2);
        }
        free(ref);
    }
    fclose(f);
    free(frame);

    printf("%s: %ld frames, %ld compared, %ld skipped (demux artifact), "
           "%ld byte-count mismatches, %ld total samples checked\n",
           cand->path, nframes, compared, skipped_quirk, byte_mismatches, total_samples_checked);

    CHECK(compared > 0, "at least one frame was actually comparable");
    /* The isolated-per-frame demux artifact should be rare, not routine --
     * except on a file that's already known to be corrupt, where it's
     * simply not a meaningful signal either way. */
    if (cand->strict && compared + skipped_quirk > 0)
        CHECK(skipped_quirk * 10 <= (compared + skipped_quirk),
              "fewer than 10% of frames hit the isolated-frame demux artifact");
    CHECK(byte_mismatches == 0, "every comparable frame's audio matches FFmpeg's dv demuxer exactly");
    return 1;
}

int main(void)
{
    av_log_set_level(AV_LOG_QUIET); /* the per-frame isolated demux logs noise on purpose */

    const AVInputFormat *dv_fmt = av_find_input_format("dv");
    if (!dv_fmt) {
        fprintf(stderr, "test_dv_audio_vs_libavformat: \"dv\" demuxer not built in\n");
        return 1;
    }

    candidate_t candidates[] = {
        { "tests/data/dv-ntsc.dv", 0, 1 },
        { "tests/data/dv-ntsc-32k.dv", 0, 1 },
    };
    int any_present = 0;
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        FILE *probe = fopen(candidates[i].path, "rb");
        if (probe) { fclose(probe); any_present = 1; }
        check_file(dv_fmt, &candidates[i]);
    }

    if (!any_present) {
        printf("test_dv_audio_vs_libavformat: no tests/data/dv-ntsc.dv present, nothing to validate\n");
        return 0;
    }
    if (g_failures == 0) {
        printf("test_dv_audio_vs_libavformat: all tests passed\n");
        return 0;
    }
    printf("test_dv_audio_vs_libavformat: %d failure(s)\n", g_failures);
    return g_failures;
}
