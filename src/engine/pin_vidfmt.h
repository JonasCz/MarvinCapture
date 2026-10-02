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
 * Video format helpers shared by every GUI: the MPEG-2 frame_rate_code of an
 * HDV sequence header as a rational, and the short label shown in the status
 * bar ("PAL", "NTSC", "1080i25", "720p59.94").
 */

#ifndef PIN_VIDFMT_H
#define PIN_VIDFMT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* frame_rate_code 1..8 (23.976, 24, 25, 29.97, 30, 50, 59.94, 60) -> num/den.
 * Returns 0 for a reserved / unknown code (num = den = 0). */
int pin_vidfmt_rate_from_code(int code, int *num, int *den);

/* HDV: 720 lines are progressive (720p), 1080 lines interlaced (1080i). */
int pin_vidfmt_hdv_interlaced(int height);

/* Short label. 576 lines at 25 fps = "PAL", 480 lines at 29.97 = "NTSC",
 * otherwise "<height><i|p><frame rate>" with the frame (not field) rate, e.g.
 * "1080i25", "1080i29.97", "720p59.94". "" if height or rate is unknown. */
void pin_vidfmt_label(int height, int fps_num, int fps_den, int interlaced,
                      char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* PIN_VIDFMT_H */
