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
 * The low-level config channel (EP 0x01 OUT / EP 0x81 IN), as named
 * operations instead of replayed bytes. Every request gets exactly one
 * reply whose first byte echoes the opcode. See docs/analog.md for where
 * each opcode was found in MarvinAVS64.sys.
 *
 *   01 addr n sub data..   I2C write of n bytes (sub + data) to addr
 *   02 addr nr nw sub      I2C read: write nw (=1) byte, read nr bytes;
 *                          reply "02 01 data.." (01 = acknowledged)
 *   03 addr / 04 addr      reset the chip at addr, assert then release
 *   05 00 / 06 00          FPGA loader ready / FPGA up; reply "0x 01" = yes
 *   07 00                  probe; reply "07 01" on this model
 *   08 00                  sent before (re)starting the capture streams
 *   0c 01                  power-up; reply "0c 01" = wait 1 s before use
 *   80 idx 08 ..           read 8 bytes of configuration memory
 *
 * The FPGA's capture block is itself an I2C-style target at address 0xf0
 * once the Capture bitstream is loaded (pinnacle_analog.c).
 */

#ifndef PINNACLE_CFG_H
#define PINNACLE_CFG_H

#include "pinnacle_device.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One request/reply exchange. reply may be NULL. */
pinnacle_status_t pinnacle_cfg_xfer(pinnacle_device_t *dev, const uint8_t *req, int req_len,
                                    uint8_t *reply, int reply_cap, int *reply_len);

/* Two-byte opcode request "op arg"; returns the reply's second byte in *result. */
pinnacle_status_t pinnacle_cfg_op(pinnacle_device_t *dev, uint8_t op, uint8_t arg,
                                  uint8_t *result);

pinnacle_status_t pinnacle_i2c_write(pinnacle_device_t *dev, uint8_t addr, uint8_t sub,
                                     uint8_t val);
pinnacle_status_t pinnacle_i2c_read(pinnacle_device_t *dev, uint8_t addr, uint8_t sub,
                                    uint8_t *val);

/* 03 addr, 04 addr. */
pinnacle_status_t pinnacle_cfg_chip_reset(pinnacle_device_t *dev, uint8_t addr);

/* Loads an FPGA bitstream: alt 0, "05 00" must answer ready, the bitstream
 * goes out on EP 0x02, the FPGA gets ~1 s to configure, then "06 00" must
 * answer up. The caller selects the alt setting the new design uses. Works
 * on a device that is already running another bitstream -- that is how the
 * vendor driver switches between DV (OHCI) and analog (Capture) without a
 * replug. */
pinnacle_status_t pinnacle_fpga_load(pinnacle_device_t *dev, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* PINNACLE_CFG_H */
