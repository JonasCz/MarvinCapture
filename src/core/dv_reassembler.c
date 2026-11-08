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

#include "dv_reassembler.h"

#include <stdlib.h>
#include <string.h>

/* Convenience sink: writes each whole unit to a FILE*, same as every write()
 * call this file used to make directly before the callback refactor. */
static int file_write(const uint8_t *data, size_t len, void *user)
{
    FILE *f = user;
    return fwrite(data, 1, len, f) == len ? 0 : -1;
}

void dv_output_file(dv_output_t *out, FILE *fp)
{
    out->write = file_write;
    out->on_unit = NULL;
    out->on_loss = NULL;
    out->user = fp;
}

#define DIF_SEQUENCE_BYTES (150 * 80) /* 12,000 */
#define DIF_BLOCK_BYTES 80
#define NTSC_SEQ_COUNT 10
#define PAL_SEQ_COUNT 12

/* Give up resyncing and drop the oldest bytes if we go this long without
 * finding a valid header — protects against unbounded growth if the stream
 * is garbage (e.g. device not actually initialised). */
#define MAX_RESYNC_WINDOW (DIF_SEQUENCE_BYTES * 4)

/* Type-9 message framing (layer 1). */
#define MSG_TYPE_DATA 9
#define RING_ADDR_LO 0x7000u
#define RING_ADDR_HI 0x10000u

/* OHCI isochronous receive (layer 2). */
#define ISO_TCODE 0xA
#define ISO_TAG_CIP 1
#define CIP_HEADER_BYTES 8
#define ISO_TRAILER_BYTES 4
#define ISO_MAX_DLEN 1024

/* HDV / MPEG2-TS over IEC 61883-4. */
#define CIP_FMT_DV 0x00
#define CIP_FMT_MPEG2TS 0x20
#define HDV_DBS_QUADLETS 6            /* 24-byte data blocks, 8 per source packet */
#define HDV_SOURCE_PACKET_BYTES 192   /* 4-byte timestamp + 188-byte TS packet */
#define HDV_SPH_BYTES 4
#define TS_PACKET_BYTES 188
/* HDV analogue of DV's "until the first frame starts": a discontinuity in the
 * first ~60 ms (1000 TS packets at 25 Mbit/s) is the join into the already
 * running ring, not loss. */
#define HDV_JOIN_TS_PACKETS 1000

/* ------------------------------------------------------------------ */
/* Layer 3: DIF sequences -> frame-aligned output                      */
/* ------------------------------------------------------------------ */

static int is_dif_header(const uint8_t *p, unsigned *dseq_out)
{
    if (p[0] != 0x1f || p[2] != 0x00)
        return 0;
    if ((p[1] & 0x0f) != 0x07)
        return 0;
    *dseq_out = (p[1] >> 4) & 0x0f;
    return 1;
}

static size_t find_header(const uint8_t *buf, size_t len, size_t from, unsigned *dseq_out)
{
    if (len < 3)
        return (size_t)-1;
    for (size_t i = from; i + 3 <= len; i++) {
        if (is_dif_header(buf + i, dseq_out))
            return i;
    }
    return (size_t)-1;
}

/* Emits the completed frame in r->frame_buf to the output callbacks.
 * r->expected_dseq is the sequence count: it starts at 0, increments once
 * per sequence written (real or zero-padded), and the caller's completion
 * loop has already brought it up to at least the frame's size (its own
 * standard, else the confirmed one) -- so it's equal to that normally, or
 * (matching the pre-refactor file-sequential code, which had no ceiling of
 * its own) larger, on a capture where a bogus dseq briefly ran past the end
 * of the frame. */
static int emit_frame(dv_reassembler_t *r)
{
    size_t len = (size_t)r->expected_dseq * DIF_SEQUENCE_BYTES;
    if (r->out.write && r->out.write(r->frame_buf, len, r->out.user) != 0)
        return -1;
    if (r->out.on_unit)
        r->out.on_unit(DV_FORMAT_DV, r->frame_buf, len, r->out.user);
    return 0;
}

/* The frame in progress says by itself which standard it is: sequence 10 or
 * 11 arrived (only PAL has them), or its sequence header blocks' DSF bits
 * (byte 3, bit 7: 1 = 625/50) agree clearly. 0 = no such evidence. */
static unsigned frame_own_seq_count(const dv_reassembler_t *r)
{
    if (r->expected_dseq > NTSC_SEQ_COUNT)
        return PAL_SEQ_COUNT;
    unsigned pal = r->frame_dsf_pal, ntsc = r->frame_dsf_ntsc;
    if (pal >= DV_DSF_MIN_VOTES && pal > 2 * ntsc)
        return PAL_SEQ_COUNT;
    if (ntsc >= DV_DSF_MIN_VOTES && ntsc > 2 * pal)
        return NTSC_SEQ_COUNT;
    return 0;
}

