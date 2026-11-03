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
#include <stdlib.h>

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

#ifdef __linux__
#ifndef SCHED_IDLE
#define SCHED_IDLE 5 /* only declared with _GNU_SOURCE */
#endif
#endif

static volatile int g_keep_busy = PIN_THREAD_KEEP_BUSY_AUTO;

void pin_thread_set_keep_busy(int mode)
{
    if (mode == PIN_THREAD_KEEP_BUSY_AUTO || mode == PIN_THREAD_KEEP_BUSY_ON ||
        mode == PIN_THREAD_KEEP_BUSY_OFF)
        g_keep_busy = mode;
}

int pin_thread_get_keep_busy(void)
{
    return g_keep_busy;
}

#if defined(__linux__)
/* Keeps one core out of the idle states when /dev/cpu_dma_latency is not available (a busy
 * core is enough: every wake-up the xHCI needs is then served by a running CPU or a shallow
 * one). SCHED_IDLE, so it only ever uses time nothing else wants. */
static void *busy_main(void *arg)
{
    pin_thread_boost_t *b = arg;
    struct sched_param sp = { .sched_priority = 0 };
    pthread_setschedparam(pthread_self(), SCHED_IDLE, &sp);
    while (!b->busy_stop)
        for (int i = 0; i < 100000; i++)
            __asm__ __volatile__("" ::: "memory");
    return NULL;
}
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
    int keep_busy = g_keep_busy;
#if defined(__ANDROID__)
    /* An app cannot open it (SELinux; Termux without root neither), so AUTO means the
     * keep-busy thread there. Not even tried: every attempt would log an SELinux denial. */
    b->pmqos_fd = -1;
    if (keep_busy == PIN_THREAD_KEEP_BUSY_AUTO)
        keep_busy = PIN_THREAD_KEEP_BUSY_ON;
#else
    b->pmqos_fd = open("/dev/cpu_dma_latency", O_WRONLY);
    if (b->pmqos_fd >= 0) {
        int32_t zero = 0;
        if (write(b->pmqos_fd, &zero, sizeof(zero)) != (ssize_t)sizeof(zero)) {
            close(b->pmqos_fd);
            b->pmqos_fd = -1;
        }
    }
#endif
    /* AUTO: the busy core is only the fallback for a refused latency request. ON runs it
     * even next to a granted one (for a machine where the request alone is not enough);
     * OFF never (saves the power and heat of a busy core, at the risk of lost frames). */
    int want_busy = keep_busy == PIN_THREAD_KEEP_BUSY_ON ||
                    (keep_busy == PIN_THREAD_KEEP_BUSY_AUTO && b->pmqos_fd < 0);
    if (want_busy && pthread_create(&b->busy_thread, NULL, busy_main, b) == 0)
        b->busy = 1;
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
    if (b->busy) {
        b->busy_stop = 1;
        pthread_join(b->busy_thread, NULL);
        b->busy = 0;
    }
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

const char *pin_thread_boost_latency_desc(const pin_thread_boost_t *b)
{
#if defined(__linux__)
    if (b->pmqos_fd >= 0)
        return b->busy ? "set, and keeping a core busy as well (keep-busy on)" : "set";
    if (b->busy)
        return "not available, keeping a core busy instead";
    return g_keep_busy == PIN_THREAD_KEEP_BUSY_OFF ? "not available (keep-busy off)" : "not available";
#else
    (void)b;
    return "not used on this platform";
#endif
}
