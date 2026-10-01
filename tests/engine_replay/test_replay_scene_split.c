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
 * ctest: builds a synthetic .dv file (a plain sequence of frame-aligned DIF
 * frames, which pin_session.c's replay_run() plays back frame-by-frame --
 * no OHCI/type-9 framing needed for that path, see its "is_dv_frames"
 * branch) with a timecode jump partway through its SSYB pack 0x13, exactly
 * as dv_subcode.c documents (subcode blocks 1-2 of each sequence, 6 SSYBs
 * at offset 6 + 8*i -- see dv_subcode_pack_offset()).
 *
 * Two variants:
 *   - a persistent jump (50 continuous frames, then 50 frames at
 *     timecode+10s): --split must produce two files, cut exactly at the
 *     first anomalous frame (the debounce FIFO back-dates the cut, see
 *     pin_scene.h), containing 50 frames each (both segments are over
 *     1 s and 1 MB, see pin_split.h).
 *   - a jump only 10 frames before the end: the new segment would be
 *     under 1 s, so it is appended to the first file (one file, all
 *     100 frames).
 *   - a single-frame glitch (one frame is jumped, then the timecode goes
 *     right back to where the unglitched run would have been): --split
 *     must NOT cut -- a single bad frame is debounced away -- producing
 *     one file with all 100 frames.
 *
 * Usage: test_replay_scene_split (no arguments; builds its own trace files)
 */

#include "replay_test_util.h"
#include "dv_subcode.h"

#include <stdio.h>
#include <stdlib.h>

#define N_FRAMES 100 /* ~3.3 s: a new segment needs >= 1 s and >= 1 MB to get its own file */
#define SEQ_COUNT DV_SEQ_COUNT_NTSC
#define FRAME_SIZE (SEQ_COUNT * DV_SEQ_SIZE)
#define JUMP_AT 50
#define TAIL_JUMP_AT (N_FRAMES - 10) /* jump 10 frames before the end: too short for a file */
#define JUMP_FRAMES 300 /* 10 s at a nominal 30 fps, well over tc_jump_seconds (1.0) */

static int bcd_byte(int tens, int units) { return ((tens & 0xF) << 4) | (units & 0xF); }

static void set_block_section(uint8_t *frame, unsigned seq, unsigned block, int section)
{
    size_t off = seq * DV_SEQ_SIZE + (size_t)block * DV_BLOCK_SIZE;
    frame[off + 0] = (uint8_t)(section << 5);
    frame[off + 1] = 0xFF;
    frame[off + 2] = 0xFF;
}

static void build_frame_skeleton(uint8_t *frame)
{
    memset(frame, 0xFF, FRAME_SIZE);
    for (unsigned s = 0; s < SEQ_COUNT; s++) {
        set_block_section(frame, s, 0, 0);
        frame[s * DV_SEQ_SIZE + 3] = 0x00; /* DSF = 0: NTSC */
        for (unsigned b = 1; b <= 2; b++) set_block_section(frame, s, b, 1);
        for (unsigned b = 3; b <= 5; b++) set_block_section(frame, s, b, 2);
        for (unsigned b = 6; b < DV_SEQ_BLOCKS; b++)
            set_block_section(frame, s, b, ((b - 6) % 16 == 0) ? 3 : 4);
    }
}

/* frame_number is a flat 0-based counter; converted to h:m:s:f at a nominal
 * 30 units/second purely so a +JUMP_FRAMES offset reads as a multi-second
 * jump -- this project's DV subcode is IEC 61834 BCD timecode, not tied to
 * any particular frame rate here. */
static void fill_timecode(uint8_t *frame, long frame_number)
{
    int f = (int)(frame_number % 30);
    long total_s = frame_number / 30;
    int s = (int)(total_s % 60);
    int m = (int)((total_s / 60) % 60);
    int h = (int)(total_s / 3600);
    uint8_t d[4];
    d[0] = (uint8_t)bcd_byte(f / 10, f % 10);
    d[1] = (uint8_t)bcd_byte(s / 10, s % 10);
    d[2] = (uint8_t)bcd_byte(m / 10, m % 10);
    d[3] = (uint8_t)bcd_byte(h / 10, h % 10);
    for (unsigned sq = 0; sq < SEQ_COUNT; sq++) {
        size_t off1 = sq * DV_SEQ_SIZE + dv_subcode_pack_offset(1, 3);
        size_t off2 = sq * DV_SEQ_SIZE + dv_subcode_pack_offset(2, 3);
        frame[off1] = DV_PACK_TIMECODE; memcpy(frame + off1 + 1, d, 4);
        frame[off2] = DV_PACK_TIMECODE; memcpy(frame + off2 + 1, d, 4);
    }
}

/* Writes N_FRAMES frames to `path`. glitch_only: if 0, frames >= JUMP_AT all
 * carry frame_number + JUMP_FRAMES (a persistent jump); if 1, only frame
 * JUMP_AT itself does, and every frame after it continues the original,
 * unjumped sequence (a single-frame glitch that self-heals). */
