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
 * ctest: HDV scene split end to end through pin_api.h on the replay device.
 *
 * The tape is four ffmpeg-made pieces back to back (tests/make_fixtures.cmake):
 * a1 (01:00:00:00), x (one GOP with a stray timecode), a2 (a1's timecode
 * going on as if x had been right) and b (05:00:00:00, the next scene). The
 * pieces are joined at the TS packet level with the continuity counters
 * renumbered, so the GOP timecode is the only thing that jumps. Captured from
 * the start of the tape with --split, it must give:
 *   - exactly two files: the stray GOP does not split (a1, x, a2 in the first);
 *   - each file starts with PAT, PMT and then a picture that carries a sequence
 *     header and a GOP header; the second one's timecode is b's first;
 *   - the first file ends with the last picture before b (the cut is not late);
 *   - no picture lost or written twice: the video packets of the two files,
 *     one after the other, are the tape's, from the first captured picture to
 *     the end (the replay drops the picture still being read at the end of the
 *     file, as the reassembler drops a partial one when a capture stops);
 *   - both files decode with libavcodec, every picture, without errors.
 *
 * Usage: test_replay_hdv_split <a1.ts> <x.ts> <a2.ts> <b.ts>
 */

#include "replay_test_util.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#define TS 188

typedef struct {
    uint8_t *data;
    size_t n; /* packets */
} ts_buf_t;

