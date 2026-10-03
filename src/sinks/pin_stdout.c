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

#include "pin_stdout.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

#define WRITE_CHUNK (1u << 20)

static int g_fd = -1;

int pin_path_is_stdout(const char *path)
{
    return path && path[0] == '-' && path[1] == '\0';
}

void pin_stdout_set_fd(int fd)
{
    g_fd = fd;
}

void pin_stdout_prepare(void)
{
#if defined(_WIN32)
    int fd = g_fd >= 0 ? g_fd : _fileno(stdout);
    if (fd >= 0)
        _setmode(fd, _O_BINARY);
#else
    signal(SIGPIPE, SIG_IGN);
#endif
}

#if defined(_WIN32)
static HANDLE target_handle(void)
{
    /* The C runtime's fd 1 first: it follows a dup2() onto fd 1, which
     * GetStdHandle() does not necessarily do. */
    intptr_t h = _get_osfhandle(g_fd >= 0 ? g_fd : 1);
    if (h != -1 && h != -2)
        return (HANDLE)h;
    return g_fd >= 0 ? INVALID_HANDLE_VALUE : GetStdHandle(STD_OUTPUT_HANDLE);
}
#endif

int pin_stdout_check(char *why, size_t why_size)
{
    const char *msg = NULL;
#if defined(_WIN32)
    HANDLE h = target_handle();
    DWORD mode;
    if (!h || h == INVALID_HANDLE_VALUE)
        msg = "there is no standard output to write to";
    else if (GetFileType(h) == FILE_TYPE_CHAR && GetConsoleMode(h, &mode))
        msg = "refusing to write video to the terminal; pipe it into a program";
#else
    int fd = g_fd >= 0 ? g_fd : 1;
    if (isatty(fd))
        msg = "refusing to write video to the terminal; pipe it into a program";
    else if (fcntl(fd, F_GETFD) < 0)
        msg = "there is no standard output to write to";
#endif
    if (msg && why && why_size)
        snprintf(why, why_size, "%s", msg);
    return msg ? -1 : 0;
}

int pin_stdout_write(const void *data, size_t n)
{
    const uint8_t *p = data;
#if defined(_WIN32)
    HANDLE h = target_handle();
    if (!h || h == INVALID_HANDLE_VALUE)
        return PIN_STDOUT_ERROR;
    while (n > 0) {
        DWORD chunk = n > WRITE_CHUNK ? WRITE_CHUNK : (DWORD)n, done = 0;
        if (!WriteFile(h, p, chunk, &done, NULL)) {
            DWORD e = GetLastError();
            if (e == ERROR_BROKEN_PIPE || e == ERROR_NO_DATA || e == ERROR_PIPE_NOT_CONNECTED)
                return PIN_STDOUT_CLOSED;
            return PIN_STDOUT_ERROR;
        }
        if (done == 0)
            return PIN_STDOUT_ERROR;
        p += done;
        n -= done;
    }
#else
    int fd = g_fd >= 0 ? g_fd : 1;
    while (n > 0) {
        size_t chunk = n > WRITE_CHUNK ? WRITE_CHUNK : n;
        ssize_t w = write(fd, p, chunk);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return errno == EPIPE ? PIN_STDOUT_CLOSED : PIN_STDOUT_ERROR;
        }
        p += w;
        n -= (size_t)w;
    }
#endif
    return PIN_STDOUT_OK;
}