/* After a frame: the confirmed standard follows DV_STD_SWITCH_FRAMES frames
 * in a row whose own evidence says the other one. */
static void std_follow(dv_reassembler_t *r, unsigned own)
{
    if (own && !r->std_known) {
        /* the first frame that says: that is the standard, no change */
        r->std_known = 1;
        r->system_seq_count = own;
    }
    if (!own || own == r->system_seq_count) {
        r->std_cand_frames = 0;
        return;
    }
    if (++r->std_cand_frames >= DV_STD_SWITCH_FRAMES) {
        r->system_seq_count = own;
        r->std_cand_frames = 0;
        r->std_switches++;
    }
}

/* Ends the DV frame in progress at the end of the stream or of DV (a switch
 * to HDV): emitted if it is complete, else dropped (it was never handed to
 * the output callbacks) with the counters put back. */
static int dv_end_frame(dv_reassembler_t *r)
{
    int rc = 0;
    if (r->frame_started) {
        unsigned own = frame_own_seq_count(r);
        if (r->expected_dseq < (own ? own : r->system_seq_count)) {
            /* Capture stopped mid-frame. Zero-padding it out would end every
             * output with a frame that is part black, so drop the partial
             * frame instead and leave the output a whole number of good
             * frames. */
            r->sequences_written = r->frame_start_written;
            r->sequences_dropped = r->frame_start_dropped;
        } else {
            /* Fully received, but not yet flushed -- that only happens when
             * a later frame's dseq 0 arrives, and the capture stopped first. */
            rc = emit_frame(r);
            r->frames_written++;
            std_follow(r, own);
        }
        r->frame_started = 0;
    }
    return rc;
}

/* Sequences in the frame in progress: by its own evidence, else the confirmed standard. */
static unsigned frame_seq_total(const dv_reassembler_t *r)
{
    unsigned own = frame_own_seq_count(r);
    return own ? own : r->system_seq_count;
}

/* Ends the frame in progress: padded with empty sequences for the ones that did not
 * arrive, then emitted. */
static int frame_finish(dv_reassembler_t *r)
{
    unsigned own = frame_own_seq_count(r);
    unsigned n = own ? own : r->system_seq_count;
    while (r->expected_dseq < n) {
        r->sequences_dropped++;
        r->expected_dseq++;
    }
    if (emit_frame(r) != 0)
        return -1;
    r->frames_written++;
    std_follow(r, own);
    return 0;
}

/* Starts a frame with the sequence at p, which is number `dseq` of it; the ones before it
 * were lost and stay empty. */
static void frame_begin(dv_reassembler_t *r, const uint8_t *p, unsigned dseq)
{
    r->frame_started = 1;
    r->frame_start_written = r->sequences_written;
    r->frame_start_dropped = r->sequences_dropped;
    r->frame_dsf_pal = r->frame_dsf_ntsc = 0;
    memset(r->frame_buf, 0, sizeof(r->frame_buf));
    r->sequences_dropped += dseq;
    memcpy(r->frame_buf + (size_t)dseq * DIF_SEQUENCE_BYTES, p, DIF_SEQUENCE_BYTES);
    r->sequences_written++;
    r->expected_dseq = dseq + 1;
    if (p[3] & 0x80) r->frame_dsf_pal++; else r->frame_dsf_ntsc++;
}

/* How many frame boundaries lie between the last sequence placed and the arriving one
 * (number `dseq`), and how many whole frames in between never arrived (*lost). The DBC
 * says how many sequences vanished (25 blocks each, known modulo 256, and 25 is invertible
 * there), and the vanished ones are j * n + dseq - last - 1 for j boundaries of n-sequence
 * frames; if that fits, j is known, and so is the loss, however many frames it was. Without
 * a fitting gap: a dseq 0 is a new frame, a later dseq is the same frame (the ones between
 * lost), and a dseq that goes back is a new frame whose start was lost if a gap happened, else
 * a stray match (-1: ignore it). */
static int frames_between(const dv_reassembler_t *r, unsigned dseq, unsigned n, unsigned *lost)
{
    unsigned last = r->expected_dseq - 1;
    *lost = 0;
    if (r->dbc_gap_pending) {
        int t = (int)((r->dbc_pending * 41u) & 0xFF); /* sequences lost */
        int x = t - ((int)dseq - (int)last - 1);
        if (x >= 0 && x % (int)n == 0) {
            unsigned j = (unsigned)(x / (int)n);
            *lost = j ? j - 1 : 0;
            return (int)j;
        }
        if (dseq <= last)
            return 1;
    }
    if (dseq == 0)
        return 1;
    return dseq > last ? 0 : -1;
}