static ts_buf_t read_ts(const char *path)
{
    ts_buf_t b = { NULL, 0 };
    FILE *f = fopen(path, "rb");
    CHECK(f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    CHECK(size > 0 && size % TS == 0);
    b.data = malloc((size_t)size);
    CHECK(b.data);
    CHECK(fread(b.data, 1, (size_t)size, f) == (size_t)size);
    fclose(f);
    b.n = (size_t)size / TS;
    return b;
}

static int pkt_pid(const uint8_t *p) { return ((p[1] & 0x1F) << 8) | p[2]; }
static int pkt_pusi(const uint8_t *p) { return (p[1] & 0x40) != 0; }

static const uint8_t *pkt_payload(const uint8_t *p, size_t *len)
{
    int afc = (p[3] >> 4) & 3;
    size_t off = 4;
    if (afc & 2)
        off += 1u + p[4];
    if (!(afc & 1) || off >= TS) {
        *len = 0;
        return p + TS;
    }
    *len = TS - off;
    return p + off;
}

/* a packet that starts a video PES (stream_id 0xE0..0xEF): a picture */
static int video_start(const uint8_t *p)
{
    size_t len;
    const uint8_t *pl = pkt_payload(p, &len);
    return pkt_pusi(p) && len >= 4 && pl[0] == 0 && pl[1] == 0 && pl[2] == 1 && (pl[3] & 0xF0) == 0xE0;
}

/* The video elementary stream bytes of the picture that starts at packet i (up to max). */
static size_t picture_es(const ts_buf_t *b, size_t i, int vpid, uint8_t *es, size_t max)
{
    size_t n = 0;
    for (size_t k = i; k < b->n; k++) {
        const uint8_t *p = b->data + k * TS;
        if (pkt_pid(p) != vpid)
            continue;
        if (k > i && pkt_pusi(p))
            break;
        size_t len;
        const uint8_t *pl = pkt_payload(p, &len);
        if (k == i && len >= 9) { /* skip the PES header */
            size_t hl = 9u + pl[8];
            pl += hl < len ? hl : len;
            len -= hl < len ? hl : len;
        }
        if (n + len > max)
            len = max - n;
        memcpy(es + n, pl, len);
        n += len;
        if (n == max)
            break;
    }
    return n;
}

/* Sequence header and GOP timecode ("hh:mm:ss:ff", "" if none) of the picture at packet i. */
static void picture_info(const ts_buf_t *b, size_t i, int vpid, int *seq, char tc[16])
{
    uint8_t es[4096];
    size_t n = picture_es(b, i, vpid, es, sizeof(es));
    *seq = 0;
    tc[0] = 0;
    for (size_t k = 0; k + 8 <= n; k++) {
        if (es[k] || es[k + 1] || es[k + 2] != 1)
            continue;
        if (es[k + 3] == 0xB3)
            *seq = 1;
        if (es[k + 3] == 0xB8 && !tc[0]) {
            uint32_t v = ((uint32_t)es[k + 4] << 24) | ((uint32_t)es[k + 5] << 16) |
                         ((uint32_t)es[k + 6] << 8) | es[k + 7];
            snprintf(tc, 16, "%02u:%02u:%02u:%02u", (v >> 26) & 31, (v >> 20) & 63, (v >> 13) & 63,
                     (v >> 7) & 63);
        }
    }
}

static int find_video_pid(const ts_buf_t *b)
{
    for (size_t i = 0; i < b->n; i++)
        if (video_start(b->data + i * TS))
            return pkt_pid(b->data + i * TS);
    return -1;
}

/* The packets of one PID, in order (pointers into b). */
static size_t pid_packets(const ts_buf_t *b, int pid, const uint8_t ***out)
{
    *out = malloc((b->n + 1) * sizeof(**out));
    CHECK(*out);
    size_t n = 0;
    for (size_t i = 0; i < b->n; i++)
        if (pkt_pid(b->data + i * TS) == pid)
            (*out)[n++] = b->data + i * TS;
    return n;
}

static size_t count_pictures(const ts_buf_t *b, int vpid)
{
    size_t n = 0;
    for (size_t i = 0; i < b->n; i++)
        if (pkt_pid(b->data + i * TS) == vpid && video_start(b->data + i * TS))
            n++;
    return n;
}

/* The pieces back to back, the continuity counter of each PID running on across the joins. */
static ts_buf_t join(char **paths, int count, size_t *pictures, int *vpid)
{
    ts_buf_t parts[8], all = { NULL, 0 };
    CHECK(count <= 8);
    for (int i = 0; i < count; i++) {
        parts[i] = read_ts(paths[i]);
        all.n += parts[i].n;
    }
    all.data = malloc(all.n * TS);
    CHECK(all.data);
    *vpid = find_video_pid(&parts[0]);
    CHECK(*vpid > 0);
    int last_cc[8192];
    for (int i = 0; i < 8192; i++)
        last_cc[i] = -1;
    size_t at = 0;
    for (int i = 0; i < count; i++) {
        pictures[i] = count_pictures(&parts[i], *vpid);
        for (size_t k = 0; k < parts[i].n; k++, at++) {
            uint8_t *p = all.data + at * TS;
            memcpy(p, parts[i].data + k * TS, TS);
            int pid = pkt_pid(p);
            if (pid == 0x1FFF)
                continue;
            if (last_cc[pid] >= 0)
                p[3] = (uint8_t)((p[3] & 0xF0) | ((last_cc[pid] + ((p[3] & 0x10) ? 1 : 0)) & 15));
            last_cc[pid] = p[3] & 15;
        }
        free(parts[i].data);
    }
    return all;
}

/* PAT, PMT (the PAT's PMT PID), then a picture with a sequence header and a GOP header. */
static void check_file_start(const ts_buf_t *b, int vpid, char tc[16])
{
    CHECK(b->n > 3);
    const uint8_t *pat = b->data, *pmt = b->data + TS, *pic = b->data + 2 * TS;
    CHECK(pkt_pid(pat) == 0 && pkt_pusi(pat));
    size_t len;
    const uint8_t *pl = pkt_payload(pat, &len);
    const uint8_t *sec = pl + 1 + pl[0];
    CHECK(sec[0] == 0x00);                              /* table_id: PAT */
    int pmt_pid = ((sec[10] & 0x1F) << 8) | sec[11];    /* first program's PMT PID */
    CHECK(pkt_pid(pmt) == pmt_pid && pkt_pusi(pmt));
    pl = pkt_payload(pmt, &len);
    CHECK(pl[1 + pl[0]] == 0x02);                       /* table_id: PMT */
    CHECK(pkt_pid(pic) == vpid && video_start(pic));
    int seq;
    picture_info(b, 2, vpid, &seq, tc);
    CHECK(seq);
    CHECK(tc[0]);
}

/* Decodes every video packet of the file; returns the pictures decoded. */
static int decode_all(const char *path)
{
    AVFormatContext *fc = NULL;
    CHECK(avformat_open_input(&fc, path, NULL, NULL) >= 0);
    CHECK(avformat_find_stream_info(fc, NULL) >= 0);
    int vidx = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    CHECK(vidx >= 0);
    const AVCodec *codec = avcodec_find_decoder(fc->streams[vidx]->codecpar->codec_id);
    CHECK(codec);
    AVCodecContext *cc = avcodec_alloc_context3(codec);
    CHECK(cc);
    CHECK(avcodec_parameters_to_context(cc, fc->streams[vidx]->codecpar) >= 0);
    cc->err_recognition = AV_EF_CRCCHECK | AV_EF_BITSTREAM | AV_EF_EXPLODE;
    CHECK(avcodec_open2(cc, codec, NULL) >= 0);
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    int frames = 0, eof = 0;
    while (!eof) {
        int rd = av_read_frame(fc, pkt);
        if (rd < 0)
            eof = 1;
        else if (pkt->stream_index != vidx) {
            av_packet_unref(pkt);
            continue;
        } else
            CHECK(!(pkt->flags & AV_PKT_FLAG_CORRUPT));
        CHECK(avcodec_send_packet(cc, eof ? NULL : pkt) >= 0);
        av_packet_unref(pkt);
        int r;
        while ((r = avcodec_receive_frame(cc, frame)) >= 0) {
            CHECK(frame->decode_error_flags == 0);
            frames++;
            av_frame_unref(frame);
        }
        CHECK(r == AVERROR(EAGAIN) || r == AVERROR_EOF);
    }
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&cc);
    avformat_close_input(&fc);
    return frames;
}

