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
 * Host-side FX2 firmware download (Cypress "A0" loader). The image is a raw
 * 8051 binary loaded at address 0 in 512-byte blocks, exactly as the vendor
 * driver does it (MarvinAVS64.sys FUN_0002bf8c: vendor request 0xA0, wValue =
 * block * 512). The transport is a callback so the sequence can be unit
 * tested without a device.
 */

#ifndef PINNACLE_FX2_H
#define PINNACLE_FX2_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PINNACLE_FX2_CPUCS 0xE600u     /* CPU control/status register; bit 0 = hold 8051 in reset */
#define PINNACLE_FX2_BLOCK 512u
#define PINNACLE_FX2_MAX_IMAGE 0x4000u /* the FX2's 16 KB program RAM */

/* Sends one vendor OUT request 0xA0 (bmRequestType 0x40) with the given
 * wValue and data. Returns 0 on success, non-zero on failure. */
typedef int (*pinnacle_fx2_write_fn)(void *user, uint16_t addr, const uint8_t *data, uint16_t len);

/* 0 if the image is plausible for the FX2: non-empty, at most 16 KB and
 * starting with an 8051 LJMP (0x02), which every FX2 firmware has at the
 * reset vector. */
int pinnacle_fx2_validate(const uint8_t *img, size_t len);

/* Block i of an image of len bytes: its target address and byte count.
 * Returns 0 and sets *addr / *n, or -1 when i is past the last block. */
int pinnacle_fx2_block(size_t len, unsigned i, uint16_t *addr, uint16_t *n);

/* The load sequence: CPUCS = 1 (reset), every block in order, CPUCS = 0 (run).
 * settle_ms is slept after each CPUCS write (the vendor driver waits 50 ms).
 * Returns 0 or the first non-zero callback result. */
int pinnacle_fx2_download(const uint8_t *img, size_t len, pinnacle_fx2_write_fn write,
                          void *user, unsigned settle_ms);

#ifdef __cplusplus
}
#endif

#endif /* PINNACLE_FX2_H */
