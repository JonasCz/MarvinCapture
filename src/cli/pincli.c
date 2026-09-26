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
 * pincli — minimal capture CLI for the Pinnacle 500-USB.
 *
 * Initialises the device (FPGA bitstream + alt setting), replays the
 * captured stream-start command sequence, and writes the reassembled stream
 * to a file until Ctrl+C, at which point it replays the stream-stop sequence
 * and exits cleanly. A DV camera yields raw DV; an HDV camera yields an
 * MPEG-2 transport stream. The format is detected from the stream itself.
 */

#include "pinnacle_device.h"
#include "pinnacle_enum.h"
#include "pinnacle_lock.h"
#include "pinnacle_stream.h"
#include "dv_reassembler.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
/* winpthreads gives MinGW/UCRT64 the same pthreads as Linux, so the writer
 * thread below no longer needs a separate _WIN32 fork. alarm()/SIGALRM
 * genuinely don't exist on Windows and stay guarded, further down. */
#include <pthread.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* Every exit path past a successful pinnacle_lock_acquire() needs both the
 * lock and the device released, in that order (see pinnacle_close(),
 * which -- unlike pinnacle_lock_release() -- is also safe to call again on
 * an already-closed dev, so ordering here only matters for the lock). */
static void release_and_close(pinnacle_lock_t *lock, pinnacle_device_t *dev)
{
    pinnacle_lock_release(lock);
    pinnacle_close(dev);
}

/*
 * The USB thread must never wait on the disk. A queue of EP 0x88 transfers is
 * only ~300 ms deep; a filesystem flush that blocks fwrite() for longer than
 * the queue can absorb makes the FPGA's receive FIFO overrun and drop bus
 * cycles (seen as a hole in the stream ~5 s into a run, when the kernel's
 * writeback first fires). So the USB thread only copies into a large buffer,
 * and a second thread runs the reassembler and writes the file.
 */
#define SINK_BYTES (64u << 20)

typedef struct {
    dv_reassembler_t *reasm;
    time_t last_report;
    uint8_t *buf;
    size_t head, tail;              /* monotonic byte counters into buf */
    size_t max_fill;
    int closed, failed, overflowed;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    pthread_t thread;
    int started;
} sink_t;

static void sink_progress(sink_t *s)
{
    time_t now = time(NULL);
    if (now == s->last_report)
        return;
    s->last_report = now;
    const dv_reassembler_t *r = s->reasm;
    if (r->format == DV_FORMAT_HDV)
        fprintf(stderr, "\rcaptured %.1f MB, HDV: %lu TS packets (%lu cc errors)   ",
                r->bytes_fed / (1024.0 * 1024.0), r->ts_packets, r->ts_cc_errors);
    else
        fprintf(stderr, "\rcaptured %.1f MB, %lu frames, %lu sequences (%lu dropped)   ",
                r->bytes_fed / (1024.0 * 1024.0), r->frames_written,
                r->sequences_written, r->sequences_dropped);
    fflush(stderr);
}

static void *sink_thread(void *arg)
{
    sink_t *s = arg;

    pthread_mutex_lock(&s->lock);
    for (;;) {
        while (s->head == s->tail && !s->closed)
            pthread_cond_wait(&s->wake, &s->lock);
        if (s->head == s->tail)
            break;                       /* closed and fully drained */

        size_t off = s->tail % SINK_BYTES;
        size_t n = s->head - s->tail;
        if (n > SINK_BYTES - off)
            n = SINK_BYTES - off;
        pthread_mutex_unlock(&s->lock);

        /* Bytes in [tail, head) are not touched by the producer, so no lock
         * is held while this (possibly slow) write runs. */
        int rc = dv_reassembler_feed(s->reasm, s->buf + off, n);
        sink_progress(s);

        pthread_mutex_lock(&s->lock);
        s->tail += n;
        if (rc != 0) {
            s->failed = 1;
            break;
        }
    }
    pthread_mutex_unlock(&s->lock);
    return NULL;
}

static int sink_start(sink_t *s, dv_reassembler_t *reasm)
{
    memset(s, 0, sizeof(*s));
    s->reasm = reasm;
    s->buf = malloc(SINK_BYTES);
    if (!s->buf)
        return -1;
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->wake, NULL);
    if (pthread_create(&s->thread, NULL, sink_thread, s) != 0) {
        free(s->buf);
        return -1;
    }
    s->started = 1;
    return 0;
}

static int sink_push(sink_t *s, const uint8_t *data, size_t len)
{
    pthread_mutex_lock(&s->lock);
    if (s->failed) {
        pthread_mutex_unlock(&s->lock);
        return -1;
    }
    size_t fill = s->head - s->tail;
    if (len > SINK_BYTES - fill) {
        s->overflowed = 1;
        pthread_mutex_unlock(&s->lock);
        return -1;
    }
    size_t off = s->head % SINK_BYTES;
    size_t first = SINK_BYTES - off;
    if (first > len)
        first = len;
    memcpy(s->buf + off, data, first);
    memcpy(s->buf, data + first, len - first);
    s->head += len;
    if (s->head - s->tail > s->max_fill)
        s->max_fill = s->head - s->tail;
    pthread_cond_signal(&s->wake);
    pthread_mutex_unlock(&s->lock);
    return 0;
}

