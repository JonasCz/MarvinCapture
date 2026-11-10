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
 * Test seam, like PIN_REPLAY_UNPLUG_AFTER: PIN_TEST_WRITE_FAIL_AFTER=<bytes> makes every output
 * file of the raw sink (.dv, .m2t, the HDV remux's temp .ts) and of the DV rewrap sink (DV AVI,
 * DV MOV) take that many bytes and then refuse the rest, as a full drive (ENOSPC) or FAT32's
 * file size limit would: the write that crosses it is cut short there, every later one fails,
 * the container's index or trailer included. Read when a file is opened; nothing changes
 * unless it is set (the files are then written exactly as without this file).
 */

#define _FILE_OFFSET_BITS 64

#include "sinks_internal.h"
#include "../core/pin_fs.h"

#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
#endif

/* PIN_TEST_WRITE_STALL=<bytes>:<ms> (sinks_internal.h); 0 when unset */
static void stall_seam(uint64_t *after, long *ms)
{
    const char *v = getenv("PIN_TEST_WRITE_STALL");
    char *end = NULL;
    *after = v && v[0] ? (uint64_t)strtoull(v, &end, 10) : 0;
    *ms = end && *end == ':' ? strtol(end + 1, NULL, 10) : 0;
}

uint64_t pin_sink_fault_budget(void)
{
    const char *v = getenv("PIN_TEST_WRITE_FAIL_AFTER");
    unsigned long long n = v && v[0] ? strtoull(v, NULL, 10) : 0;
    if (n)
        return (uint64_t)n;
    uint64_t after;
    long ms;
    stall_seam(&after, &ms);
    return ms > 0 ? UINT64_MAX - 1 : UINT64_MAX;   /* the stall needs the seam's own output */
}

long pin_sink_fault_stall_timeout_ms(long dflt_ms)
{
    const char *v = getenv("PIN_TEST_STALL_TIMEOUT_MS");
    long ms = v && v[0] ? strtol(v, NULL, 10) : 0;
    return ms > 0 ? ms : dflt_ms;
}

size_t pin_sink_fault_fwrite(const void *data, size_t len, FILE *f, uint64_t *left)
{
    if (*left == UINT64_MAX)
        return fwrite(data, 1, len, f);
    size_t n = len < *left ? len : (size_t)*left;
    size_t done = n ? fwrite(data, 1, n, f) : 0;
    *left -= done;
    if (done == n && n < len)
        errno = ENOSPC;
    return done;
}

/* The custom output of pin_sink_fault_avio_open(): a stdio file with a byte budget. */
typedef struct {
    FILE *f;
    uint64_t left;
    uint64_t written;
    uint64_t stall_after;   /* PIN_TEST_WRITE_STALL */
    long stall_ms;
} fault_file_t;

static void sleep_ms(long ms)
{
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
#endif
}

static int fault_write(void *opaque, const uint8_t *buf, int size)
{
    fault_file_t *ff = opaque;
    ff->written += (uint64_t)size;
    if (ff->stall_ms > 0 && ff->written > ff->stall_after) {
        long ms = ff->stall_ms;
        ff->stall_ms = 0;
        sleep_ms(ms);
    }
    size_t done = pin_sink_fault_fwrite(buf, (size_t)size, ff->f, &ff->left);
    return done == (size_t)size ? size : AVERROR(ENOSPC);
}

static int64_t fault_seek(void *opaque, int64_t offset, int whence)
{
    fault_file_t *ff = opaque;
    whence &= ~AVSEEK_FORCE;
    if (whence == AVSEEK_SIZE) {
        off_t here = ftello(ff->f);
        if (fseeko(ff->f, 0, SEEK_END) != 0)
            return AVERROR(EIO);
        off_t size = ftello(ff->f);
        fseeko(ff->f, here, SEEK_SET);
        return (int64_t)size;
    }
    if (fseeko(ff->f, (off_t)offset, whence) != 0)
        return AVERROR(EIO);
    return (int64_t)ftello(ff->f);
}

int pin_sink_fault_avio_open(AVIOContext **pb, const char *path, uint64_t budget)
{
    fault_file_t *ff = calloc(1, sizeof(*ff));
    uint8_t *buf = av_malloc(32768);
    if (ff)
        ff->f = pin_fs_fopen(path, "wb");
    if (!ff || !ff->f || !buf) {
        if (ff && ff->f)
            fclose(ff->f);
        free(ff);
        av_free(buf);
        return AVERROR(EIO);
    }
    ff->left = budget;
    stall_seam(&ff->stall_after, &ff->stall_ms);
    *pb = avio_alloc_context(buf, 32768, 1, ff, NULL, fault_write, fault_seek);
    if (!*pb) {
        fclose(ff->f);
        free(ff);
        av_free(buf);
        return AVERROR(ENOMEM);
    }
    return 0;
}

int pin_sink_fault_avio_close(AVIOContext **pb)
{
    if (!pb || !*pb)
        return 0;
    fault_file_t *ff = (*pb)->opaque;
    avio_flush(*pb);
    int err = (*pb)->error;
    if (fclose(ff->f) != 0 && err >= 0)
        err = AVERROR(EIO);
    free(ff);
    av_freep(&(*pb)->buffer);
    avio_context_free(pb);
    return err < 0 ? err : 0;
}
