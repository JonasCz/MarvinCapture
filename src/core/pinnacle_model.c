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

#include "pinnacle_model.h"

#include <stddef.h>

/* The 510-USB (0223) takes exactly the 500-USB's bitstreams: every PID
 * branch in the vendor driver treats 0x223 like 0x213 (docs/hardware.md). */
const pinnacle_model_t pinnacle_model_table[] = {
    { 0x0213, "Pinnacle Studio 500-USB", 1, 1, "fpga-ohci.bin", "fpga-capture.bin", 0x4a }, /* Marvin-Lite */
    { 0x0223, "Pinnacle Studio 510-USB", 1, 0, "fpga-ohci.bin", "fpga-capture.bin", 0x4a }, /* Marvin-Lite rev. 2 */
    { 0x0206, "Pinnacle MovieBox Deluxe", 0, 0, NULL, NULL, 0x48 },                      /* Marvin-classic */
    { 0x0212, "Pinnacle Studio 700-USB", 0, 0, NULL, NULL, 0x4a },                       /* Marvin-CR */
    { 0x0224, "Pinnacle Studio 710-USB", 0, 0, NULL, NULL, 0x4a },                       /* Marvin-CR */
};
const int pinnacle_model_table_count =
    (int)(sizeof(pinnacle_model_table) / sizeof(pinnacle_model_table[0]));

const pinnacle_model_t *pinnacle_model_lookup(uint16_t pid)
{
    for (int i = 0; i < pinnacle_model_table_count; i++) {
        if (pinnacle_model_table[i].pid == pid)
            return &pinnacle_model_table[i];
    }
    return NULL;
}
