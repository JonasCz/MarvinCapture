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
 * make_ep88 -- builds a raw EP 0x88 dump (the byte stream the device sends while its
 * isochronous receive runs, see docs/protocol.md and src/core/dv_reassembler.c) from
 * plain DV and MPEG-2 TS files, so the tests can drive dv_reassembler and the replay
 * device without a recording of the real endpoint.
 *
 * usage: make_ep88 [--no-junk] <out.bin> <in.dv|in.ts>[@N]...
 *
 * The inputs are wrapped one after the other into one continuous stream, each by its
 * content: a file that starts with the TS sync byte 0x47 is an MPEG-2 TS (HDV, every
 * 188-byte packet must start with 0x47); one that starts with a DIF header block is DV,
 * whole frames, each frame 144,000 or 120,000 bytes by the DSF bit of its own header
 * block (so PAL and NTSC can follow each other, even within one file). "@N" after a
 * name takes only its first N TS packets or DV frames (a stray burst of one format in
 * the other).
 *
 * What it writes, from the inside out:
 *  - IEC 61883 packets as a camera sends them. DV (61883-2): 6 DIF blocks = 480 bytes
 *    behind the 8-byte CIP header (SID 1, DBS 0x78, FMT 0x00, FDF 0x80 at 50 Hz), and an
 *    empty CIP-only packet after every 15 (the rate padding to the 8 kHz cycle).
 *    HDV (61883-4): 0 to 3 source packets of 192 bytes (a 4-byte SPH timestamp and one
 *    TS packet; 8 data blocks of 24 bytes) behind the CIP header (DBS 6, FN 3, SPH 1,
 *    FMT 0x20). DBC counts data blocks, continuous across all inputs.
 *  - Each packet as an OHCI isochronous-receive buffer-fill record in the receive ring:
 *    the isoch header (dataLength, tag 1, channel 63, tcode 0xA), the packet padded to a
 *    quadlet, and the trailer (xferStatus 0x8411, cycle timestamp).
 *  - The ring (36,864 bytes at device address 0x7000) read out as type-9 messages of
 *    varying length (64..2047 bytes, not aligned to the records), with an IR descriptor
 *    status write-back message (0x118C / 0x119C, no stream data) each time it wraps.
 *  - Unless --no-junk, a few hundred bytes in front, as when the first read joins a
 *    stream that is already running: the tail of a message (layer 1 resynchronises) and a
 *    ring message with stale contents (layer 2 resynchronises). Neither contains anything
 *    that parses as a header, so the reassembler's output still starts with the first
 *    input frame: reassembled, a dump of DV files is byte-identical to the files.
 *
 * The output depends on the inputs only (no clock, no random seed), so the same inputs
 * always give the same dump.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RING_ADDR 0x7000u
#define RING_BYTES 0x9000u   /* 36,864: device addresses 0x7000..0xFFFF */
#define RING_START 0x8000u   /* where the first message reads from: the ring wraps early */
#define MSG_MIN 64u
#define MSG_MAX 2047u        /* the 11-bit length field */
#define STATUS_ADDR_A 0x118Cu
#define STATUS_ADDR_B 0x119Cu

#define CIP_BYTES 8
#define DIF_BLOCK 80
#define DV_PACKET_BYTES (6 * DIF_BLOCK) /* 480 */
#define DV_FRAME_PAL 144000
#define DV_FRAME_NTSC 120000
#define DV_EMPTY_EVERY 15

#define TS_PACKET 188
#define SRC_PACKET 192 /* SPH + TS packet */
#define HDV_MAX_PER_PACKET 3

/* source packets per isochronous packet, repeated (0 = an empty packet): ~2 per cycle,
 * about what a 25 Mbit/s HDV stream needs at 8000 cycles a second */
static const unsigned hdv_pattern[] = { 2, 3, 2, 0, 2, 3, 2, 1 };

