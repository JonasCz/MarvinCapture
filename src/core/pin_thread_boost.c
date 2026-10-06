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

#include "pin_thread_boost.h"

#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <avrt.h>
#elif defined(__APPLE__)
#include <pthread/qos.h>
#elif defined(__linux__)
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

void pin_thread_boost(pin_thread_boost_t *b)
{
    memset(b, 0, sizeof(*b));
#if defined(_WIN32)
    DWORD task = 0;
    HANDLE h = AvSetMmThreadCharacteristicsW(L"Capture", &task);
    b->mmcss = h;
    if (h) {
        b->raised = 1;
        return;
    }
    b->old_priority = GetThreadPriority(GetCurrentThread());
    b->raised = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) != 0;
#elif defined(__APPLE__)
    /* The closest thing to MMCSS: the top QoS class (user-interactive). */
    b->raised = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0;
#elif defined(__linux__)
    /* Idle states: on a quiet machine, wake-ups from deep package C-states stall the xHCI for
     * 15-35 ms now and then, which cuts frames short (any busy core stopped it). A PM QoS
     * request of 0 us keeps the CPUs out of them for as long as the descriptor stays open.
     * /dev/cpu_dma_latency is root-only by default, so this works only where a udev rule or
     * the user's privileges allow it. */
    b->pmqos_fd = open("/dev/cpu_dma_latency", O_WRONLY);
    if (b->pmqos_fd >= 0) {
        int32_t zero = 0;
        if (write(b->pmqos_fd, &zero, sizeof(zero)) != (ssize_t)sizeof(zero)) {
            close(b->pmqos_fd);
            b->pmqos_fd = -1;
        }
    }
    /* Real-time FIFO if RLIMIT_RTPRIO / CAP_SYS_NICE allow it (a low priority is enough: the
     * thread sleeps in poll() between completions); otherwise a negative nice value, which
     * needs RLIMIT_NICE. Without either the thread stays at normal priority. */
    struct sched_param sp = { .sched_priority = 10 };
    pthread_t self = pthread_self();
    if (pthread_getschedparam(self, &b->old_policy, &b->old_param) == 0 &&
        pthread_setschedparam(self, SCHED_FIFO, &sp) == 0) {
        b->rt = b->raised = 1;
        return;
    }
    pid_t tid = (pid_t)syscall(SYS_gettid);
    errno = 0;
    int old = getpriority(PRIO_PROCESS, (id_t)tid);
    if (errno == 0) {
        b->old_nice = old;
        if (setpriority(PRIO_PROCESS, (id_t)tid, -10) == 0)
            b->niced = b->raised = 1;
    }
#endif
}

void pin_thread_unboost(pin_thread_boost_t *b)
{
#if defined(_WIN32)
    if (b->mmcss)
        AvRevertMmThreadCharacteristics((HANDLE)b->mmcss);
    else if (b->raised)
        SetThreadPriority(GetCurrentThread(), b->old_priority);
#elif defined(__linux__)
    if (b->rt)
        pthread_setschedparam(pthread_self(), b->old_policy, &b->old_param);
    else if (b->niced)
        setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), b->old_nice);
    if (b->pmqos_fd >= 0)
        close(b->pmqos_fd);
    b->pmqos_fd = -1;
#endif
    b->raised = 0;
}

const char *pin_thread_boost_desc(const pin_thread_boost_t *b)
{
#if defined(__linux__)
    if (b->rt)
        return "raised (SCHED_FIFO)";
    if (b->niced)
        return "raised (nice -10)";
    return "normal";
#else
    return b->raised ? "raised" : "normal";
#endif
}
