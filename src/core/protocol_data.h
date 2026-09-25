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
 * Pinnacle 500-USB ("Marvin-Lite") — byte sequences still replayed verbatim
 * from traces/20260922-142720-coldboot-driver-init.pcapng: the config-channel
 * bring-up (EP 0x01) around the FPGA bitstream upload, and the bitstream's
 * chunking.
 *
 * The 1394 start and stop sequences on EP 0x02 used to live here as 232 + 4
 * replayed packets. They are now generated step by step in pinnacle_1394.c
 * and explained in docs/startup.md; git history has the old tables, and
 * tools/seqdecode.py decodes any EP 0x02 trace.
 */

#ifndef PINNACLE_PROTOCOL_DATA_H
#define PINNACLE_PROTOCOL_DATA_H

#include <stdint.h>

#define PINNACLE_PROTO_MAX_PKT 156

typedef struct {
    unsigned delay_ms;
    unsigned len;
    uint8_t data[PINNACLE_PROTO_MAX_PKT];
} pinnacle_pkt_t;

/* FPGA bitstream upload chunk sizes, in order (sum = 78,422 bytes). Static
 * across independent cold-boot and detach/reattach captures (byte-identical,
 * MD5 3888c23c9bcc81c88964c45d981a0b68) — replayed verbatim, semantics
 * unknown and irrelevant (it's the FPGA's own raw .rbf load). */
static const unsigned PINNACLE_BITSTREAM_CHUNK_SIZES[] = { 17408, 16384, 16384, 16384, 11862 };
#define PINNACLE_BITSTREAM_CHUNK_COUNT 5
#define PINNACLE_BITSTREAM_TOTAL_SIZE 78422

/* 80 bulk-OUT transfers on the low-level config channel (EP 0x01/0x81),
 * captured from traces/20260922-142720-coldboot-driver-init.pcapng, from
 * the first byte after SET_INTERFACE selects alt 0 up to (and including)
 * the last one before the FPGA bitstream upload begins. Most of these
 * (the ones with a constant 0x4a second byte) are SAA7113 analog-decoder
 * I2C register writes; the rest (0x03/0x04/0x05/0x07/0x0c prefixed, and
 * the two 10-byte 0x80-prefixed reads) are unidentified firmware/EEPROM
 * commands. Previously assumed skippable for pure DV capture -- that
 * assumption is unverified and is exactly what this replay tests. See
 * docs/command-channel-findings.md. */
static const pinnacle_pkt_t PINNACLE_CONFIG_PREBITSTREAM_SEQ[] = {
    { .delay_ms = 0, .len = 2, .data = { 0x07, 0x00 } },
    { .delay_ms = 5, .len = 2, .data = { 0x0c, 0x01 } },
    { .delay_ms = 1249, .len = 2, .data = { 0x03, 0x4a } },
    { .delay_ms = 5, .len = 2, .data = { 0x04, 0x4a } },
    { .delay_ms = 59, .len = 5, .data = { 0x02, 0x4a, 0x01, 0x01, 0x00 } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x01, 0x08 } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x02, 0xc0 } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x03, 0x33 } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x04, 0x00 } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x05, 0x00 } },
    { .delay_ms = 8, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x06, 0xe9 } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x07, 0x0d } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x08, 0xb8 } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x09, 0x01 } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x0a, 0x80 } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x0b, 0x47 } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x0c, 0x40 } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x0d, 0x00 } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x0e, 0x01 } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x0f, 0x2a } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x11, 0x04 } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x12, 0x02 } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x13, 0x00 } },
    { .delay_ms = 8, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x15, 0x00 } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x16, 0x00 } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x17, 0x00 } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x40, 0x02 } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x41, 0x00 } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x42, 0x00 } },
    { .delay_ms = 8, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x43, 0x00 } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x44, 0x00 } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x45, 0x00 } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x46, 0x00 } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x47, 0x00 } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x48, 0x00 } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x49, 0x00 } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x4a, 0x00 } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x4b, 0x00 } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x4c, 0x00 } },
    { .delay_ms = 6, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x4d, 0x00 } },
    { .delay_ms = 5, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x4e, 0x00 } },
    { .delay_ms = 5, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x4f, 0x00 } },
    { .delay_ms = 5, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x50, 0x00 } },
    { .delay_ms = 5, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x51, 0x00 } },
    { .delay_ms = 5, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x52, 0x00 } },
    { .delay_ms = 5, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x53, 0x00 } },
    { .delay_ms = 5, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x54, 0x00 } },
    { .delay_ms = 5, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x55, 0xff } },
    { .delay_ms = 5, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x56, 0xff } },
    { .delay_ms = 5, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x57, 0xff } },
    { .delay_ms = 5, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x58, 0x00 } },
    { .delay_ms = 5, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x59, 0x54 } },
    { .delay_ms = 5, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x5a, 0x07 } },
    { .delay_ms = 5, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x5b, 0x83 } },
    { .delay_ms = 5, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x5e, 0x00 } },
    { .delay_ms = 5, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x02, 0xc0 } },
    { .delay_ms = 5, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x09, 0x01 } },
    { .delay_ms = 5, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x09, 0x00 } },
    { .delay_ms = 5, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x0e, 0x89 } },
    { .delay_ms = 5, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x0f, 0x2a } },
    { .delay_ms = 5, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x10, 0x00 } },
    { .delay_ms = 5, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x10, 0x40 } },
    { .delay_ms = 5, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x08, 0xf8 } },
    { .delay_ms = 6, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x40, 0x82 } },
    { .delay_ms = 5, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x5a, 0x0a } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x02, 0xc0 } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x09, 0x00 } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x0a, 0x7f } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x0b, 0x46 } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x0c, 0x42 } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x09, 0x01 } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x08, 0xe8 } },
    { .delay_ms = 7, .len = 5, .data = { 0x01, 0x4a, 0x02, 0x11, 0x0c } },
    { .delay_ms = 7, .len = 2, .data = { 0x03, 0x00 } },
    { .delay_ms = 7, .len = 2, .data = { 0x04, 0x00 } },
    { .delay_ms = 7, .len = 2, .data = { 0x03, 0xf0 } },
    { .delay_ms = 7, .len = 2, .data = { 0x04, 0xf0 } },
    { .delay_ms = 77, .len = 10, .data = { 0x80, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
    { .delay_ms = 7, .len = 10, .data = { 0x80, 0x03, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
    { .delay_ms = 7, .len = 2, .data = { 0x05, 0x00 } },
};
#define PINNACLE_CONFIG_PREBITSTREAM_SEQ_COUNT 80

/* Single exchange on the same channel observed right after the bitstream
 * upload completes, before SET_INTERFACE selects alt 1. */
static const pinnacle_pkt_t PINNACLE_CONFIG_POSTBITSTREAM_SEQ[] = {
    { .delay_ms = 0, .len = 2, .data = { 0x06, 0x00 } },
};
#define PINNACLE_CONFIG_POSTBITSTREAM_SEQ_COUNT 1

#endif /* PINNACLE_PROTOCOL_DATA_H */