typedef struct {
    FILE *out;
    uint32_t rng;
    /* layer 1: the ring read out as type-9 messages */
    uint8_t msg[MSG_MAX];
    unsigned msg_len, msg_want;
    uint32_t msg_addr;
    uint32_t ring_pos;      /* ring offset of the next byte */
    unsigned long wraps;
    /* layer 2: isochronous packets */
    unsigned dbc;
    unsigned long cycle;
    unsigned dv_since_empty;
    unsigned long hdv_step;
    unsigned long bytes_out;
} ep88_t;

static void die(const char *what, const char *path)
{
    fprintf(stderr, "make_ep88: %s%s%s\n", what, path ? ": " : "", path ? path : "");
    exit(1);
}

/* a fixed LCG: deterministic message lengths and junk */
static uint32_t rnd(ep88_t *e)
{
    e->rng = e->rng * 1103515245u + 12345u;
    return e->rng >> 16;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void out_bytes(ep88_t *e, const uint8_t *p, size_t n)
{
    if (fwrite(p, 1, n, e->out) != n)
        die("write failed", NULL);
    e->bytes_out += n;
}

static void msg_header(ep88_t *e, unsigned len, uint32_t addr)
{
    uint8_t h[4];
    put_le32(h, (9u << 28) | ((uint32_t)len << 16) | addr);
    out_bytes(e, h, 4);
}

static void msg_flush(ep88_t *e)
{
    if (e->msg_len > 0) {
        msg_header(e, e->msg_len, e->msg_addr);
        out_bytes(e, e->msg, e->msg_len);
    }
    e->msg_len = 0;
    e->msg_want = MSG_MIN + rnd(e) % (MSG_MAX - MSG_MIN + 1);
}

/* Appends bytes to the ring; they go out as type-9 messages of varying length, cut at
 * the ring's end, where the descriptor's status write-back follows. */
static void ring_put(ep88_t *e, const uint8_t *p, size_t n)
{
    while (n > 0) {
        if (e->msg_len == 0)
            e->msg_addr = RING_ADDR + e->ring_pos;
        size_t k = e->msg_want - e->msg_len;
        if (k > n)
            k = n;
        if (k > RING_BYTES - e->ring_pos)
            k = RING_BYTES - e->ring_pos;
        memcpy(e->msg + e->msg_len, p, k);
        e->msg_len += (unsigned)k;
        e->ring_pos += (uint32_t)k;
        p += k;
        n -= k;
        if (e->ring_pos == RING_BYTES) {
            msg_flush(e);
            uint8_t st[4];
            put_le32(st, 0x84110000u); /* xferStatus run|active|ack_complete, resCount 0 */
            msg_header(e, 4, (e->wraps & 1) ? STATUS_ADDR_B : STATUS_ADDR_A);
            out_bytes(e, st, 4);
            e->wraps++;
            e->ring_pos = 0;
        } else if (e->msg_len == e->msg_want) {
            msg_flush(e);
        }
    }
}

/* One isochronous packet (CIP header + payload) as a buffer-fill record. */
static void iso_record(ep88_t *e, const uint8_t cip[CIP_BYTES], const uint8_t *payload, size_t n)
{
    static const uint8_t pad[4];
    uint8_t q[4];
    unsigned dlen = (unsigned)(CIP_BYTES + n);
    put_le32(q, ((uint32_t)dlen << 16) | (1u << 14) | (63u << 8) | (0xAu << 4));
    ring_put(e, q, 4);
    ring_put(e, cip, CIP_BYTES);
    if (n > 0)
        ring_put(e, payload, n);
    ring_put(e, pad, ((dlen + 3u) & ~3u) - dlen);
    /* timestamp: 3 bits of seconds, 13 bits of cycle count */
    put_le32(q, (0x8411u << 16) | (uint32_t)(((e->cycle / 8000) & 7) << 13) | (uint32_t)(e->cycle % 8000));
    ring_put(e, q, 4);
    e->cycle++;
}

/* A DV packet: 6 DIF blocks, or NULL for an empty one (which carries the next DBC). */
static void dv_packet(ep88_t *e, const uint8_t *blocks, int pal)
{
    const uint8_t cip[CIP_BYTES] = { 0x01, 0x78, 0x00, (uint8_t)e->dbc,
                                     0x80, (uint8_t)(pal ? 0x80 : 0x00), 0xFF, 0xFF };
    iso_record(e, cip, blocks, blocks ? DV_PACKET_BYTES : 0);
    if (blocks)
        e->dbc = (e->dbc + 1) & 0xFF; /* DBS 0x78: one data block per packet */
}

static int is_dif_frame_start(const uint8_t *p)
{
    return p[0] == 0x1F && p[1] == 0x07 && p[2] == 0x00; /* header block, Dseq 0 */
}

static unsigned long wrap_dv(ep88_t *e, FILE *in, const char *path, unsigned long limit)
{
    static uint8_t frame[DV_FRAME_PAL];
    unsigned long frames = 0, pal_frames = 0;
    while (frames < limit) {
        size_t n = fread(frame, 1, DIF_BLOCK, in);
        if (n == 0)
            break;
        if (n != DIF_BLOCK || !is_dif_frame_start(frame)) {
            fprintf(stderr, "make_ep88: %s: frame %lu does not start with a DIF header block\n", path, frames);
            exit(1);
        }
        int pal = (frame[3] & 0x80) != 0; /* DSF: 625/50 */
        size_t len = pal ? DV_FRAME_PAL : DV_FRAME_NTSC;
        if (fread(frame + DIF_BLOCK, 1, len - DIF_BLOCK, in) != len - DIF_BLOCK) {
            fprintf(stderr, "make_ep88: %s: frame %lu is cut short\n", path, frames);
            exit(1);
        }
        for (size_t off = 0; off < len; off += DV_PACKET_BYTES) {
            dv_packet(e, frame + off, pal);
            if (++e->dv_since_empty == DV_EMPTY_EVERY) {
                dv_packet(e, NULL, pal);
                e->dv_since_empty = 0;
            }
        }
        frames++;
        pal_frames += pal;
    }
    if (frames == 0)
        die("no DV frames in", path);
    fprintf(stderr, "make_ep88: %s: %lu DV frame(s), %lu PAL, %lu NTSC\n", path, frames, pal_frames,
            frames - pal_frames);
    return frames;
}

/* An HDV packet of `count` source packets (0 = empty). */
static void hdv_packet(ep88_t *e, const uint8_t *src, unsigned count)
{
    const uint8_t cip[CIP_BYTES] = { 0x01, 0x06, 0xC4, (uint8_t)e->dbc, 0xA0, 0x00, 0x00, 0x00 };
    iso_record(e, cip, src, (size_t)count * SRC_PACKET);
    e->dbc = (e->dbc + 8u * count) & 0xFF; /* DBS 6: 8 data blocks per source packet */
}

static unsigned long wrap_ts(ep88_t *e, FILE *in, const char *path, unsigned long limit)
{
    uint8_t src[HDV_MAX_PER_PACKET * SRC_PACKET];
    unsigned long packets = 0;
    int eof = 0;
    while (!eof) {
        unsigned want = hdv_pattern[e->hdv_step++ % (sizeof(hdv_pattern) / sizeof(hdv_pattern[0]))];
        unsigned got = 0;
        while (got < want) {
            uint8_t *sp = src + (size_t)got * SRC_PACKET;
            size_t n = packets < limit ? fread(sp + 4, 1, TS_PACKET, in) : 0;
            if (n == 0) {
                eof = 1;
                break;
            }
            if (n != TS_PACKET || sp[4] != 0x47) {
                fprintf(stderr, "make_ep88: %s: TS packet %lu is %s\n", path, packets,
                        n != TS_PACKET ? "cut short" : "out of sync (no 0x47)");
                exit(1);
            }
            /* SPH: 7 reserved bits, then the 25-bit cycle time stamp (cycle count, offset) */
            uint32_t ts = ((uint32_t)(e->cycle % 8000) << 12) | (uint32_t)((packets * 1000) % 3072);
            sp[0] = (uint8_t)((ts >> 24) & 0x01);
            sp[1] = (uint8_t)(ts >> 16);
            sp[2] = (uint8_t)(ts >> 8);
            sp[3] = (uint8_t)ts;
            got++;
            packets++;
        }
        if (got > 0 || (want == 0 && !eof))
            hdv_packet(e, src, got);
    }
    if (packets == 0)
        die("no TS packets in", path);
    fprintf(stderr, "make_ep88: %s: %lu TS packet(s)\n", path, packets);
    return packets;
}

/* What the first read sees when it joins a running stream: the tail of a message, then a
 * ring message whose contents are stale. All junk bytes are below 0x80, so no 4-byte
 * window reads as a type-9 header (top nibble 9 in its last byte) and no quadlet as an
 * isochronous header (tcode 0xA in its first byte's high nibble); the junk message's own
 * header (addr 0xF000, length 256) has no such byte at the edge either. */
static void junk(ep88_t *e)
{
    uint8_t j[256];
    for (size_t i = 0; i < 150; i++)
        j[i] = (uint8_t)(rnd(e) & 0x7F);
    out_bytes(e, j, 150);
    for (size_t i = 0; i < sizeof(j); i++)
        j[i] = (uint8_t)(rnd(e) & 0x7F);
    e->msg_want = sizeof(j);
    ring_put(e, j, sizeof(j));
}

int main(int argc, char **argv)
{
    int arg = 1, with_junk = 1;
    if (arg < argc && strcmp(argv[arg], "--no-junk") == 0) {
        with_junk = 0;
        arg++;
    }
    if (argc - arg < 2) {
        fprintf(stderr, "usage: %s [--no-junk] <out.bin> <in.dv|in.ts>[@N]...\n", argv[0]);
        return 2;
    }

    ep88_t *e = calloc(1, sizeof(*e));
    if (!e)
        die("out of memory", NULL);
    e->rng = 0x4D415256u;
    e->ring_pos = RING_START;
    e->dbc = 0x40;
    e->out = fopen(argv[arg], "wb");
    if (!e->out)
        die("cannot create", argv[arg]);
    setvbuf(e->out, NULL, _IOFBF, 1u << 20);
    msg_flush(e); /* picks the first message length */
    if (with_junk)
        junk(e);

    for (int i = arg + 1; i < argc; i++) {
        /* name@N: the first N TS packets / DV frames only */
        char path[4096];
        unsigned long limit = (unsigned long)-1;
        snprintf(path, sizeof(path), "%s", argv[i]);
        char *at = strrchr(path, '@');
        if (at && at[1] && strspn(at + 1, "0123456789") == strlen(at + 1)) {
            limit = strtoul(at + 1, NULL, 10);
            *at = 0;
        }
        FILE *in = fopen(path, "rb");
        if (!in)
            die("cannot open", path);
        int c = fgetc(in);
        if (c == EOF)
            die("empty input", path);
        rewind(in);
        if (c == 0x47)
            wrap_ts(e, in, path, limit);
        else if (c == 0x1F)
            wrap_dv(e, in, path, limit);
        else
            die("neither DV (DIF header block) nor MPEG-2 TS (0x47)", path);
        fclose(in);
    }
    msg_flush(e);
    if (fclose(e->out) != 0)
        die("write failed", argv[arg]);
    fprintf(stderr, "make_ep88: %s: %lu bytes, %lu isochronous packets, ring wrapped %lu time(s)\n",
            argv[arg], e->bytes_out, e->cycle, e->wraps);
    free(e);
    return 0;
}
