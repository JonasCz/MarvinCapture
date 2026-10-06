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

#include "pin_host_perf.h"

#include <string.h>

#if defined(__linux__)
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#include <powrprof.h>
#endif

#if defined(_WIN32)
/* The built-in plans (GUIDs of GUID_MAX_POWER_SAVINGS, GUID_MIN_POWER_SAVINGS,
 * GUID_TYPICAL_POWER_SAVINGS and the Ultimate Performance plan). A custom plan is the user's
 * own business and is never flagged. */
static const GUID PLAN_POWER_SAVER = { 0xa1841308, 0x3541, 0x4fab, { 0xbc, 0x81, 0xf7, 0x15, 0x56, 0xf2, 0x0b, 0x4a } };
static const GUID PLAN_BALANCED = { 0x381b4222, 0xf694, 0x41f0, { 0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e } };
#endif

int pin_host_perf_check(void)
{
    int mask = 0;
#if defined(__linux__)
    if (access("/dev/cpu_dma_latency", W_OK) != 0)
        mask |= PIN_HOST_PERF_CPU_LATENCY;
#elif defined(_WIN32)
    GUID *active = NULL;
    if (PowerGetActiveScheme(NULL, &active) == ERROR_SUCCESS && active) {
        if (IsEqualGUID(active, &PLAN_POWER_SAVER) || IsEqualGUID(active, &PLAN_BALANCED))
            mask |= PIN_HOST_PERF_POWER_PLAN;
        LocalFree(active);
    }
#endif
    return mask;
}

const char *pin_host_perf_text(int kind)
{
    switch (kind) {
    case PIN_HOST_PERF_CPU_LATENCY:
        return "Could not stop the CPU from going into deep sleep states while capturing "
               "(/dev/cpu_dma_latency is not writable by this user). On an otherwise idle "
               "machine the wake-ups can make the capture lose frames, so MarvinCapture keeps "
               "one CPU core busy at idle priority instead. To fix this, and let your CPU run "
               "quieter and cooler while capturing, allow it with a udev rule:";
    case PIN_HOST_PERF_POWER_PLAN:
        return "The Windows power plan is not set to \"High performance\", which is "
               "recommended for the most reliable capture: with power saving, deep CPU sleep "
               "states can occasionally make the capture lose frames. To fix this, set your "
               "power plan to \"High performance\" (Control Panel, Power Options) while capturing.";
    }
    return "";
}

const char *pin_host_perf_fix(int kind)
{
    switch (kind) {
    case PIN_HOST_PERF_CPU_LATENCY:
        return "echo 'KERNEL==\"cpu_dma_latency\", MODE=\"0666\"' | "
               "sudo tee /etc/udev/rules.d/99-cpu-dma-latency.rules\n"
               "sudo udevadm control --reload && sudo udevadm trigger --name-match=cpu_dma_latency";
    }
    return "";
}
