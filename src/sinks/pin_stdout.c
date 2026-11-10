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

#include <limits.h>
#include <stdatomic.h>
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
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define WRITE_CHUNK (1u << 20)

static int g_fd = -1;
/* pin_stdout_cancel(): the write in progress gives up. There is one standard output per process,
 * so one flag; pin_stdout_prepare() clears it for the next capture. */
static atomic_int g_cancel;
#if defined(_WIN32)
static atomic_ulong g_writer_tid;   /* the thread inside WriteFile, 0 none */
#endif

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
    atomic_store(&g_cancel, 0);
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

void pin_stdout_cancel(void)
{
    atomic_store(&g_cancel, 1);
#if defined(_WIN32)
    /* An anonymous pipe has no overlapped I/O: the blocked WriteFile is cancelled from here. The
     * writer may be just about to call it, so the cancel is repeated for a short while. */
    for (int i = 0; i < 20; i++) {
        DWORD tid = (DWORD)atomic_load(&g_writer_tid);
        if (!tid)
            break;
        HANDLE t = OpenThread(THREAD_TERMINATE, FALSE, tid);
        if (t) {
            CancelSynchronousIo(t);
            CloseHandle(t);
        }
        Sleep(5);
    }
#endif
}

int pin_stdout_write(const void *data, size_t n)
{
    const uint8_t *p = data;
#if defined(_WIN32)
    HANDLE h = target_handle();
    if (!h || h == INVALID_HANDLE_VALUE)
        return PIN_STDOUT_ERROR;
    while (n > 0) {
        if (atomic_load(&g_cancel))
            return PIN_STDOUT_ERROR;
        DWORD chunk = n > WRITE_CHUNK ? WRITE_CHUNK : (DWORD)n, done = 0;
        atomic_store(&g_writer_tid, (unsigned long)GetCurrentThreadId());
        BOOL ok = WriteFile(h, p, chunk, &done, NULL);
        DWORD e = ok ? 0 : GetLastError();
        atomic_store(&g_writer_tid, 0);
        if (!ok) {
            if (e == ERROR_BROKEN_PIPE || e == ERROR_NO_DATA || e == ERROR_PIPE_NOT_CONNECTED)
                return PIN_STDOUT_CLOSED;
            return PIN_STDOUT_ERROR;   /* ERROR_OPERATION_ABORTED: pin_stdout_cancel() */
        }
        if (done == 0)
            return PIN_STDOUT_ERROR;
        p += done;
        n -= done;
    }
#else
    int fd = g_fd >= 0 ? g_fd : 1;
    /* A pipe or socket: a blocking write of more than the free space waits until the reader has
     * taken it all, and nothing interrupts it. So the descriptor is made non-blocking for the
     * call: write() then takes as much as fits (a partial write), and when nothing fits poll()
     * waits for room, checking for a cancel every 100 ms. The chunk is not limited to PIPE_BUF
     * (512 bytes on macOS), which would cost tens of thousands of system calls a second. */
    struct stat sb;
    int pipe_like = fstat(fd, &sb) == 0 && (S_ISFIFO(sb.st_mode) || S_ISSOCK(sb.st_mode));
    int old_flags = -1;
    if (pipe_like) {
        int fl = fcntl(fd, F_GETFL);
        if (fl >= 0 && !(fl & O_NONBLOCK) && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0)
            old_flags = fl;   /* restore it on the way out */
    }
    int result = PIN_STDOUT_OK;
    while (n > 0) {
        if (atomic_load(&g_cancel)) {
            result = PIN_STDOUT_ERROR;
            break;
        }
        size_t chunk = n > WRITE_CHUNK ? WRITE_CHUNK : n;
        ssize_t w = write(fd, p, chunk);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* full: wait until the reader has drained some. The timeout only bounds each
                 * wait so a cancel is seen; the loop goes on until the data is written or the
                 * pipe fails. */
                struct pollfd pfd = { .fd = fd, .events = POLLOUT };
                int pr = poll(&pfd, 1, 100);
                if (pr < 0 && errno != EINTR) {
                    result = PIN_STDOUT_ERROR;
                    break;
                }
                continue;
            }
            result = errno == EPIPE ? PIN_STDOUT_CLOSED : PIN_STDOUT_ERROR;
            break;
        }
        p += w;
        n -= (size_t)w;
    }
    if (old_flags >= 0)
        fcntl(fd, F_SETFL, old_flags);
    return result;
#endif
    return PIN_STDOUT_OK;
}
