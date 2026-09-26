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
 * On-the-fly rewrap for DV (-> type-2 AVI or MOV) and a close-time remux
 * fallback for HDV (-> MOV or MKV).
 *
 * DV: each reassembled frame (dv_reassembler's whole-unit callback) is
 * muxed as one dvvideo packet -- the DIF bytes are DV's own bitstream, no
 * transcoding needed, exactly like piping raw .dv through `ffmpeg -f dv`.
 * codecpar (width/height/field_order/SAR/colour) is set once, from the
 * first frame's dv_subcode info (see dv_subcode.h) merged with the caller's
 * pin_sink_params_t.
 *
 * Audio: rather than hand-porting IEC 61834's DIF audio-block deshuffle
 * (dv_extract_audio in FFmpeg's libavformat/dv.c) into this project, each
 * frame's own bytes are re-opened through FFmpeg's *own* "dv" demuxer --
 * already an enabled build component (see third_party/README.md) -- via a
 * small in-memory, fully-buffered (therefore seekable) AVIOContext. That
 * demuxer already does the deshuffle (16-bit linear at 48/44.1 kHz, and
 * dv_audio_12to16 for the 32 kHz 12-bit nonlinear mode) and hands back
 * plain pcm_s16le packets, which are simply re-muxed into our output's
 * audio stream. This is the plan's suggested port, done by reusing the
 * reference implementation instead of retyping its lookup tables by hand --
 * it cannot disagree with the tables it *is*, which is exactly what
 * tests/sinks/dvaudio_check verifies isn't just circular: that check
 * decodes the *original* .dv file (not our rewrap output) with the same
 * demuxer and compares sample-for-sample against what ends up in our
 * output file, catching any mistake in the remux/repack step here even
 * though the deshuffle itself is shared code.
 *
 * A one-frame demux is not free (probe + parse per frame), but DV frame
 * rates are 25-30 Hz, nowhere near a bottleneck for this driver.
 *
 * Known limitation of demuxing one isolated frame at a time: FFmpeg's dv
 * demuxer matches the frame's DSF/STYPE header byte against a fixed table
 * of known camera/deck profiles to know how to decode its audio layout; a
 * frame whose header doesn't match any entry ("stype NN is invalid" in the
 * log) yields no audio stream for that one frame even though its video is
 * otherwise fine and gets written normally. This does happen occasionally
 * on real tapes (a handful of frames in the traces/ep88-*.bin samples this
 * project ships hit it) -- almost certainly genuine header noise on the
 * source tape rather than something this driver introduces, since
 * dv_reassembler already validated the frame's length. The output stays
 * correct where it can be: that frame's video plays, and dv_write_unit
 * pads its audio with silence so later audio stays in sync. See the risk
 * list for a capture-wide fallback if this turns out to matter in
 * practice.
 *
 * HDV: the "feed the mpegts demuxer live over a non-seekable queue" path
 * the plan describes as the preferred approach needs a look-ahead buffer
 * before the demuxer will report streams, which is fiddly to get right
 * under this task's time budget and, per the plan's own risk list, has a
 * documented fallback: write the raw TS to a temp file, then remux at
 * close(). That fallback is what's implemented here for HDV. The temp file
 * is a real, seekable file, so opening it with the mpegts demuxer needs no
 * special-casing; mpeg2video + mp2 are stream-copied into the target
 * container with codec_tag left to the muxer (mov_get_mpeg2_xdcam_codec_tag
 * picks the hdv2/hdv3 4CC from the stream's geometry/profile) and
 * field_order/SAR/colour set explicitly to match the plan.
 */

#include "pin_sink.h"
#include "sinks_internal.h"
#include "../engine/dv_subcode.h"
#include "../core/pin_log.h"

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ DV -- */

/* One muxed audio track's timeline state. DV's optional 32 kHz 12-bit
 * nonlinear mode can carry two independent stereo pairs per frame (SMPTE
 * 314M's "four channel" audio); pair 1 is always the main track (ch[0]),
 * pair 2 -- when present -- becomes a second audio stream (ch[1]), each
 * tracked and padded to the video timeline independently since either one
 * can individually fall behind (see rewrap_mux_audio_pair()). */
typedef struct {
    AVStream *ast;      /* NULL until this pair is first seen */
    int64_t apts;
} audio_track_t;

typedef struct {
    pin_format_t format;      /* PIN_FMT_DV_AVI or PIN_FMT_DV_MOV */
    pin_sink_params_t params;

    AVFormatContext *fmt;
    AVStream *vst;
    audio_track_t ch[2];       /* [0] = pair 1 (main), [1] = pair 2, if present */
    int header_written;
    int64_t vpts;

    pin_sink_status_t st;

    /* Temp path for HDV's raw-then-remux fallback. */
    char raw_path[1200];
    FILE *raw_f;
} rewrap_priv_t;

static int is_valid_dv_unit_len(size_t len)
{
    return len == (size_t)DV_SEQ_COUNT_NTSC * DV_SEQ_SIZE ||
           len == (size_t)DV_SEQ_COUNT_PAL * DV_SEQ_SIZE;
}

/* Minimal read-only, seekable in-memory AVIOContext, used to hand one DV
 * frame's bytes to the "dv" demuxer for audio extraction. */
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
    if (whence == SEEK_SET)
        base = 0;
    else if (whence == SEEK_CUR)
        base = (int64_t)m->pos;
    else if (whence == SEEK_END)
        base = (int64_t)m->size;
    else
        return -1;
    int64_t np = base + offset;
    if (np < 0 || np > (int64_t)m->size)
        return -1;
    m->pos = (size_t)np;
    return np;
}

/* Demuxes one DV frame's worth of bytes with FFmpeg's own "dv" demuxer and
 * re-muxes its audio into our output's audio stream(s) (created lazily,
 * one per pair -- see audio_track_t above).
 *
 * Every pair's contribution is capped to *exactly* this one video frame's
 * sample span on the output timeline -- computed as target_after(vpts+1)
 * minus the pair's own running apts, not a fixed "rate/fps" constant, so
 * drop-frame rates (29.97 etc.) never accumulate rounding drift. This
 * matters because the per-frame demux doesn't reliably hand back exactly
 * one frame's worth: a frame with header noise can yield none (see the
 * file's "Known limitation" comment), and -- observed on
 * traces/ep88-ntsc-sample.bin's 32 kHz footage -- an occasional frame
 * yields *two* packets on the same stream, together holding roughly twice
 * a normal frame's bytes, apparently a quirk of how libavformat/dv.c
 * segments that particular frame's AAUX data rather than this driver's
 * own bookkeeping (the input buffer handed to it is exactly one physical
 * DV frame, 120000/144000 bytes, so it cannot itself contain two frames'
 * worth of tape). Short: pad with silence, here, so the deficit never
 * carries forward. Long: truncate the excess, here, for the same reason.
 * Either way every pair's apts lands on the video timeline after every
 * single frame, which is what keeps A/V sync exact end to end regardless
 * of what any one frame's demux happens to return. */
static pin_status_t rewrap_dv_extract_audio(rewrap_priv_t *p, const uint8_t *data, size_t len)
{
    const AVInputFormat *dv_fmt = av_find_input_format("dv");
    if (!dv_fmt) {
        pin_logf(PIN_LOG_ERROR, "sink_rewrap: \"dv\" demuxer not built in\n");
        return PIN_ERR_CODEC;
    }

    mem_ctx_t mem = { .base = data, .size = len, .pos = 0 };
    uint8_t *iobuf = av_malloc(4096);
    AVIOContext *avio = avio_alloc_context(iobuf, 4096, 0, &mem, mem_read, NULL, mem_seek);
    AVFormatContext *in = avformat_alloc_context();
    in->pb = avio;

    pin_status_t result = PIN_OK;
    int rc = avformat_open_input(&in, NULL, dv_fmt, NULL);
    if (rc < 0) {
        pin_logf(PIN_LOG_WARN, "sink_rewrap: per-frame dv demux failed (%d), frame has no audio\n", rc);
        avio_context_free(&avio);
        return PIN_OK;   /* video still written; treat as an audio-less frame */
    }
    /* Deliberately *not* calling avformat_find_stream_info() here: on a
     * single buffered frame it was measured to read ahead into (and
     * consume) this same buffer's only audio packet before the read loop
     * below ever saw it, silently dropping that frame's audio.
     *
     * The tradeoff: the "dv" demuxer adds its AVStreams as it discovers
     * them while reading packets (audio is typically not known to exist
     * until the AAUX pack area of the DIF data is actually read), not
     * necessarily all up front in avformat_open_input()'s read_header. So
     * every packet is read into a small buffer *first*; only once nothing
     * more is available is in->nb_streams stable enough to look for an
     * audio stream. Cheap: a single DV frame demuxes to only a handful of
     * packets. */
    AVPacket *bufpkt[16];
    int nbuf = 0;
    AVPacket *spare = av_packet_alloc();
    while (nbuf < 16 && av_read_frame(in, spare) >= 0) {
        bufpkt[nbuf++] = spare;
        spare = av_packet_alloc();
    }
    av_packet_free(&spare);   /* the one alloc that never got a packet */

    /* Pair 1 is whichever audio stream comes first (lowest index -- always
     * index 1 when present, since index 0 is video); pair 2, if the 4-ch
     * 32 kHz mode is in use, is the next one. */
    int audio_idx[2] = { -1, -1 };
    int n_audio = 0;
    for (unsigned i = 0; i < in->nb_streams && n_audio < 2; i++)
        if (in->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            audio_idx[n_audio++] = (int)i;

    /* Pass 1: create every audio stream this frame reveals *before* the
     * header goes out. Both pairs (when the 4-ch mode is in use) are
     * always discovered together on whichever frame first has audio, so
     * writing the header as soon as pair 1's stream exists -- before pair
     * 2's loop iteration gets a chance to add its stream -- would commit a
     * 2-stream header to disk and then hand packets to a 3rd stream the
     * muxer never declared. Every AVFormatContext stream must exist before
     * avformat_write_header(); this loop guarantees that regardless of pair
     * count. */
    for (int pair = 0; pair < n_audio; pair++) {
        audio_track_t *tr = &p->ch[pair];
        if (tr->ast)
            continue;
        if (p->header_written) {
            /* A stream can only be added before avformat_write_header();
             * this would be a frame where audio is seen for the first time
             * after some earlier frame already forced the header out with
             * no audio declared (e.g. frame 0 had none). Rather than crash
             * muxing into an undeclared stream, that pair is dropped for
             * the rest of this file -- logged once. */
            pin_logf(PIN_LOG_WARN, "sink_rewrap: audio pair %d appeared after the header was "
                                    "already written (no audio on earlier frames); dropping "
                                    "it for the rest of this file\n", pair + 1);
            continue;
        }
        AVStream *src = in->streams[audio_idx[pair]];
        tr->ast = avformat_new_stream(p->fmt, NULL);
        tr->ast->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        tr->ast->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
        tr->ast->codecpar->sample_rate = src->codecpar->sample_rate;
        tr->ast->codecpar->format = AV_SAMPLE_FMT_S16;
        av_channel_layout_copy(&tr->ast->codecpar->ch_layout, &src->codecpar->ch_layout);
        tr->ast->codecpar->bits_per_coded_sample = 16;
        tr->ast->codecpar->block_align = (int)(tr->ast->codecpar->ch_layout.nb_channels * 2);
        tr->ast->time_base = (AVRational){ 1, src->codecpar->sample_rate };
        pin_logf(PIN_LOG_INFO, "sink_rewrap: DV audio pair %d: %d Hz, %d ch (from the dv "
                                "demuxer)%s\n", pair + 1, src->codecpar->sample_rate,
                src->codecpar->ch_layout.nb_channels,
                pair == 1 ? " -- 4-channel 32 kHz mode, second stereo pair" : "");
    }
    if (!p->header_written && n_audio > 0) {
        int hrc = avformat_write_header(p->fmt, NULL);
        if (hrc < 0) {
            pin_logf(PIN_LOG_ERROR, "sink_rewrap: avformat_write_header failed (%d)\n", hrc);
            result = PIN_ERR_CODEC;
        } else {
            p->header_written = 1;
        }
    }

    /* Pass 2: extract, cap-to-this-frame and pad each *already-established*
     * track's audio -- iterating over p->ch[] (every track this file has
     * ever declared), not over this frame's own n_audio. That distinction
     * is exactly the bug this fixes: a frame whose per-frame demux finds
     * *no* audio streams at all (audio_idx totally empty -- the demuxer
     * gives up, not just a short/absent packet for a stream it still
     * knows about) used to skip the pair loop entirely, so an
     * already-open track's apts silently fell behind the video timeline
     * with no padding -- and every later frame's audio in that track then
     * landed at the wrong offset in the output, a real A/V sync bug (seen
     * on traces/ep88-sample.bin PAL). Padding here for every established
     * track on every single frame, whether or not this frame's demux
     * mentions it, is what keeps that invariant unconditional. */
    for (int pair = 0; pair < 2 && p->header_written; pair++) {
        audio_track_t *tr = &p->ch[pair];
        if (!tr->ast)
            continue;   /* this track has never been seen; nothing to pad yet */
        int idx = pair < n_audio ? audio_idx[pair] : -1;

        int ch = tr->ast->codecpar->ch_layout.nb_channels;
        int rate = tr->ast->codecpar->sample_rate;

        size_t avail_bytes = 0;
        if (idx >= 0)
            for (int i = 0; i < nbuf; i++)
                if (bufpkt[i]->stream_index == idx)
                    avail_bytes += (size_t)bufpkt[i]->size;
        int64_t avail_samples = (int64_t)(avail_bytes / (2 * (size_t)ch));

        int64_t target_after = av_rescale(p->vpts + 1, (int64_t)rate * p->params.fps_den,
                                          p->params.fps_num);
        int64_t need = target_after - tr->apts;
        if (need < 0)
            need = 0;
        int64_t take = avail_samples < need ? avail_samples : need;
        int64_t pad = need - take;

        /* One packet per frame per track -- real bytes (if any) followed by
         * zero-fill (if any) in the same buffer -- rather than up to two
         * separate packets (a real one plus a tiny trailing silence one).
         * Two independent tiny packets, each individually valid, were
         * measured to come back from an AVI round-trip in the *wrong* byte
         * count when read back (more silence than was ever written, and
         * misaligned with the next frame's real audio) -- reproducible on
         * traces/ep88-ntsc-sample.bin's 32 kHz audio, where a 1-sample pad
         * packet immediately preceding a real packet on the same stream is
         * exactly the case that triggered it. Whether that's an
         * interleaving quirk in libavformat/avienc.c's buffering of very
         * small packets or something else wasn't root-caused further given
         * this task's time budget; avoiding ever emitting a standalone
         * sub-frame packet sidesteps it entirely and is more standard
         * practice besides (one packet per frame is what every other DV
         * audio path -- FFmpeg's own dv demuxer included -- does). */
        if (need > 0) {
            AVPacket *out_pkt = av_packet_alloc();
            if (out_pkt && av_new_packet(out_pkt, (int)(need * 2 * ch)) == 0) {
                size_t off = 0, want = (size_t)take * 2 * (size_t)ch;
                for (int i = 0; i < nbuf && off < want; i++) {
                    if (bufpkt[i]->stream_index != idx)
                        continue;
                    size_t n = (size_t)bufpkt[i]->size < want - off ? (size_t)bufpkt[i]->size
                                                                    : want - off;
                    memcpy(out_pkt->data + off, bufpkt[i]->data, n);
                    off += n;
                }
                if (pad > 0)
                    memset(out_pkt->data + off, 0, (size_t)pad * 2 * (size_t)ch);
                out_pkt->stream_index = tr->ast->index;
                out_pkt->pts = out_pkt->dts = tr->apts;
                out_pkt->duration = need;
                tr->apts += need;
                if (pad > 0)
                    p->st.audio_padded_samples += (uint64_t)pad;
                if (av_interleaved_write_frame(p->fmt, out_pkt) < 0)
                    result = PIN_ERR_IO;
                p->st.bytes_written += want;
            }
            av_packet_free(&out_pkt);
        }
    }

    for (int i = 0; i < nbuf; i++)
        av_packet_free(&bufpkt[i]);

    avformat_close_input(&in);
    return result;
}

static pin_status_t dv_open(pin_sink_t *s, const char *path, const pin_sink_params_t *params)
{
    rewrap_priv_t *p = s->priv;
    p->params = *params;
    const char *muxer = p->format == PIN_FMT_DV_AVI ? "avi" : "mov";

    int rc = avformat_alloc_output_context2(&p->fmt, NULL, muxer, path);
    if (rc < 0 || !p->fmt)
        return PIN_ERR_CODEC;

    p->vst = avformat_new_stream(p->fmt, NULL);
    if (!p->vst)
        return PIN_ERR_NOMEM;
    p->vst->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    p->vst->codecpar->codec_id = AV_CODEC_ID_DVVIDEO;
    p->vst->codecpar->width = params->width;
    p->vst->codecpar->height = params->height;
    p->vst->codecpar->format = AV_PIX_FMT_YUV420P; /* nominal; dvvideo carries its own DIF */
    p->vst->codecpar->field_order = AV_FIELD_BB;    /* DV: bottom field first */
    /* Derived from params->aspect + geometry here, not trusted from
     * params->sample_aspect_num/den as pre-computed upstream -- same
     * reasoning as sink_ffv1.c's ffv1_open(): a non-AUTO aspect override
     * must reach the file regardless of what any particular caller passed
     * in those two fields. is_pal from height: DV is always 720-wide, so
     * 576 lines is PAL, 480 is NTSC (matches dv_subcode.h's DSF bit, which
     * pin_sink_sar_for doesn't need here since geometry already implies it). */
    {
        int is_pal = params->height == 576;
        int sn, sd;
        pin_sink_sar_for(PIN_KIND_DV, is_pal, params->aspect, params->width, &sn, &sd);
        p->vst->codecpar->sample_aspect_ratio = (AVRational){ sn, sd };
        /* Both the avi and mov muxers read SAR from AVStream, not
         * AVCodecParameters (see libavformat/avienc.c mov/avi vprp/tkhd
         * code); codecpar's copy above is kept in sync for callers that
         * read it directly. */
        p->vst->sample_aspect_ratio = (AVRational){ sn, sd };
    }
    p->vst->codecpar->color_range = AVCOL_RANGE_MPEG;
    p->vst->codecpar->color_primaries =
        params->colour_matrix == PIN_MATRIX_BT709 ? AVCOL_PRI_BT709 : AVCOL_PRI_BT470BG;
    p->vst->codecpar->color_space =
        params->colour_matrix == PIN_MATRIX_BT709 ? AVCOL_SPC_BT709 : AVCOL_SPC_BT470BG;
    if (p->format == PIN_FMT_DV_AVI)
        p->vst->codecpar->codec_tag = MKTAG('d', 'v', 's', 'd');
    /* PIN_FMT_DV_MOV: codec_tag left 0 -- the mov muxer picks dvcp/dvc  from
     * codecpar (see libavformat/movenc.c mov_get_dv_codec_tag). */
    p->vst->time_base = (AVRational){ params->fps_den, params->fps_num };
    p->vst->avg_frame_rate = (AVRational){ params->fps_num, params->fps_den };

    if (params->title[0])
        av_dict_set(&p->fmt->metadata, "title", params->title, 0);

    rc = avio_open(&p->fmt->pb, path, AVIO_FLAG_WRITE);
    if (rc < 0)
        return PIN_ERR_IO;

    return PIN_OK;
}

static pin_status_t dv_write_unit(pin_sink_t *s, const uint8_t *data, size_t len)
{
    rewrap_priv_t *p = s->priv;
    if (!is_valid_dv_unit_len(len)) {
        p->st.units_damaged++;
        pin_logf(PIN_LOG_WARN, "sink_rewrap: dropping a %zu-byte DV unit (not 10/12 sequences)\n", len);
        return PIN_OK;
    }

    /* Audio must be discovered (or ruled absent) before the header is
     * written, so hold the header until frame 0 has been through the
     * per-frame audio demux above. */
    pin_status_t arc = rewrap_dv_extract_audio(p, data, len);

    if (!p->header_written) {
        int rc = avformat_write_header(p->fmt, NULL);
        if (rc < 0) {
            pin_logf(PIN_LOG_ERROR, "sink_rewrap: avformat_write_header failed (%d)\n", rc);
            return PIN_ERR_CODEC;
        }
        p->header_written = 1;
    }

    AVPacket *pkt = av_packet_alloc();
    av_new_packet(pkt, (int)len);
    memcpy(pkt->data, data, len);
    pkt->stream_index = p->vst->index;
    pkt->pts = pkt->dts = p->vpts++;
    pkt->flags |= AV_PKT_FLAG_KEY;
    int rc = av_interleaved_write_frame(p->fmt, pkt);
    av_packet_free(&pkt);
    if (rc < 0) {
        p->st.last_error = PIN_ERR_IO;
        return PIN_ERR_IO;
    }
    p->st.bytes_written += len;
    p->st.units_written++;

    /* Audio (both pairs, if present) was already extracted, exactly capped
     * and padded to this frame's span on the output timeline, inside
     * rewrap_dv_extract_audio() above -- see its header comment. */
    return arc;
}

static pin_status_t dv_close(pin_sink_t *s)
{
    rewrap_priv_t *p = s->priv;
    pin_status_t rc = PIN_OK;
    if (p->fmt) {
        if (!p->header_written) {
            /* No frame ever arrived (or all were damaged): still produce a
             * valid, empty container rather than a truncated file. */
            if (avformat_write_header(p->fmt, NULL) >= 0)
                p->header_written = 1;
        }
        if (p->header_written && av_write_trailer(p->fmt) < 0)
            rc = PIN_ERR_IO;
        if (p->fmt->pb)
            avio_closep(&p->fmt->pb);
        avformat_free_context(p->fmt);
    }
    if (p->st.last_error != PIN_OK)
        rc = p->st.last_error;
    free(p);
    free(s);
    return rc;
}

/* ----------------------------------------------------------------- HDV -- */

static pin_status_t hdv_open(pin_sink_t *s, const char *path, const pin_sink_params_t *params)
{
    rewrap_priv_t *p = s->priv;
    p->params = *params;
    snprintf(p->raw_path, sizeof(p->raw_path), "%s.rawts.tmp", path);
    p->raw_f = fopen(p->raw_path, "wb");
    if (!p->raw_f)
        return PIN_ERR_IO;
    setvbuf(p->raw_f, NULL, _IOFBF, 4 << 20);
    /* Final container path is remembered via s->priv (rewrap_priv_t doesn't
     * currently store it separately) -- reuse raw_path's sibling by
     * stripping the suffix at close time instead of a second buffer. */
    return PIN_OK;
}

static pin_status_t hdv_write_unit(pin_sink_t *s, const uint8_t *data, size_t len)
{
    rewrap_priv_t *p = s->priv;
    if (fwrite(data, 1, len, p->raw_f) != len) {
        p->st.last_error = PIN_ERR_IO;
        return PIN_ERR_IO;
    }
    p->st.bytes_written += len;
    p->st.units_written++;
    return PIN_OK;
}

/* Fallback remux, run once at close(): demux the temp .ts (a real, seekable
 * file -- no look-ahead concerns) and stream-copy video+audio into the
 * target MOV/MKV. */
static pin_status_t hdv_remux(rewrap_priv_t *p, const char *final_path)
{
    AVFormatContext *in = NULL;
    int rc = avformat_open_input(&in, p->raw_path, NULL, NULL);
    if (rc < 0) {
        pin_logf(PIN_LOG_ERROR, "sink_rewrap: opening the HDV temp TS failed (%d)\n", rc);
        return PIN_ERR_CODEC;
    }
    if (avformat_find_stream_info(in, NULL) < 0) {
        avformat_close_input(&in);
        return PIN_ERR_CODEC;
    }

    const char *muxer = p->format == PIN_FMT_HDV_MOV ? "mov" : "matroska";
    AVFormatContext *out = NULL;
    avformat_alloc_output_context2(&out, NULL, muxer, final_path);
    if (!out) {
        avformat_close_input(&in);
        return PIN_ERR_CODEC;
    }

    int map[8];
    memset(map, -1, sizeof(map));
    int vidx = -1;
    for (unsigned i = 0; i < in->nb_streams && i < 8; i++) {
        AVCodecParameters *cp = in->streams[i]->codecpar;
        if (cp->codec_type != AVMEDIA_TYPE_VIDEO && cp->codec_type != AVMEDIA_TYPE_AUDIO)
            continue;
        AVStream *o = avformat_new_stream(out, NULL);
        avcodec_parameters_copy(o->codecpar, cp);
        o->codecpar->codec_tag = 0;   /* let the target muxer choose (hdv3 etc.) */
        o->time_base = in->streams[i]->time_base;
        if (cp->codec_type == AVMEDIA_TYPE_VIDEO) {
            vidx = (int)i;
            o->codecpar->field_order = AV_FIELD_TT;
            o->codecpar->color_primaries = AVCOL_PRI_BT709;
            o->codecpar->color_trc = AVCOL_TRC_BT709;
            o->codecpar->color_space = AVCOL_SPC_BT709;
            o->codecpar->color_range = AVCOL_RANGE_MPEG;
            /* 1440x1080 HDV anamorphic: SAR so DAR comes out 4:3 (the usual
             * HDV1 raster) -- see pin_sink_sar_for's PIN_KIND_HDV branch. */
            int sn, sd;
            pin_sink_sar_for(PIN_KIND_HDV, 1, p->params.aspect, cp->width, &sn, &sd);
            o->codecpar->sample_aspect_ratio = (AVRational){ sn, sd };
            o->sample_aspect_ratio = (AVRational){ sn, sd };  /* see dv_open()'s comment */
        }
        map[i] = o->index;
    }
    if (vidx < 0) {
        avformat_free_context(out);
        avformat_close_input(&in);
        return PIN_ERR_CODEC;
    }

    if (p->params.title[0])
        av_dict_set(&out->metadata, "title", p->params.title, 0);

    rc = avio_open(&out->pb, final_path, AVIO_FLAG_WRITE);
    if (rc < 0) {
        avformat_free_context(out);
        avformat_close_input(&in);
        return PIN_ERR_IO;
    }
    if (avformat_write_header(out, NULL) < 0) {
        avio_closep(&out->pb);
        avformat_free_context(out);
        avformat_close_input(&in);
        return PIN_ERR_CODEC;
    }

    AVPacket *pkt = av_packet_alloc();
    pin_status_t result = PIN_OK;
    while (av_read_frame(in, pkt) >= 0) {
        if (pkt->stream_index < 8 && map[pkt->stream_index] >= 0) {
            AVStream *is = in->streams[pkt->stream_index];
            AVStream *os = out->streams[map[pkt->stream_index]];
            av_packet_rescale_ts(pkt, is->time_base, os->time_base);
            pkt->stream_index = map[pkt->stream_index];
            if (av_interleaved_write_frame(out, pkt) < 0)
                result = PIN_ERR_IO;
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    if (av_write_trailer(out) < 0)
        result = PIN_ERR_IO;
    avio_closep(&out->pb);
    avformat_free_context(out);
    avformat_close_input(&in);
    return result;
}

static pin_status_t hdv_close(pin_sink_t *s)
{
    rewrap_priv_t *p = s->priv;
    if (p->raw_f) {
        fclose(p->raw_f);
        p->raw_f = NULL;
    }

    /* Recover the final path: raw_path is "<final>.rawts.tmp". */
    char final_path[1200];
    size_t n = strlen(p->raw_path);
    size_t suffix = strlen(".rawts.tmp");
    pin_status_t rc = PIN_OK;
    if (n > suffix) {
        memcpy(final_path, p->raw_path, n - suffix);
        final_path[n - suffix] = '\0';
        if (p->st.units_written > 0)
            rc = hdv_remux(p, final_path);
        else
            pin_logf(PIN_LOG_WARN, "sink_rewrap: no HDV pictures captured, leaving no output file\n");
    } else {
        rc = PIN_ERR_INTERNAL;
    }
    remove(p->raw_path);
    if (p->st.last_error != PIN_OK)
        rc = p->st.last_error;
    free(p);
    free(s);
    return rc;
}

static void rewrap_get_status(pin_sink_t *s, pin_sink_status_t *out)
{
    rewrap_priv_t *p = s->priv;
    *out = p->st;
}

pin_sink_t *sink_rewrap_create(pin_format_t format)
{
    pin_sink_t *s = calloc(1, sizeof(*s));
    rewrap_priv_t *p = calloc(1, sizeof(*p));
    if (!s || !p) {
        free(s);
        free(p);
        return NULL;
    }
    p->format = format;
    s->priv = p;
    s->get_status = rewrap_get_status;

    if (format == PIN_FMT_DV_AVI || format == PIN_FMT_DV_MOV) {
        s->open = dv_open;
        s->write_unit = dv_write_unit;
        s->close = dv_close;
        s->supports_title = 1;
    } else if (format == PIN_FMT_HDV_MOV || format == PIN_FMT_HDV_MKV) {
        s->open = hdv_open;
        s->write_unit = hdv_write_unit;
        s->close = hdv_close;
        s->supports_title = 1;
    } else {
        free(p);
        free(s);
        return NULL;
    }
    return s;
}
