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

#if defined(_WIN32)
#include <io.h>
#define pinnacle_ftruncate(fd, len) _chsize_s((fd), (len))
#define pinnacle_fileno _fileno
#else
#include <unistd.h>
#define pinnacle_ftruncate(fd, len) ftruncate((fd), (len))
#define pinnacle_fileno fileno
#endif

/* Cuts the output file back to `off`. Only used to discard a frame that the
 * capture stopped in the middle of. Returns 0 on success; a non-seekable
 * output (a pipe) fails here and the caller falls back to zero-padding. */
static int truncate_to(FILE *out, long off)
{
    if (off < 0 || fflush(out) != 0)
        return -1;
    if (pinnacle_ftruncate(pinnacle_fileno(out), off) != 0)
        return -1;
    if (fseek(out, off, SEEK_SET) != 0)
        return -1;
    return 0;
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

static int write_zero_sequence(dv_reassembler_t *r)
{
    static const uint8_t zeros[DIF_SEQUENCE_BYTES];
    if (fwrite(zeros, 1, DIF_SEQUENCE_BYTES, r->out) != DIF_SEQUENCE_BYTES)
        return -1;
    r->sequences_dropped++;
    return 0;
}

static int write_real_sequence(dv_reassembler_t *r, const uint8_t *p)
{
    if (fwrite(p, 1, DIF_SEQUENCE_BYTES, r->out) != DIF_SEQUENCE_BYTES)
        return -1;
    r->sequences_written++;
    return 0;
}

/* Handles one located DIF sequence at buf+p with the given dseq. Advances
 * frame/sequence bookkeeping and writes to the output file. */
static int handle_sequence(dv_reassembler_t *r, const uint8_t *p, unsigned dseq)
{
    if (dseq >= r->system_seq_count) {
        /* Only PAL has sequence numbers 10/11; upgrade our assumption. */
        r->system_seq_count = PAL_SEQ_COUNT;
    }

    if (dseq == 0) {
        if (r->frame_started) {
            while (r->expected_dseq < r->system_seq_count) {
                if (write_zero_sequence(r) != 0)
                    return -1;
                r->expected_dseq++;
            }
            r->frames_written++;
        }
        r->frame_started = 1;
        r->frame_start_off = ftell(r->out);
        r->frame_start_written = r->sequences_written;
        r->frame_start_dropped = r->sequences_dropped;
        if (write_real_sequence(r, p) != 0)
            return -1;
        r->expected_dseq = 1;
        return 0;
    }

    if (!r->frame_started) {
        /* Haven't seen sequence 0 yet; ignore until we do, so the file
         * always starts frame-aligned. */
        return 0;
    }

    if (dseq < r->expected_dseq) {
        /* Stale/duplicate/out-of-order match; ignore rather than rewind. */
        return 0;
    }

    while (r->expected_dseq < dseq) {
        if (write_zero_sequence(r) != 0)
            return -1;
        r->expected_dseq++;
    }
    if (write_real_sequence(r, p) != 0)
        return -1;
    r->expected_dseq = dseq + 1;
    return 0;
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
 * on success reports how many DIF bytes follow and how much to skip after
 * them. Kept strict so that resync after a glitch can't latch onto noise. */
static int iso_header_ok(const uint8_t *h, unsigned *dif_len, unsigned *skip_len)
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
    if ((dlen - CIP_HEADER_BYTES) % DIF_BLOCK_BYTES != 0)
        return 0;
    /* CIP quadlet 0 starts with 00b, quadlet 1 with 10b (IEC 61883-1). */
    if ((h[4] & 0xC0) != 0x00 || (h[8] & 0xC0) != 0x80)
        return 0;

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
        if (r->frame_started) {
            /* Distance forward, modulo 256. Anything else (a backwards jump)
             * is counted as a single lost block rather than ~255. */
            unsigned gap = (dbc - r->dbc_next) & 0xFF;
            r->dbc_gaps++;
            r->dbc_lost_blocks += (gap > 0 && gap < 128) ? gap : 1;
        } else {
            r->dbc_joins++;
        }
    }

    r->dbc_next = (dbc + blocks) & 0xFF;
    r->dbc_blocks += blocks;
}

static int ring_feed(dv_reassembler_t *r, const uint8_t *data, size_t len)
{
    while (len > 0) {
        if (r->iso_dif_left > 0) {
            size_t n = r->iso_dif_left < len ? r->iso_dif_left : len;
            if (dif_feed(r, data, n) != 0)
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
        if (iso_header_ok(r->iso_hdr, &dif_len, &skip_len)) {
            if (dif_len > 0)
                r->iso_packets++;
            else
                r->iso_empty++;
            /* CIP data block size is DBS quadlets (0x78 = 480 bytes for DV,
             * i.e. one data block per packet), not one 80-byte DIF block. */
            unsigned dbs_bytes = (r->iso_hdr[5] ? r->iso_hdr[5] : 256u) * 4u;
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

int dv_reassembler_init(dv_reassembler_t *r, FILE *out)
{
    memset(r, 0, sizeof(*r));
    r->out = out;
    r->buf_cap = MAX_RESYNC_WINDOW + DIF_SEQUENCE_BYTES;
    r->buf = malloc(r->buf_cap);
    if (!r->buf)
        return -1;
    r->system_seq_count = NTSC_SEQ_COUNT;
    return 0;
}

void dv_reassembler_finish(dv_reassembler_t *r)
{
    if (r->frame_started) {
        if (r->expected_dseq < r->system_seq_count) {
            /* Capture stopped mid-frame. Zero-padding it out would end every
             * file with a frame that is part black, so drop the partial frame
             * instead and leave the file a whole number of good frames. */
            if (truncate_to(r->out, r->frame_start_off) == 0) {
                r->sequences_written = r->frame_start_written;
                r->sequences_dropped = r->frame_start_dropped;
            } else {
                while (r->expected_dseq < r->system_seq_count) {
                    write_zero_sequence(r);
                    r->expected_dseq++;
                }
                r->frames_written++;
            }
        } else {
            r->frames_written++;
        }
        r->frame_started = 0;
    }
    free(r->buf);
    r->buf = NULL;
}