/* Lets the writer drain everything already queued, then joins it. */
static void sink_stop(sink_t *s)
{
    if (!s->started)
        return;
    pthread_mutex_lock(&s->lock);
    s->closed = 1;
    pthread_cond_signal(&s->wake);
    pthread_mutex_unlock(&s->lock);
    pthread_join(s->thread, NULL);
    s->started = 0;
    if (s->overflowed)
        fprintf(stderr,
                "pincli: WARNING: the writer fell more than %u MB behind the USB "
                "stream; capture was stopped (is the output disk stalled?)\n",
                SINK_BYTES >> 20);
    else if (s->failed)
        fprintf(stderr, "pincli: write error in the output writer\n");
    fprintf(stderr, "pincli: writer buffer high-water mark %.2f MB of %u MB\n",
            s->max_fill / (1024.0 * 1024.0), SINK_BYTES >> 20);
    free(s->buf);
}

typedef struct {
    sink_t *sink;
    unsigned long total_bytes;
    time_t deadline;      /* 0 = run until Ctrl+C */
} capture_ctx_t;

static int on_data(const uint8_t *data, size_t len, void *user)
{
    capture_ctx_t *ctx = user;
    ctx->total_bytes += len;

    if (sink_push(ctx->sink, data, len) != 0) {
        fprintf(stderr, "\npincli: output failed, stopping\n");
        return -1;
    }

    if (ctx->deadline && time(NULL) >= ctx->deadline)
        g_stop = 1;
    return g_stop ? -1 : 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s -o <output> -b <bitstream.bin>\n"
            "\n"
            "  -o, --output <file>      output path (required). Raw DV from a DV\n"
            "                           camera (use .dv), MPEG-2 transport stream\n"
            "                           from an HDV camera (use .ts); the format is\n"
            "                           detected from the stream.\n"
            "  -b, --bitstream <file>   FPGA bitstream blob, extracted from the\n"
            "                           user's own vendor driver install/capture\n"
            "                           (default: traces/fpga-bitstream-candidate.bin\n"
            "                           relative to the repo root; required if that's\n"
            "                           not found)\n"
            "  -t, --duration <secs>    stop after this many seconds of capture\n"
            "                           (default: until Ctrl+C). Takes the same\n"
            "                           shutdown path as Ctrl+C.\n"
            "\n"
            "Captures until Ctrl+C.\n",
            argv0);
}

