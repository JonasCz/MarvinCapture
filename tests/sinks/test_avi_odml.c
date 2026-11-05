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
 * ctest: avi_writer's OpenDML layout over many RIFF segments (the analog AVI format). The
 * segment limit is set small for the test (avi_writer_set_segment_limit(); normally 1000 MiB),
 * so a few MB of small frames make a first RIFF with its idx1 and a dozen AVIX segments, each
 * with its standard index, all reached through the super index in the header. libavformat must
 * then find every frame from the indexes alone (before reading anything), and read back every
 * video frame in order and every audio sample; the headers' lengths count them all.
 *
 * Then the super index full: 1024 entries (1024 segments of 1000 MiB, ~1 TB, 14 h of analog
 * AVI; here of 48 KiB). The write that would need a 1025th segment fails, and the file holds
 * exactly the frames written before it, all indexed and counted, and closes without an error.
 */

#include "test_util.h"
#include "avi_writer.h"

#include <libavformat/avformat.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 48
#define FRAME_BYTES (W * H * 2)

static long count_tag(const unsigned char *buf, long n, const char *tag)
{
    long c = 0;
    for (long i = 0; i + 4 <= n; i++)
        if (memcmp(buf + i, tag, 4) == 0)
            c++;
    return c;
}

/* Where the k-th (0-based) chunk with this id starts, or -1. */
static long find_tag(const unsigned char *buf, long n, const char *tag, int k)
{
    for (long i = 0; i + 4 <= n; i++)
        if (memcmp(buf + i, tag, 4) == 0 && k-- == 0)
            return i;
    return -1;
}

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Writes up to max_frames numbered frames (and rate / 25 samples of audio each, every sample
 * the frame's number) in segments of at most `segment` bytes, until a write fails; closes the
 * file. Returns the frames written, video and audio. */
static long write_avi(const char *path, uint32_t segment, int rate, long max_frames)
{
    const int spf = rate / 25;
    avi_writer_set_segment_limit(segment);
    avi_writer_t *w = avi_open_titled(path, W, H, 25, 1, 4, 3, (unsigned)rate, 2, "OpenDML test");
    CHECK(w);
    uint8_t frame[FRAME_BYTES];
    int16_t *pcm = malloc((size_t)spf * 4);
    CHECK(pcm);
    long i;
    for (i = 0; i < max_frames; i++) {
        int num = (int)i;
        memset(frame, 0x80, sizeof(frame));
        memcpy(frame, &num, sizeof(num));   /* the frame's number, to check the order */
        for (int k = 0; k < spf * 2; k++)
            pcm[k] = (int16_t)i;
        if (avi_write_video(w, frame, sizeof(frame)) != 0)
            break;
        /* with the sizes used here a full file refuses a video frame, never its audio */
        CHECK(avi_write_audio(w, (const uint8_t *)pcm, (size_t)spf * 4) == 0);
    }
    free(pcm);
    CHECK(avi_close(w) == 0);
    avi_writer_set_segment_limit(0);
    return i;
}

/* The file's layout and what libavformat reads from it: `frames` numbered frames, their audio
 * at `rate`. Returns the number of RIFF segments. */
static long check_avi(const char *path, long frames, int rate)
{
    const int spf = rate / 25;
    /* one RIFF AVI with its idx1, then AVIX segments; the video super index in the header lists
     * one standard index per segment; the stream headers' dwLength (40 bytes into each strh)
     * count the frames and the samples */
    FILE *f = fopen(path, "rb");
    CHECK(f);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)n);
    CHECK(buf && fread(buf, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    long segments = 1 + count_tag(buf, n, "AVIX");
    CHECK_EQ_I(count_tag(buf, n, "idx1"), 1);
    CHECK_EQ_I(count_tag(buf, n, "ix00"), segments);
    long indx = find_tag(buf, n, "indx", 0);         /* the video stream's comes first */
    CHECK(indx > 0);
    CHECK_EQ_I(le32(buf + indx + 12), segments);      /* nEntriesInUse */
    long strh_v = find_tag(buf, n, "strh", 0), strh_a = find_tag(buf, n, "strh", 1);
    CHECK(strh_v > 0 && strh_a > strh_v);
    CHECK_EQ_I(le32(buf + strh_v + 40), frames);
    CHECK_EQ_I(le32(buf + strh_a + 40), frames * spf);
    free(buf);

    AVFormatContext *fc = NULL;
    CHECK(avformat_open_input(&fc, path, NULL, NULL) >= 0);
    CHECK(avformat_find_stream_info(fc, NULL) >= 0);
    int vidx = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    int aidx = av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    CHECK(vidx >= 0 && aidx >= 0);
    /* found through the indexes, every segment's, before a packet was read */
    CHECK_EQ_I(avformat_index_get_entries_count(fc->streams[vidx]), frames);
    CHECK_EQ_I(fc->streams[vidx]->nb_frames, frames);
    CHECK_EQ_I(fc->streams[aidx]->codecpar->sample_rate, rate);

    long vframes = 0;
    int64_t samples = 0;
    AVPacket *pkt = av_packet_alloc();
    while (av_read_frame(fc, pkt) >= 0) {
        if (pkt->stream_index == vidx) {
            CHECK_EQ_I(pkt->size, FRAME_BYTES);
            int num;
            memcpy(&num, pkt->data, sizeof(num));
            CHECK_EQ_I(num, vframes);   /* in order, none lost or doubled */
            vframes++;
        } else if (pkt->stream_index == aidx) {
            const int16_t *smp = (const int16_t *)pkt->data;
            for (int k = 0; k < pkt->size / 4; k++, samples++)
                CHECK_EQ_I(smp[2 * k], samples / spf);
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avformat_close_input(&fc);
    CHECK_EQ_I(vframes, frames);
    CHECK_EQ_I(samples, (int64_t)frames * spf);
    printf("OK: %s: %ld bytes, %ld RIFF segments, %ld video frames in order, %lld audio samples\n",
           path, n, segments, vframes, (long long)samples);
    return segments;
}

int main(void)
{
    /* 250 frames (10 s), 48 kHz, 256 KiB segments: about 18 frames each, 14 segments */
    CHECK_EQ_I(write_avi("test_avi_odml.avi", 256 * 1024, 48000, 250), 250);
    CHECK(check_avi("test_avi_odml.avi", 250, 48000) >= 10);
    remove("test_avi_odml.avi");

    /* the super index full: 1024 segments of 48 KiB (6 frames each at 8 kHz, ~45 MB) */
    long frames = write_avi("test_avi_full.avi", 48 * 1024, 8000, 10000);
    CHECK(frames > 1024 && frames < 10000);
    CHECK_EQ_I(check_avi("test_avi_full.avi", frames, 8000), 1024);
    remove("test_avi_full.avi");
    return 0;
}