static int place_sequence(dv_reassembler_t *r, const uint8_t *p, unsigned dseq)
{
    if (!r->frame_started) {
        /* Haven't seen sequence 0 yet; ignore until we do, so the output
         * always starts frame-aligned. */
        if (dseq == 0)
            frame_begin(r, p, 0);
        return 0;
    }

    unsigned lost;
    int boundaries = frames_between(r, dseq, frame_seq_total(r), &lost);
    if (boundaries < 0)
        return 0; /* stale/duplicate match; ignore rather than rewind */

    if (boundaries > 0) {
        /* A frame ends here. A frame that lost its first sequence(s) starts with them
         * empty (counted as an error frame), frames lost whole are only counted. */
        if (frame_finish(r) != 0)
            return -1;
        if (lost) {
            r->frames_lost += lost;
            if (r->out.on_loss)
                r->out.on_loss(0, lost, r->out.user);
        }
        frame_begin(r, p, dseq);
        return 0;
    }

    while (r->expected_dseq < dseq) {
        r->sequences_dropped++;
        r->expected_dseq++;
    }
    memcpy(r->frame_buf + (size_t)dseq * DIF_SEQUENCE_BYTES, p, DIF_SEQUENCE_BYTES);
    r->sequences_written++;
    r->expected_dseq = dseq + 1;
    if (p[3] & 0x80) r->frame_dsf_pal++; else r->frame_dsf_ntsc++;
    return 0;
}

/* Handles one located DIF sequence at buf+p with the given dseq. Advances
 * frame/sequence bookkeeping and copies real data into the in-progress
 * frame buffer; a frame is only handed to the output callbacks once it's
 * complete (the next frame's first sequence arrived, or dv_reassembler_finish
 * at end of capture). */
static int handle_sequence(dv_reassembler_t *r, const uint8_t *p, unsigned dseq)
{
    int rc = place_sequence(r, p, dseq);
    r->dbc_pending = 0;
    r->dbc_gap_pending = 0;
    return rc;
}

/* Feeds recovered DIF bytes (layers 1+2 already stripped) into sequence
 * assembly. The stream is contiguous, so the header search normally hits
 * immediately at offset 0 of each 12,000-byte step; it stays a search only as
 * a resync safety net. */