static void write_synthetic_trace(const char *path, int glitch_only, long jump_at)
{
    uint8_t *frame = malloc(FRAME_SIZE);
    CHECK(frame);
    FILE *f = fopen(path, "wb");
    CHECK(f);
    for (long i = 0; i < N_FRAMES; i++) {
        build_frame_skeleton(frame);
        long tc = i;
        if (glitch_only) {
            if (i == jump_at) tc = i + JUMP_FRAMES;
        } else {
            if (i >= jump_at) tc = i + JUMP_FRAMES;
        }
        fill_timecode(frame, tc);
        CHECK(fwrite(frame, 1, FRAME_SIZE, f) == (size_t)FRAME_SIZE);
    }
    fclose(f);
    free(frame);
}

static long file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

/* Runs a --split capture against `trace_path` for long enough to play the
 * whole (short) synthetic trace once through, then waits for the
 * single-pass replay to auto-stop at EOT (see replay_handle_eot() in
 * pin_session.c). */
static void run_split_capture(const char *trace_path, const char *out_base)
{
    pin_session_t *s = pin_test_open_replay(trace_path);
    pin_test_prepare_dv(s);

    pin_capture_opts_t opts;
    pin_capture_opts_defaults(&opts);
    strncpy(opts.path, out_base, sizeof(opts.path) - 1);
    opts.format_dv = PIN_FMT_DV_RAW;
    opts.scene_split = 1;
    CHECK(pin_capture_start(s, &opts, 1) == PIN_OK);

    pin_status_snapshot_t snap;
    pin_test_wait_state(s, PIN_STATE_CAPTURING, PIN_STATE_ERROR, 3000, &snap);
    CHECK(snap.state == PIN_STATE_CAPTURING);

    /* N_FRAMES frames at ~33 ms pacing is a little over a second; give it
     * generous headroom, then wait for the single pass to auto-stop. */
    pin_test_wait_state(s, PIN_STATE_READY, PIN_STATE_ERROR, 10000, &snap);
    CHECK(snap.state == PIN_STATE_READY);
    pin_capture_stop(s); /* no-op if already stopped */
    pin_close(s);
}

int main(void)
{
    /* ---- persistent jump: expect a clean 2-way split ---- */
    write_synthetic_trace("scene_jump.dv", 0, JUMP_AT);
    run_split_capture("scene_jump.dv", "scene_jump_out");

    CHECK(pin_test_file_exists("scene_jump_out-0001.dv"));
    CHECK(pin_test_file_exists("scene_jump_out-0002.dv"));
    CHECK(!pin_test_file_exists("scene_jump_out-0003.dv"));
    long f1 = file_size("scene_jump_out-0001.dv");
    long f2 = file_size("scene_jump_out-0002.dv");
    long f1_frames = f1 / FRAME_SIZE, f2_frames = f2 / FRAME_SIZE;
    printf("jump split: file1 %ld bytes (%ld frames), file2 %ld bytes (%ld frames)\n",
           f1, f1_frames, f2, f2_frames);
    /* The replay worker can start streaming its very first frame before the
     * capture-start command (posted from this thread, right after seeing
     * READY) reaches its mailbox -- so at most that one leading frame may
     * be missing from what actually gets captured. Everything from the
     * timecode jump onward is never at risk that way, so file2's frame
     * count is exact; file1's is JUMP_AT, minus 0 or 1. */
    CHECK(f1_frames == JUMP_AT || f1_frames == JUMP_AT - 1);
    CHECK_EQ_I(f2_frames, N_FRAMES - JUMP_AT);
    printf("OK: persistent timecode jump cut cleanly at frame %d\n", JUMP_AT);

    /* ---- single-frame glitch: expect no split at all ---- */
    write_synthetic_trace("scene_glitch.dv", 1, JUMP_AT);
    run_split_capture("scene_glitch.dv", "scene_glitch_out");

    CHECK(pin_test_file_exists("scene_glitch_out-0001.dv"));
    CHECK(!pin_test_file_exists("scene_glitch_out-0002.dv"));
    long g1 = file_size("scene_glitch_out-0001.dv");
    long g1_frames = g1 / FRAME_SIZE;
    printf("glitch: single file %ld bytes (%ld frames)\n", g1, g1_frames);
    CHECK(g1_frames == N_FRAMES || g1_frames == N_FRAMES - 1); /* see the jump case's comment */
    printf("OK: single-frame glitch was debounced away, no split\n");

    /* ---- jump just before the end: the tiny tail is merged, not split off ---- */
    write_synthetic_trace("scene_tail.dv", 0, TAIL_JUMP_AT);
    run_split_capture("scene_tail.dv", "scene_tail_out");

    CHECK(pin_test_file_exists("scene_tail_out-0001.dv"));
    CHECK(!pin_test_file_exists("scene_tail_out-0002.dv"));
    long t1_frames = file_size("scene_tail_out-0001.dv") / FRAME_SIZE;
    printf("short tail: single file (%ld frames)\n", t1_frames);
    CHECK(t1_frames == N_FRAMES || t1_frames == N_FRAMES - 1);
    printf("OK: short tail merged into the previous file\n");

    return 0;
}
