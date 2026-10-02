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
 * Model table: one row per Marvin-family unit (USB VID 0x2304), the single
 * place that says which PIDs exist, what they are called, whether this
 * driver drives them, which FPGA bitstreams they take and how they differ
 * in behaviour. Nothing else in the core may hard-code a PID.
 *
 * Where the rows come from: marvinavs64.inf (docs/analog.md, "Models") and
 * the PID branches in the vendor driver (docs/hardware.md, "Models").
 */

#ifndef PINNACLE_MODEL_H
#define PINNACLE_MODEL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t pid;               /* under VID 0x2304 */
    const char *name;           /* "Pinnacle Studio 500-USB" */
    int supported;              /* 1 = the core opens and drives it */
    int tested;                 /* 1 = verified end to end on real hardware */
    const char *dv_bitstream;   /* file name in firmware/: DV/HDV (OHCI) design */
    const char *analog_bitstream; /* file name in firmware/: analog capture design */
    uint8_t decoder_i2c;        /* 7-bit I2C address of the SAA7113-class video decoder */
    /* 1 = "Marvin-CR" firmware family (500/510/700/710): the config channel
     * has the 0c power-up and 80 <idx> 08 configuration-memory reads. 0 =
     * the older "classic" firmware (MovieBox Deluxe), which has neither: the
     * vendor driver skips both and reads the identity with FX2 vendor
     * request 0xA0 instead (pinnacle_device.c). */
    int cr_config;
    /* File name in firmware/ of the FX2 (USB controller) image the host must
     * download when the unit boots without firmware (probe "07 00" not
     * answered "07 01"), or NULL for the models that boot from EEPROM
     * (pinnacle_ensure_fx2, pinnacle_fx2.h). */
    const char *fx2_firmware;
} pinnacle_model_t;

extern const pinnacle_model_t pinnacle_model_table[];
extern const int pinnacle_model_table_count;

/* Looks up a PID (under VID 0x2304). NULL if it is not a known Marvin model. */
const pinnacle_model_t *pinnacle_model_lookup(uint16_t pid);

#ifdef __cplusplus
}
#endif

#endif /* PINNACLE_MODEL_H */