static int dif_feed(dv_reassembler_t *r, const uint8_t *data, size_t len)
{
    r->dif_bytes += len;

    if (r->buf_len + len > r->buf_cap) {
        size_t new_cap = r->buf_len + len;
        uint8_t *nb = realloc(r->buf, new_cap);
        if (!nb)
            return -1;
        r->buf = nb;
        r->buf_cap = new_cap;
    }
    memcpy(r->buf + r->buf_len, data, len);
    r->buf_len += len;

    size_t consumed = 0;
    for (;;) {
        unsigned dseq = 0;
        size_t p = find_header(r->buf, r->buf_len, consumed, &dseq);
        if (p == (size_t)-1) {
            if (r->buf_len - consumed > MAX_RESYNC_WINDOW)
                consumed = r->buf_len - DIF_SEQUENCE_BYTES;
            break;
        }
        if (p + DIF_SEQUENCE_BYTES > r->buf_len) {
            consumed = p;
            break;
        }

        if (handle_sequence(r, r->buf + p, dseq) != 0)
            return -1;

        consumed = p + DIF_SEQUENCE_BYTES;
    }

    if (consumed > 0) {
        memmove(r->buf, r->buf + consumed, r->buf_len - consumed);
        r->buf_len -= consumed;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Layer 2: OHCI buffer-fill records -> DIF bytes                      */
/* ------------------------------------------------------------------ */

/* Validates the 12 bytes of isoch header + CIP header we've accumulated, and
 * on success reports how many payload bytes follow, how much to skip after
 * them, and which format the CIP FMT field says this is. Kept strict so that
 * resync after a glitch can't latch onto noise. A packet of the other format
 * than the current one is valid here; ring_feed() decides what to do with it. */
static int iso_header_ok(const uint8_t *h, unsigned *dif_len, unsigned *skip_len,
                         dv_format_t *fmt_out)
{
    uint32_t w = (uint32_t)h[0] | ((uint32_t)h[1] << 8) |
                 ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 24);
    unsigned dlen = (w >> 16) & 0xFFFF;
    unsigned tag = (w >> 14) & 3;
    unsigned tcode = (w >> 4) & 0xF;

    if (tcode != ISO_TCODE || tag != ISO_TAG_CIP)
        return 0;
    if (dlen < CIP_HEADER_BYTES || dlen > ISO_MAX_DLEN)
        return 0;
    /* CIP quadlet 0 starts with 00b, quadlet 1 with 10b (IEC 61883-1). */
    if ((h[4] & 0xC0) != 0x00 || (h[8] & 0xC0) != 0x80)
        return 0;

    unsigned fmt = h[8] & 0x3F;
    dv_format_t f;
    if (fmt == CIP_FMT_DV) {
        if ((dlen - CIP_HEADER_BYTES) % DIF_BLOCK_BYTES != 0)
            return 0;
        f = DV_FORMAT_DV;
    } else if (fmt == CIP_FMT_MPEG2TS) {
        if (h[5] != HDV_DBS_QUADLETS ||
            (dlen - CIP_HEADER_BYTES) % HDV_SOURCE_PACKET_BYTES != 0)
            return 0;
        f = DV_FORMAT_HDV;
    } else {
        return 0;
    }
    *fmt_out = f;
    *dif_len = dlen - CIP_HEADER_BYTES;
    /* Payload is padded to a quadlet, then the status/timestamp trailer. */
    *skip_len = ((dlen + 3u) & ~3u) - dlen + ISO_TRAILER_BYTES;
    return 1;
}

/* CIP header layout for DV (IEC 61883-1/-2), the 8 bytes following the isoch
 * header: 01 SID/DBS(=0x78, 480 bytes per data block) FN/QPC DBC | 80 FMT/FDF
 * SYT_hi SYT_lo. iso_hdr[] holds [0..3] isoch header, [4..11] CIP, so the DBC
 * byte is iso_hdr[7].
 *
 * DBC counts data blocks, so a packet carrying `blocks` DIF blocks advances it
 * by `blocks`. Empty packets carry the next expected value unchanged. A jump
 * means the device, the USB path or the 1394 bus lost data — this is what a
 * "were any frames dropped?" answer rests on. */
static void check_dbc(dv_reassembler_t *r, unsigned dbc, unsigned blocks)
{
    /* Empty packets are skipped: they carry no data, and implementations
     * disagree on whether their DBC is the previous or the next value, which
     * would make every one of them look like a gap. */
    if (blocks == 0)
        return;

    if (!r->dbc_valid) {
        r->dbc_valid = 1;
        r->dbc_next = (dbc + blocks) & 0xFF;
        r->dbc_blocks += blocks;
        return;
    }

    if (dbc != r->dbc_next) {
        /* Before frame assembly has locked on we are joining a ring that was
         * already running: the first read lands mid-record and layer 2
         * resyncs a quadlet at a time, discarding bytes by design. Exactly
         * one discontinuity always showed up there, independent of run length
         * (measured over 20 s, 30 s, 60 s and 300 s captures), so counting it
         * as loss would put a permanent false positive on every run. */
        int joining = (r->format == DV_FORMAT_HDV)
                          ? r->ts_packets - r->ts_join_base < HDV_JOIN_TS_PACKETS
                          : !r->frame_started;
        r->dbc_epoch++;
        if (!joining) {
            /* Distance forward, modulo 256. Anything else (a backwards jump)
             * is counted as a single lost block rather than ~255. */
            unsigned gap = (dbc - r->dbc_next) & 0xFF;
            unsigned lost = (gap > 0 && gap < 128) ? gap : 1;
            r->dbc_gaps++;
            r->dbc_lost_blocks += lost;
            if (r->format == DV_FORMAT_DV) {
                r->dbc_pending = (r->dbc_pending + gap) & 0xFF;
                r->dbc_gap_pending = 1;
            }
            if (r->out.on_loss)
                r->out.on_loss(lost, 0, r->out.user);
        } else {
            r->dbc_joins++;
        }
    }

    r->dbc_next = (dbc + blocks) & 0xFF;
    r->dbc_blocks += blocks;
}

/* True if `p[0..len)` contains the 4-byte sequence `pat`. */
static int contains4(const uint8_t *p, size_t len, const uint8_t *pat)
{
    for (size_t i = 0; i + 4 <= len; i++)
        if (memcmp(p + i, pat, 4) == 0)
            return 1;
    return 0;
}

/* Appends one 188-byte TS packet to the picture currently being
 * accumulated, growing the buffer as needed. */
static int ts_pic_append(dv_reassembler_t *r, const uint8_t *pkt)
{
    if (r->ts_pic_len + TS_PACKET_BYTES > r->ts_pic_cap) {
        size_t new_cap = r->ts_pic_cap ? r->ts_pic_cap * 2 : (64u << 10);
        while (new_cap < r->ts_pic_len + TS_PACKET_BYTES)
            new_cap *= 2;
        uint8_t *nb = realloc(r->ts_pic_buf, new_cap);
        if (!nb)
            return -1;
        r->ts_pic_buf = nb;
        r->ts_pic_cap = new_cap;
    }
    memcpy(r->ts_pic_buf + r->ts_pic_len, pkt, TS_PACKET_BYTES);
    r->ts_pic_len += TS_PACKET_BYTES;
    return 0;
}

/* Emits the picture accumulated in r->ts_pic_buf to the output callbacks and
 * resets the accumulator so the next picture starts empty. */
static int emit_picture(dv_reassembler_t *r)
{
    int rc = 0;
    if (r->ts_pic_len > 0) {
        if (r->out.write && r->out.write(r->ts_pic_buf, r->ts_pic_len, r->out.user) != 0)
            rc = -1;
        else if (r->out.on_unit)
            r->out.on_unit(DV_FORMAT_HDV, r->ts_pic_buf, r->ts_pic_len, r->out.user);
    }
    r->ts_pic_len = 0;
    return rc;
}

/* Drops the picture in progress (never handed to the output callbacks) and
 * rolls the packet count back to match. */
static void ts_drop_picture(dv_reassembler_t *r)
{
    if (r->ts_gate_open && r->ts_pic_active)
        r->ts_packets = r->ts_pic_start_packets;
    r->ts_pic_active = 0;
    r->ts_pic_len = 0;
}

/* Closes the gate: nothing is written until the next picture that opens a GOP
 * (ts_gate_try_open()). PAT/PMT and the CC table stay. */
static void ts_rearm_gate(dv_reassembler_t *r)
{
    ts_drop_picture(r);
    r->ts_gate_open = 0;
    r->ts_video_pid = 0;
    r->ts_newpid = 0;
    r->ts_newpid_gops = 0;
}

/* Opens the gate on packet p if it is a video PES start with a sequence
 * header, i.e. the start of a GOP, so the decoder has everything it needs
 * from packet one. Returns 1 if it opened (p is the first packet of the
 * first picture), 0 if not, -1 if an output callback failed. */
static int ts_gate_try_open(dv_reassembler_t *r, const uint8_t *p, unsigned po, unsigned pid,
                            int video_start)
{
    static const uint8_t seq_hdr[4] = { 0x00, 0x00, 0x01, 0xB3 };
    if (!(video_start && contains4(p + po, TS_PACKET_BYTES - po, seq_hdr)))
        return 0;
    r->ts_gate_open = 1;
    r->ts_video_pid = (int)pid;
    r->ts_newpid = 0;
    r->ts_newpid_gops = 0;
    /* PAT/PMT replay is its own small unit, written immediately and
     * unconditionally -- unlike a picture, it is never dropped even if
     * the capture stops before the first picture completes. */
    if (r->ts_pat_valid) {
        if (r->out.write && r->out.write(r->ts_pat, TS_PACKET_BYTES, r->out.user) != 0)
            return -1;
        r->ts_packets++;
    }
    if (r->ts_pmt_valid) {
        if (r->out.write && r->out.write(r->ts_pmt, TS_PACKET_BYTES, r->out.user) != 0)
            return -1;
        r->ts_packets++;
    }
    r->ts_pic_start_packets = r->ts_packets;
    r->ts_pic_active = 1;
    if (ts_pic_append(r, p) != 0)
        return -1;
    r->ts_packets++;
    return 1;
}

/* HDV: handles one complete 188-byte TS packet. Buffers it into the picture
 * currently being accumulated (starting a new one, and flushing the
 * previous complete one, at each video PES start for the tracked PID);
 * nothing is handed to the output callbacks until a whole picture is ready.
 * Returns -1 if an output callback failed, 0 otherwise. */
static int ts_handle_packet(dv_reassembler_t *r)
{
    static const uint8_t seq_hdr[4] = { 0x00, 0x00, 0x01, 0xB3 };
    const uint8_t *p = r->ts_buf;

    if (p[0] != 0x47) {
        /* Before the gate opens this is expected join noise (stale ring
         * contents); afterwards it means the framing lost sync. */
        if (r->ts_gate_open)
            r->ts_sync_errors++;
        else
            r->ts_discarded++;
        return 0;
    }

    unsigned pid = ((p[1] & 0x1F) << 8) | p[2];
    int pusi = (p[1] & 0x40) != 0;
    unsigned afc = (p[3] >> 4) & 3;
    uint8_t cc = p[3] & 0x0F;
    unsigned po = (afc & 2) ? 5u + p[4] : 4u; /* payload offset */
    if (po > TS_PACKET_BYTES)
        po = TS_PACKET_BYTES;

    /* Continuity, only judged once we're writing: what precedes the gate is
     * discarded anyway. */
    if (pid != 0x1FFF && (afc & 1)) {
        int discontinuity = (afc & 2) && p[4] > 0 && (p[5] & 0x80);
        uint8_t last = r->ts_cc[pid];
        if (r->ts_gate_open && last != 0xFF && !discontinuity && cc != last &&
            cc != ((last + 1) & 0x0F)) {
            if (r->ts_pid_epoch[pid] != r->dbc_epoch)
                r->ts_cc_forgiven++;
            else
                r->ts_cc_errors++;
        }
        r->ts_cc[pid] = cc;
        r->ts_pid_epoch[pid] = r->dbc_epoch;
    }

    /* Remember the latest PAT and PMT so the gate can replay them. Both are
     * a single packet each on HDV. */
    if (pusi && (afc & 1)) {
        if (pid == 0 && po + 13 <= TS_PACKET_BYTES) {
            unsigned sec = po + 1u + p[po]; /* skip pointer_field */
            if (sec + 12 <= TS_PACKET_BYTES && p[sec] == 0x00) {
                unsigned end = sec + 3 + (((p[sec + 1] & 0x0F) << 8) | p[sec + 2]) - 4; /* before CRC */
                if (end > TS_PACKET_BYTES)
                    end = TS_PACKET_BYTES;
                for (unsigned e = sec + 8; e + 4 <= end; e += 4) {
                    if (((p[e] << 8) | p[e + 1]) != 0) { /* program 0 is the NIT */
                        r->ts_pmt_pid = ((p[e + 2] & 0x1F) << 8) | p[e + 3];
                        break;
                    }
                }
                memcpy(r->ts_pat, p, TS_PACKET_BYTES);
                r->ts_pat_valid = 1;
            }
        } else if (r->ts_pmt_pid && (int)pid == r->ts_pmt_pid) {
            memcpy(r->ts_pmt, p, TS_PACKET_BYTES);
            r->ts_pmt_valid = 1;
        }
    }

    /* A video PES start (stream_id 0xE0..0xEF) begins a picture. */
    int video_start = pusi && (afc & 1) && po + 4 <= TS_PACKET_BYTES &&
                      p[po] == 0x00 && p[po + 1] == 0x00 && p[po + 2] == 0x01 &&
                      (p[po + 3] & 0xF0) == 0xE0;

    if (!r->ts_gate_open) {
        int rc = ts_gate_try_open(r, p, po, pid, video_start);
        if (rc == 0)
            r->ts_discarded++;
        return rc < 0 ? -1 : 0;
    }

    if (video_start && (int)pid != r->ts_video_pid &&
        contains4(p + po, TS_PACKET_BYTES - po, seq_hdr)) {
        /* A GOP on another video PID. The tracked one's pictures never end
         * while its PID is silent, so a PID change that persists is followed:
         * the picture in progress (which ran on into the new PID) is
         * dropped and the gate opens again here. */
        if (r->ts_newpid == (int)pid) {
            r->ts_newpid_gops++;
        } else {
            r->ts_newpid = (int)pid;
            r->ts_newpid_gops = 1;
        }
        if (r->ts_newpid_gops >= DV_TS_PID_SWITCH_GOPS) {
            ts_rearm_gate(r);
            r->ts_pid_switches++;
            return ts_gate_try_open(r, p, po, pid, video_start) < 0 ? -1 : 0;
        }
    }

    if (video_start && (int)pid == r->ts_video_pid) {
        r->ts_newpid_gops = 0;
        if (r->ts_pic_active && emit_picture(r) != 0)
            return -1;
        r->ts_pic_start_packets = r->ts_packets;
        r->ts_pic_active = 1;
    }

    if (ts_pic_append(r, p) != 0)
        return -1;
    r->ts_packets++;
    if (r->ts_pic_len > DV_TS_PIC_MAX) {
        /* no picture start for far too long (the video PID went silent):
         * drop what piled up and wait for the next GOP, on whatever PID */
        ts_rearm_gate(r);
        r->ts_pic_overflows++;
    }
    return 0;
}

static int ts_feed(dv_reassembler_t *r, const uint8_t *data, size_t len)
{
    r->dif_bytes += len;
    while (len > 0) {
        unsigned off = r->iso_pos % HDV_SOURCE_PACKET_BYTES;
        size_t n;
        if (off < HDV_SPH_BYTES) {
            n = HDV_SPH_BYTES - off;
            if (n > len)
                n = len;
        } else {
            n = HDV_SOURCE_PACKET_BYTES - off;
            if (n > len)
                n = len;
            memcpy(r->ts_buf + r->ts_have, data, n);
            r->ts_have += (unsigned)n;
            if (r->ts_have == TS_PACKET_BYTES) {
                if (ts_handle_packet(r) != 0)
                    return -1;
                r->ts_have = 0;
            }
        }
        r->iso_pos += (unsigned)n;
        data += n;
        len -= n;
    }
    return 0;
}

/* DV state back to "waiting for a frame start" (the confirmed standard stays). */
static void dv_reset(dv_reassembler_t *r)
{
    r->frame_started = 0;
    r->expected_dseq = 0;
    r->frame_dsf_pal = r->frame_dsf_ntsc = 0;
    r->std_cand_frames = 0;
    r->dbc_pending = 0;
    r->dbc_gap_pending = 0;
    r->buf_len = 0;
}

/* HDV state back to a closed gate with nothing known: a new transport stream
 * starts on a sequence header with its own PAT/PMT, PIDs and CC table. */
static void hdv_reset(dv_reassembler_t *r)
{
    ts_rearm_gate(r);
    r->ts_pmt_pid = 0;
    r->ts_pat_valid = r->ts_pmt_valid = 0;
    r->ts_have = 0;
    memset(r->ts_cc, 0xFF, sizeof(r->ts_cc));
    r->ts_join_base = r->ts_packets;
}

/* Forgets the run of other-format packets (it was too short to switch). */
static void fmt_cand_drop(dv_reassembler_t *r)
{
    r->fmt_dropped_packets += r->fmt_cand_packets;
    r->fmt_cand = DV_FORMAT_UNKNOWN;
    r->fmt_cand_packets = 0;
    r->fmt_cand_len = 0;
}

/* Keeps a payload of the run aside (at most DV_FMT_SWITCH_PACKETS of them). Out of
 * memory it is just not kept: the replay then has a gap, as a lost packet would. */
static int fmt_cand_append(dv_reassembler_t *r, const uint8_t *data, size_t n)
{
    if (r->fmt_cand_len + n > r->fmt_cand_cap) {
        size_t cap = r->fmt_cand_cap ? r->fmt_cand_cap : (64u << 10);
        while (cap < r->fmt_cand_len + n)
            cap *= 2;
        uint8_t *nb = realloc(r->fmt_cand_buf, cap);
        if (!nb)
            return 0;
        r->fmt_cand_buf = nb;
        r->fmt_cand_cap = cap;
    }
    memcpy(r->fmt_cand_buf + r->fmt_cand_len, data, n);
    r->fmt_cand_len += n;
    return 0;
}

/* The other format persisted: end the current one's unit as at the end of a
 * capture (a complete DV frame is emitted, a partial unit dropped), start the
 * new one afresh and replay the run that made the switch into it. */
static int switch_format(dv_reassembler_t *r, dv_format_t fmt)
{
    int rc = 0;
    if (r->format == DV_FORMAT_DV)
        rc = dv_end_frame(r);
    dv_reset(r);
    hdv_reset(r);
    r->format = fmt;
    r->format_switches++;
    r->dbc_valid = 0; /* the new stream's DBC runs on its own */
    if (rc == 0 && r->fmt_cand_len > 0) {
        /* each payload is whole DIF blocks / source packets, so the run
         * back to back parses exactly as the packets did */
        r->iso_pos = 0;
        rc = fmt == DV_FORMAT_HDV ? ts_feed(r, r->fmt_cand_buf, r->fmt_cand_len)
                                  : dif_feed(r, r->fmt_cand_buf, r->fmt_cand_len);
    }
    r->fmt_cand = DV_FORMAT_UNKNOWN;
    r->fmt_cand_packets = 0;
    r->fmt_cand_len = 0;
    return rc;
}

static int ring_feed(dv_reassembler_t *r, const uint8_t *data, size_t len)
{
    while (len > 0) {
        if (r->iso_dif_left > 0) {
            size_t n = r->iso_dif_left < len ? r->iso_dif_left : len;
            int rc = r->iso_to_cand ? fmt_cand_append(r, data, n)
                     : (r->format == DV_FORMAT_HDV) ? ts_feed(r, data, n)
                                                    : dif_feed(r, data, n);
            if (rc != 0)
                return -1;
            data += n;
            len -= n;
            r->iso_dif_left -= (unsigned)n;
            continue;
        }
        if (r->iso_skip_left > 0) {
            size_t n = r->iso_skip_left < len ? r->iso_skip_left : len;
            data += n;
            len -= n;
            r->iso_skip_left -= (unsigned)n;
            continue;
        }

        /* Accumulate isoch header + CIP header. */
        size_t want = sizeof(r->iso_hdr) - r->iso_hdr_have;
        size_t n = want < len ? want : len;
        memcpy(r->iso_hdr + r->iso_hdr_have, data, n);
        r->iso_hdr_have += (unsigned)n;
        data += n;
        len -= n;
        if (r->iso_hdr_have < sizeof(r->iso_hdr))
            break;

        unsigned dif_len = 0, skip_len = 0;
        dv_format_t fmt = DV_FORMAT_UNKNOWN;
        if (iso_header_ok(r->iso_hdr, &dif_len, &skip_len, &fmt)) {
            r->iso_to_cand = 0;
            if (dif_len == 0) {
                /* empty packets of either format: rate padding, no data */
                r->iso_empty++;
            } else if (r->format == DV_FORMAT_UNKNOWN || fmt == r->format) {
                if (r->fmt_cand != DV_FORMAT_UNKNOWN)
                    fmt_cand_drop(r); /* the other format's run ended too short */
                r->format = fmt;
                r->iso_packets++;
            } else {
                /* the other format: counted, kept aside; it becomes the
                 * format once DV_FMT_SWITCH_PACKETS of it come in a row */
                if (r->fmt_cand != fmt) {
                    fmt_cand_drop(r);
                    r->fmt_cand = fmt;
                }
                if (++r->fmt_cand_packets >= DV_FMT_SWITCH_PACKETS) {
                    /* the run so far is replayed, this packet fed below */
                    if (switch_format(r, fmt) != 0)
                        return -1;
                    r->iso_packets++;
                } else {
                    r->iso_to_cand = 1;
                }
            }
            r->iso_pos = 0;
            /* CIP data block size is DBS quadlets (0x78 = 480 bytes for DV,
             * 6 = 24 bytes for HDV), not one 80-byte DIF block. A packet kept
             * aside is not checked: if it was a stray, the next packet of the
             * current format shows the data it took as a gap. */
            unsigned dbs_bytes = (r->iso_hdr[5] ? r->iso_hdr[5] : 256u) * 4u;
            if (!r->iso_to_cand && (dif_len == 0 || fmt == r->format))
                check_dbc(r, r->iso_hdr[7], dif_len / dbs_bytes);
            r->iso_dif_left = dif_len;
            r->iso_skip_left = skip_len;
            r->iso_hdr_have = 0;
        } else {
            /* Resync a quadlet at a time: the ring is quadlet-aligned, and
             * descriptor boundaries can leave a gap between records. */
            r->iso_resyncs++;
            memmove(r->iso_hdr, r->iso_hdr + 4, sizeof(r->iso_hdr) - 4);
            r->iso_hdr_have -= 4;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Layer 1: type-9 messages -> ring bytes                              */
/* ------------------------------------------------------------------ */

int dv_reassembler_feed(dv_reassembler_t *r, const uint8_t *data, size_t len)
{
    r->bytes_fed += len;

    while (len > 0) {
        if (r->msg_left > 0) {
            size_t n = r->msg_left < len ? r->msg_left : len;
            if (r->msg_is_ring && ring_feed(r, data, n) != 0)
                return -1;
            data += n;
            len -= n;
            r->msg_left -= (unsigned)n;
            continue;
        }

        size_t want = sizeof(r->msg_hdr) - r->msg_hdr_have;
        size_t n = want < len ? want : len;
        memcpy(r->msg_hdr + r->msg_hdr_have, data, n);
        r->msg_hdr_have += (unsigned)n;
        data += n;
        len -= n;
        if (r->msg_hdr_have < sizeof(r->msg_hdr))
            break;

        uint32_t w = (uint32_t)r->msg_hdr[0] | ((uint32_t)r->msg_hdr[1] << 8) |
                     ((uint32_t)r->msg_hdr[2] << 16) | ((uint32_t)r->msg_hdr[3] << 24);
        if ((w >> 28) == MSG_TYPE_DATA) {
            unsigned addr = w & 0xFFFF;
            r->msg_left = (w >> 16) & 0x7FF;
            r->msg_is_ring = (addr >= RING_ADDR_LO && addr < RING_ADDR_HI);
            r->msg_hdr_have = 0;
        } else {
            /* Not a data message — shift one byte and keep looking. The
             * stream is quadlet-aligned in practice, but a byte-wise resync
             * costs nothing and recovers from any offset. */
            r->msg_resyncs++;
            memmove(r->msg_hdr, r->msg_hdr + 1, sizeof(r->msg_hdr) - 1);
            r->msg_hdr_have -= 1;
        }
    }

    return 0;
}

int dv_reassembler_init(dv_reassembler_t *r, const dv_output_t *out)
{
    memset(r, 0, sizeof(*r));
    r->out = *out;
    r->buf_cap = MAX_RESYNC_WINDOW + DIF_SEQUENCE_BYTES;
    r->buf = malloc(r->buf_cap);
    if (!r->buf)
        return -1;
    r->system_seq_count = NTSC_SEQ_COUNT;
    memset(r->ts_cc, 0xFF, sizeof(r->ts_cc));
    return 0;
}

void dv_reassembler_finish(dv_reassembler_t *r)
{
    if (r->format == DV_FORMAT_HDV) {
        /* Drop the trailing partial picture (never handed to the output
         * callbacks in the first place) so the output ends on a whole one,
         * and roll the packet count back to match. */
        ts_drop_picture(r);
    } else {
        /* a complete frame not yet flushed is emitted, a partial one dropped */
        dv_end_frame(r);
    }
    if (r->fmt_cand != DV_FORMAT_UNKNOWN)
        fmt_cand_drop(r);
    free(r->buf);
    r->buf = NULL;
    free(r->ts_pic_buf);
    r->ts_pic_buf = NULL;
    free(r->fmt_cand_buf);
    r->fmt_cand_buf = NULL;
}
