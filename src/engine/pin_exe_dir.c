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

#include "pin_exe_dir.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <limits.h>
#include <mach-o/dyld.h> /* _NSGetExecutablePath */
#include <unistd.h>
#else
#include <unistd.h>
#endif

#if defined(_WIN32)
int pin_module_dir(void *module, char *out, size_t cap)
{
    /* GetModuleFileNameW cuts the name to the buffer without saying so: grow until it fits */
    DWORD size = 512;
    wchar_t *wpath = NULL;
    DWORD n = 0;
    for (;;) {
        wchar_t *nw = realloc(wpath, (size_t)size * sizeof(wchar_t));
        if (!nw) {
            free(wpath);
            return -1;
        }
        wpath = nw;
        n = GetModuleFileNameW((HMODULE)module, wpath, size);
        if (n == 0) {
            free(wpath);
            return -1;
        }
        if (n < size)
            break;
        if (size >= 65536) {    /* far past any path Windows can have */
            free(wpath);
            return -1;
        }
        size *= 2;
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, wpath, (int)n, NULL, 0, NULL, NULL);
    char *path = len > 0 ? malloc((size_t)len + 1) : NULL;
    if (path)
        len = WideCharToMultiByte(CP_UTF8, 0, wpath, (int)n, path, len, NULL, NULL);
    free(wpath);
    if (!path || len <= 0) {
        free(path);
        return -1;
    }
    path[len] = 0;
    char *slash = strrchr(path, '\\');
    char *slash2 = strrchr(path, '/');
    if (slash2 && (!slash || slash2 > slash))
        slash = slash2;
    int rc = -1;
    if (slash && (size_t)(slash - path) + 1 <= cap) {
        size_t n2 = (size_t)(slash - path);
        memcpy(out, path, n2);
        out[n2] = 0;
        rc = 0;
    }
    free(path);
    return rc;
}
#endif

int pin_exe_dir(char *out, size_t cap)
{
#if defined(_WIN32)
    return pin_module_dir(NULL, out, cap);
#else
    char path[4096];
#if defined(__APPLE__)
    char raw[4096];
    uint32_t raw_size = sizeof(raw);
    if (_NSGetExecutablePath(raw, &raw_size) != 0 || !realpath(raw, path))
        return -1;
#else
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0)
        return -1;
    path[n] = 0;
#endif
    char *slash = strrchr(path, '/');
    if (!slash)
        return -1;
    size_t n2 = (size_t)(slash - path);
    if (n2 + 1 > cap)
        return -1;
    memcpy(out, path, n2);
    out[n2] = 0;
    return 0;
#endif
}
