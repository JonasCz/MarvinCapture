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
 * Raises the calling thread for time-critical USB work (the libusb event loop
 * that reaps and resubmits the capture transfers) and lowers it again.
 *
 *   Windows  MMCSS "Capture" class, else THREAD_PRIORITY_HIGHEST
 *   macOS    the user-interactive QoS class
 *   Linux    SCHED_FIFO 10 if RLIMIT_RTPRIO allows, else nice -10 if RLIMIT_NICE
 *            does; plus a PM QoS request of 0 us on /dev/cpu_dma_latency (when
 *            the node is writable) so the CPUs stay out of deep idle states
 *            for the duration; docs/analog.md, "Power saving and capture
 *            reliability".
 *
 * Failing any of it is not an error: the thread just stays as it was.
 */
#ifndef PIN_THREAD_BOOST_H
#define PIN_THREAD_BOOST_H

#include <pthread.h>
#if defined(__linux__)
#include <sched.h>
#endif

typedef struct {
    int raised;                 /* priority was raised */
#if defined(_WIN32)
    void *mmcss;                /* MMCSS handle, or NULL */
    int old_priority;
#elif defined(__linux__)
    int rt, niced;              /* which of SCHED_FIFO / nice took effect */
    int old_policy, old_nice;
    struct sched_param old_param;
    int pmqos_fd;               /* open /dev/cpu_dma_latency, or -1 */
#endif
} pin_thread_boost_t;

void pin_thread_boost(pin_thread_boost_t *b);
void pin_thread_unboost(pin_thread_boost_t *b);
/* "raised (SCHED_FIFO)", "raised (nice -10)", "raised" or "normal" */
const char *pin_thread_boost_desc(const pin_thread_boost_t *b);

#endif
