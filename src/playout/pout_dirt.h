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
 * Test seam POUT_TEST_DIRT: a dirty tape, for testing a capture's retry of bad sections
 * (pin_retry.h) against MarvinPlayout's virtual deck. The value is a comma-separated list of
 * FROM-TO[:READS] tape frame ranges (frame numbers from 0, inclusive): each frame in a range is
 * sent damaged on its first READS reads (default 1; 0 = on every read, damage that stays), then
 * clean, as dirt on a head or tape that a second pass wipes off. DV frames get STA 7 in every
 * fifth video block, HDV packets the transport_error_indicator. Example:
 *   POUT_TEST_DIRT=1500-1510,4000-4003:2,9000-9001:0 MarvinPlayout tape.dv
 */

#ifndef POUT_DIRT_H
#define POUT_DIRT_H

#include <stddef.h>
#include <stdint.h>

/* Damages the frame just read from the tape (in place) if the seam says so. */
void pout_dirt_apply(int64_t frame, uint8_t *buf, size_t len);

#endif /* POUT_DIRT_H */
