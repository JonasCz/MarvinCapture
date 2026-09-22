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
 * captured stream-start command sequence, and writes reassembled raw DV to
 * a file until Ctrl+C, at which point it replays the stream-stop sequence
 * and exits cleanly.
 */

#include "pinnacle_device.h"
#include "pinnacle_stream.h"
#include "dv_reassembler.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

typedef struct {
    dv_reassembler_t *reasm;
    unsigned long total_bytes;
    time_t last_report;
    time_t deadline;      /* 0 = run until Ctrl+C */
} capture_ctx_t;

static int on_data(const uint8_t *data, size_t len, void *user)
{
    capture_ctx_t *ctx = user;
    ctx->total_bytes += len;

    if (dv_reassembler_feed(ctx->reasm, data, len) != 0) {
        fprintf(stderr, "pincli: write error, stopping\n");
        return -1;
    }

    time_t now = time(NULL);
    if (ctx->deadline && now >= ctx->deadline)
        g_stop = 1;
    if (now != ctx->last_report) {
        ctx->last_report = now;
        fprintf(stderr,
                "\rcaptured %.1f MB, %lu frames, %lu sequences (%lu dropped)   ",
                ctx->total_bytes / (1024.0 * 1024.0),
                ctx->reasm->frames_written,
                ctx->reasm->sequences_written,
                ctx->reasm->sequences_dropped);
        fflush(stderr);
    }

    return g_stop ? -1 : 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s -o <output.dv> -b <bitstream.bin>\n"
            "\n"
            "  -o, --output <file>      raw DV output path (required)\n"
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
    fprintf(stderr, "pincli: device opened\n");

    status = pinnacle_init_hardware(&dev, bitstream_path);
    if (status != PINNACLE_OK) {
        fprintf(stderr, "pincli: init failed: %s\n", pinnacle_strerror(status));
        pinnacle_close(&dev);
        return 1;
    }
    fprintf(stderr, "pincli: FPGA bitstream uploaded, alt setting 1 selected\n");

    FILE *out = fopen(output_path, "wb");
    if (!out) {
        fprintf(stderr, "pincli: cannot open output '%s': %s\n", output_path, strerror(errno));
        pinnacle_close(&dev);
        return 1;
    }

    dv_reassembler_t reasm;
    if (dv_reassembler_init(&reasm, out) != 0) {
        fprintf(stderr, "pincli: failed to initialise DV reassembler\n");
        fclose(out);
        pinnacle_close(&dev);
        return 1;
    }

    status = pinnacle_stream_start(&dev);
    if (status != PINNACLE_OK) {
        fprintf(stderr, "pincli: stream start failed: %s\n", pinnacle_strerror(status));
        dv_reassembler_finish(&reasm);
        fclose(out);
        pinnacle_close(&dev);
        return 1;
    }
    fprintf(stderr, "pincli: streaming started, writing to '%s' (Ctrl+C to stop)\n", output_path);

    capture_ctx_t ctx = { .reasm = &reasm, .total_bytes = 0, .last_report = 0,
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

    dv_reassembler_finish(&reasm);
    fclose(out);
    pinnacle_close(&dev);

    fprintf(stderr,
            "pincli: done. %lu bytes captured, %lu frames, %lu sequences written, %lu zero-padded\n",
            ctx.total_bytes, reasm.frames_written, reasm.sequences_written, reasm.sequences_dropped);
    fprintf(stderr,
            "pincli: framing: %lu isoch packets (%lu empty), %lu DIF bytes, "
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