int main(int argc, char **argv)
{
    CHECK(argc == 5);
    for (int i = 1; i < 5; i++)
        pin_test_require_fixture(argv[i]);

    size_t pieces[4];
    int vpid;
    ts_buf_t src = join(argv + 1, 4, pieces, &vpid);
    const char *src_path = "hdv_split_tape.ts";
    FILE *f = fopen(src_path, "wb");
    CHECK(f && fwrite(src.data, TS, src.n, f) == src.n);
    fclose(f);
    size_t src_pics = pieces[0] + pieces[1] + pieces[2] + pieces[3];
    size_t b_first = pieces[0] + pieces[1] + pieces[2]; /* b's first picture, 0-based */
    printf("tape: %zu pictures (a1 %zu, x %zu, a2 %zu, b %zu), video PID 0x%x\n", src_pics,
           pieces[0], pieces[1], pieces[2], pieces[3], vpid);

    /* the source's video packets, and where each picture starts among them */
    const uint8_t **sv;
    size_t sv_n = pid_packets(&src, vpid, &sv);
    size_t *pic_at = malloc((src_pics + 1) * sizeof(*pic_at));
    CHECK(pic_at);
    size_t np = 0;
    for (size_t k = 0; k < sv_n; k++)
        if (video_start(sv[k]))
            pic_at[np++] = k;
    CHECK(np == src_pics);
    pic_at[np] = sv_n;

    /* Play the tape from its start: wind back (the replay winds in 2 s), then PLAY and
     * capture at once (the replay sleeps while stopped, so it sees both before reading on). */
    pin_session_t *s = pin_test_open_replay(src_path);
    pin_test_prepare_dv(s);
    pin_status_snapshot_t snap;
    int is_hdv = 0;
    for (int i = 0; i < 100 && !is_hdv; i++) {
        pin_get_status(s, &snap);
        is_hdv = snap.stream_kind == PIN_KIND_HDV;
        if (!is_hdv)
            pin_test_sleep_ms(50);
    }
    CHECK(is_hdv);
    CHECK(pin_deck(s, PIN_DECK_CMD_REW) == PIN_OK);
    int stopped = 0;
    for (int i = 0; i < 300 && !stopped; i++) {
        pin_test_sleep_ms(20);
        pin_get_status(s, &snap);
        stopped = snap.deck == PIN_DECK_STOPPED;
    }
    CHECK(stopped);

    pin_capture_opts_t opts;
    pin_capture_opts_defaults(&opts);
    strncpy(opts.path, "hdv_split", sizeof(opts.path) - 1);
    opts.format_hdv = PIN_FMT_HDV_TS;
    opts.scene_split = 1;
    CHECK(pin_deck(s, PIN_DECK_CMD_PLAY) == PIN_OK);
    CHECK(pin_capture_start(s, &opts, 1) == PIN_OK);
    pin_test_wait_state(s, PIN_STATE_CAPTURING, PIN_STATE_ERROR, 3000, &snap);
    CHECK(snap.state == PIN_STATE_CAPTURING);
    /* the capture ends by itself at the end of the tape (one pass) */
    pin_test_wait_state(s, PIN_STATE_READY, PIN_STATE_ERROR, 20000, &snap);
    CHECK(snap.state == PIN_STATE_READY);
    CHECK_EQ_I(snap.stop_reason, PIN_STOP_END_OF_TAPE);
    printf("captured: %llu picture(s), %llu with errors\n", (unsigned long long)snap.frames,
           (unsigned long long)snap.frames_error);
    CHECK(snap.frames_error == 0);
    pin_close(s);

    /* exactly two files */
    CHECK(pin_test_file_exists("hdv_split-0001.m2t"));
    CHECK(pin_test_file_exists("hdv_split-0002.m2t"));
    CHECK(!pin_test_file_exists("hdv_split-0003.m2t"));
    ts_buf_t f1 = read_ts("hdv_split-0001.m2t"), f2 = read_ts("hdv_split-0002.m2t");
    size_t f1_pics = count_pictures(&f1, vpid), f2_pics = count_pictures(&f2, vpid);
    printf("files: %zu + %zu pictures\n", f1_pics, f2_pics);

    /* each starts with PAT, PMT and a GOP; the second with b's timecode */
    char tc1[16], tc2[16], tc_b[16];
    int seq;
    check_file_start(&f1, vpid, tc1);
    check_file_start(&f2, vpid, tc2);
    picture_info(&src, (size_t)(sv[pic_at[b_first]] - src.data) / TS, vpid, &seq, tc_b);
    printf("first file starts at %s, second at %s (b starts at %s)\n", tc1, tc2, tc_b);
    CHECK(seq && strcmp(tc2, tc_b) == 0);
    CHECK(strcmp(tc_b, "05:00:00:00") == 0);

    /* No loss, no duplicate: file 1's video packets, then file 2's, are the tape's from the
     * first captured picture on, and the cut is at b's first picture. */
    const uint8_t **v1, **v2;
    size_t v1_n = pid_packets(&f1, vpid, &v1), v2_n = pid_packets(&f2, vpid, &v2);
    CHECK(v1_n > 0 && v2_n > 0);
    size_t first = src_pics;
    for (size_t p = 0; p < src_pics; p++)
        if (!memcmp(sv[pic_at[p]], v1[0], TS)) { first = p; break; }
    CHECK(first < b_first);
    printf("capture began at picture %zu of the tape\n", first);
    CHECK(pic_at[b_first] - pic_at[first] == v1_n); /* file 1 ends right before b */
    CHECK(f1_pics == b_first - first);
    for (size_t k = 0; k < v1_n; k++)
        CHECK(!memcmp(v1[k], sv[pic_at[first] + k], TS));
    for (size_t k = 0; k < v2_n; k++)
        CHECK(pic_at[b_first] + k < sv_n && !memcmp(v2[k], sv[pic_at[b_first] + k], TS));
    /* the second file runs to the end of the tape (but the picture read at the end of the file) */
    size_t end = pic_at[b_first] + v2_n;
    CHECK(end == pic_at[src_pics - 1] || end == sv_n);
    CHECK(f1_pics + f2_pics == src_pics - first - (end == sv_n ? 0 : 1));
    printf("OK: %zu + %zu pictures = the tape's %zu..%zu, each once, cut at b's first picture\n",
           f1_pics, f2_pics, first, first + f1_pics + f2_pics - 1);

    /* both decode, every picture */
    int d1 = decode_all("hdv_split-0001.m2t"), d2 = decode_all("hdv_split-0002.m2t");
    printf("decoded: %d + %d pictures\n", d1, d2);
    CHECK((size_t)d1 == f1_pics);
    CHECK((size_t)d2 == f2_pics);
    printf("OK: HDV scene split at the timecode jump, not at the stray GOP\n");

    free(v1);
    free(v2);
    free(sv);
    free(pic_at);
    free(f1.data);
    free(f2.data);
    free(src.data);
    return 0;
}
