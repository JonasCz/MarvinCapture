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
 * replay_reassembler — feeds a raw EP 0x88 dump through dv_reassembler in
 * fixed-size chunks, exactly like the USB read loop would (one callback per
 * completed transfer), and writes the reassembled output to a file.
 *
 * Used two ways:
 *   1. Phase 1 baseline: built against the pre-refactor dv_reassembler, its
 *      output is hashed and frozen as the "must stay byte-identical" target.
 *   2. ctest regression: built against the refactored (callback-based)
 *      dv_reassembler, its output is hashed again and compared to the frozen
 *      baseline.
 *
 * usage: replay_reassembler <input.bin> <output.raw>
 */

#include "dv_reassembler.h"

#include <stdio.h>
#include <stdlib.h>

#define CHUNK_BYTES 16384

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <input.bin> <output.raw>\n", argv[0]);
        return 2;
    }
    const char *in_path = argv[1];
    const char *out_path = argv[2];

    FILE *in = fopen(in_path, "rb");
    if (!in) {
        fprintf(stderr, "replay_reassembler: cannot open input '%s'\n", in_path);
        return 1;
    }
    FILE *out = fopen(out_path, "wb");
    if (!out) {
        fprintf(stderr, "replay_reassembler: cannot open output '%s'\n", out_path);
        fclose(in);
        return 1;
    }
    setvbuf(out, NULL, _IOFBF, 4u << 20);

    dv_output_t sink;
    dv_output_file(&sink, out);

    dv_reassembler_t reasm;
    if (dv_reassembler_init(&reasm, &sink) != 0) {
        fprintf(stderr, "replay_reassembler: failed to init reassembler\n");
        fclose(in);
        fclose(out);
        return 1;
    }

    uint8_t buf[CHUNK_BYTES];
    size_t n;
    unsigned long chunks = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        chunks++;
        if (dv_reassembler_feed(&reasm, buf, n) != 0) {
            fprintf(stderr, "replay_reassembler: feed failed at chunk %lu\n", chunks);
            break;
        }
    }
    fclose(in);

    dv_reassembler_finish(&reasm);
    fclose(out);

    fprintf(stderr, "replay_reassembler: %s -> %s\n", in_path, out_path);
    fprintf(stderr, "  chunks fed:          %lu\n", chunks);
    fprintf(stderr, "  format:              %s\n",
            reasm.format == DV_FORMAT_HDV ? "HDV" :
            reasm.format == DV_FORMAT_DV ? "DV" : "unknown");
    fprintf(stderr, "  bytes fed:           %lu\n", reasm.bytes_fed);
    fprintf(stderr, "  dif bytes:           %lu\n", reasm.dif_bytes);
    fprintf(stderr, "  frames written:      %lu\n", reasm.frames_written);
    fprintf(stderr, "  sequences written:   %lu\n", reasm.sequences_written);
    fprintf(stderr, "  sequences dropped:   %lu\n", reasm.sequences_dropped);
    fprintf(stderr, "  ts packets:          %lu\n", reasm.ts_packets);
    fprintf(stderr, "  ts discarded:        %lu\n", reasm.ts_discarded);
    fprintf(stderr, "  ts sync errors:      %lu\n", reasm.ts_sync_errors);
    fprintf(stderr, "  ts cc errors:        %lu\n", reasm.ts_cc_errors);
    fprintf(stderr, "  msg resyncs:         %lu\n", reasm.msg_resyncs);
    fprintf(stderr, "  iso resyncs:         %lu\n", reasm.iso_resyncs);
    fprintf(stderr, "  dbc gaps:            %lu\n", reasm.dbc_gaps);
    fprintf(stderr, "  dbc lost blocks:     %lu\n", reasm.dbc_lost_blocks);

    return 0;
}
