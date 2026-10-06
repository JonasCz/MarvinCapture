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
 * Host performance recommendations: settings of the computer (not of a device)
 * that make analog capture lose frames, and the text every front end shows for
 * them. Static strings, so the CLI and the GUIs share one wording.
 */
#ifndef PIN_HOST_PERF_H
#define PIN_HOST_PERF_H

#ifdef __cplusplus
extern "C" {
#endif

#define PIN_HOST_PERF_CPU_LATENCY 0x1  /* Linux: /dev/cpu_dma_latency is not writable */
#define PIN_HOST_PERF_POWER_PLAN  0x2  /* Windows: power plan saves power at the cost of latency */

/* Bitmask of the recommendations that apply to this computer right now. */
int pin_host_perf_check(void);

/* The message for one kind (a single PIN_HOST_PERF_* bit). */
const char *pin_host_perf_text(int kind);

/* Commands that fix it, one per line, for the CLI to print; "" if there are none (Windows:
 * the GUI opens the Control Panel instead). */
const char *pin_host_perf_fix(int kind);

#ifdef __cplusplus
}
#endif

#endif
