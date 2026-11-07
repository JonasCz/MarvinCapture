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
 * MarvinPlayout on the device: opens the 1394 link, makes the pout_session loop run on the wall
 * clock with the IT stream as its output, and answers AV/C commands with the virtual deck.
 * Needs a device; nothing here has run against one (see "GUESS:" in pout_hw_link.c and
 * docs/playout.md for the test plan).
 */

#ifndef POUT_HW_SESSION_H
#define POUT_HW_SESSION_H

#include "pout_script.h"
#include "pout_tape.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opens the device named by sc (or the first one), plays the script and/or serves the virtual deck
 * until the script is over or interrupted() returns nonzero. Returns the exit code (POUT_EXIT_*),
 * with a one-line message in err when it is not 0 or 130. Status lines go to stderr. */
int pout_hw_run(const pout_script_t *sc, pout_tape_t *tape, int (*interrupted)(void), char *err, size_t err_cap);

/* The FPGA design for the DV/HDV link (fpga-ohci.bin), looked for in the settings key
 * Paths/firmware_dir, then firmware/ next to the executable (the build tree and the dist directory
 * have it there), then ./firmware. 0 and path filled, or -1 with the directories tried in why. */
int pout_hw_find_bitstream(char *path, size_t cap, char *why, size_t why_cap);

#ifdef __cplusplus
}
#endif

#endif /* POUT_HW_SESSION_H */