int main(int argc, char **argv)
{
    const char *output_path = NULL;
    const char *bitstream_path = "traces/fpga-bitstream-candidate.bin";
    long duration_s = 0;

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) && i + 1 < argc) {
            output_path = argv[++i];
        } else if ((strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--bitstream") == 0) && i + 1 < argc) {
            bitstream_path = argv[++i];
        } else if ((strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--duration") == 0) && i + 1 < argc) {
            duration_s = strtol(argv[++i], NULL, 10);
            if (duration_s <= 0) {
                fprintf(stderr, "pincli: --duration must be a positive number of seconds\n");
                return 2;
            }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "pincli: unrecognised argument '%s'\n\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    if (!output_path) {
        fprintf(stderr, "pincli: -o/--output is required\n\n");
        usage(argv[0]);
        return 2;
    }

    signal(SIGINT, on_sigint);

    pinnacle_device_t dev;
    pinnacle_status_t status = pinnacle_open(&dev);
    if (status != PINNACLE_OK) {
        fprintf(stderr, "pincli: open failed: %s\n", pinnacle_strerror(status));
        return 1;
    }
    pinnacle_tuning_from_env(&dev.tuning);
    fprintf(stderr, "pincli: device opened\n");

    /* Cross-process ownership (pinnacle_lock.h): refuse to fight another
     * pincli/pindeck/pinanalog -- or a future pinctl -- over this device.
     * Starts in PREPARING, matching what we're about to do below. */
    char device_id[PINNACLE_ENUM_ID_MAX];
    pinnacle_enum_build_id(libusb_get_device(dev.handle), device_id, sizeof(device_id));
    pinnacle_lock_t *lock = NULL;
    status = pinnacle_lock_acquire(device_id, &lock);
    if (status != PINNACLE_OK) {
        fprintf(stderr, "pincli: %s\n", status == PINNACLE_ERR_BUSY
                ? "device is already in use by another process" : "failed to take the device lock");
        pinnacle_close(&dev);
        return 1;
    }

    status = pinnacle_init_hardware(&dev, bitstream_path);
    if (status != PINNACLE_OK) {
        fprintf(stderr, "pincli: init failed: %s\n", pinnacle_strerror(status));
        release_and_close(lock, &dev);
        return 1;
    }
    fprintf(stderr, "pincli: FPGA bitstream uploaded, alt setting 1 selected\n");
    pinnacle_lock_update(lock, PINNACLE_LOCK_READY, dev.guid_hi, dev.guid_lo);

    FILE *out = fopen(output_path, "wb");
    if (!out) {
        fprintf(stderr, "pincli: cannot open output '%s': %s\n", output_path, strerror(errno));
        release_and_close(lock, &dev);
        return 1;
    }

    /* The reassembler writes 188-byte TS packets / 12 KB DIF sequences; batch
     * them into multi-megabyte writes so the disk sees few, large requests. */
    setvbuf(out, NULL, _IOFBF, 4u << 20);

    dv_output_t file_sink;
    dv_output_file(&file_sink, out);

    dv_reassembler_t reasm;
    if (dv_reassembler_init(&reasm, &file_sink) != 0) {
        fprintf(stderr, "pincli: failed to initialise DV reassembler\n");
        fclose(out);
        release_and_close(lock, &dev);
        return 1;
    }

    sink_t sink;
    if (sink_start(&sink, &reasm) != 0) {
        fprintf(stderr, "pincli: failed to start the output writer\n");
        dv_reassembler_finish(&reasm);
        fclose(out);
        release_and_close(lock, &dev);
        return 1;
    }

    status = pinnacle_stream_start(&dev);
    if (status != PINNACLE_OK) {
        fprintf(stderr, "pincli: stream start failed: %s\n", pinnacle_strerror(status));
        sink_stop(&sink);
        dv_reassembler_finish(&reasm);
        fclose(out);
        release_and_close(lock, &dev);
        return 1;
    }
    pinnacle_lock_update(lock, PINNACLE_LOCK_CAPTURING, dev.guid_hi, dev.guid_lo);
    fprintf(stderr, "pincli: streaming started, writing to '%s' (Ctrl+C to stop)\n", output_path);

    capture_ctx_t ctx = { .sink = &sink, .total_bytes = 0,
                          .deadline = duration_s ? time(NULL) + duration_s : 0 };
#if !defined(_WIN32)
    /* Belt and braces for --duration: on_data only runs when bytes arrive, so
     * a stream that died mid-run would otherwise hang past the deadline. */
    if (duration_s) {
        signal(SIGALRM, on_sigint);
        alarm((unsigned)duration_s + 2);
    }
#endif
    status = pinnacle_stream_read_loop(&dev, on_data, &ctx, &g_stop);
    fprintf(stderr, "\n");
    if (status != PINNACLE_OK)
        fprintf(stderr, "pincli: capture loop ended with error: %s\n", pinnacle_strerror(status));

    pinnacle_status_t stop_status = pinnacle_stream_stop(&dev);
    if (stop_status != PINNACLE_OK)
        fprintf(stderr, "pincli: stream stop sequence failed: %s\n", pinnacle_strerror(stop_status));

    sink_stop(&sink);       /* drain the writer before finalising the file */
    dv_reassembler_finish(&reasm);
    fclose(out);
    release_and_close(lock, &dev);

    if (reasm.format == DV_FORMAT_HDV) {
        fprintf(stderr,
                "pincli: done. %lu bytes captured, HDV (MPEG2-TS): %lu TS packets "
                "written (%lu held back before the first GOP)\n",
                ctx.total_bytes, reasm.ts_packets, reasm.ts_discarded);
        fprintf(stderr,
                "pincli: TS integrity: %lu continuity-counter errors, %lu bad-sync "
                "packets (+%lu CC jumps explained by CIP discontinuities)\n",
                reasm.ts_cc_errors, reasm.ts_sync_errors, reasm.ts_cc_forgiven);
        if (!reasm.ts_gate_open)
            fprintf(stderr, "pincli: WARNING: no video GOP header seen; output is empty\n");
    } else {
        fprintf(stderr,
                "pincli: done. %lu bytes captured, %lu frames, %lu sequences written, %lu zero-padded\n",
                ctx.total_bytes, reasm.frames_written, reasm.sequences_written, reasm.sequences_dropped);
    }
    fprintf(stderr,
            "pincli: framing: %lu isoch packets (%lu empty), %lu payload bytes, "
            "%lu message resyncs, %lu isoch resyncs\n",
            reasm.iso_packets, reasm.iso_empty, reasm.dif_bytes,
            reasm.msg_resyncs, reasm.iso_resyncs);
    if (reasm.dbc_gaps == 0)
        fprintf(stderr,
                "pincli: continuity: OK — %lu data blocks, no CIP/DBC "
                "discontinuity (%lu at stream join, expected)\n",
                reasm.dbc_blocks, reasm.dbc_joins);
    else
        fprintf(stderr,
                "pincli: continuity: %lu discontinuities, ~%lu data blocks lost "
                "of %lu (%.4f%%)\n",
                reasm.dbc_gaps, reasm.dbc_lost_blocks, reasm.dbc_blocks,
                100.0 * reasm.dbc_lost_blocks /
                    (double)(reasm.dbc_blocks + reasm.dbc_lost_blocks));

    return (status == PINNACLE_OK) ? 0 : 1;
}
