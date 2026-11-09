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
 * ctest: an HDV capture from an EP 0x88 dump (built here from hdv.ts, one TS packet per
 * isochronous packet) that loses 16 video packets in transit from the middle of a B picture.
 * The continuity counters cannot see that (they count modulo 16); the 1394 block count (DBC)
 * can. The status must show exactly that one picture as a frame with error (a B picture is no
 * reference, so nothing else is tainted), none dropped, and the same dump without the hole no
 * error at all.
 *
 * Usage: test_replay_hdv_loss <hdv.ts>
 */

#include "replay_test_util.h"
#include "../ep88_synth.h"

#define TS 188

static unsigned char *read_whole_file(const char *path, long *out_len)
{
    FILE *f = fopen(path, "rb");
    CHECK(f);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)n);
    CHECK(buf && fread(buf, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    *out_len = n;
    return buf;
}

static int ts_pid(const unsigned char *p) { return ((p[1] & 0x1F) << 8) | p[2]; }

/* a video PES start (a picture, as the reassembler cuts them) */
static int picture_start(const unsigned char *p)
{
    if (!(p[1] & 0x40) || !(p[3] & 0x10))
        return 0;
    size_t po = (p[3] & 0x20) ? 5u + p[4] : 4u;
    return po + 4 <= TS && !p[po] && !p[po + 1] && p[po + 2] == 1 && (p[po + 3] & 0xF0) == 0xE0;
}

/* One HDV isochronous packet (61883-4: DBS 6, FN 3, SPH, FMT 0x20) of one source packet. */
static void hdv_packet(ep88_synth_t *w, const unsigned char *ts)
{
    uint8_t rec[4 + 8 + 192 + 4];
    uint8_t msg[4];
    ep88_le32(rec, ((uint32_t)(8 + 192) << 16) | (1u << 14) | (63u << 8) | (0xAu << 4));
    rec[4] = 0x01; rec[5] = 0x06; rec[6] = 0xC4; rec[7] = (uint8_t)w->dbc;
    rec[8] = 0xA0; rec[9] = 0x00; rec[10] = 0x00; rec[11] = 0x00;
    memset(rec + 12, 0, 4);          /* the source packet header (time stamp): not used */
    memcpy(rec + 16, ts, TS);
    ep88_le32(rec + 12 + 192, 0x84110000u);
    ep88_le32(msg, (9u << 28) | ((uint32_t)sizeof(rec) << 16) | 0x7000u);
    ep88_put(w, msg, 4);
    ep88_put(w, rec, sizeof(rec));
}

/* Writes the dump; the TS packets [drop_from, drop_from + drop_n) are lost in transit (not
 * written, the DBC goes on: 8 blocks per source packet). */
static void write_dump(const char *path, const unsigned char *src, long n, long drop_from, long drop_n)
{
    ep88_synth_t w;
    memset(&w, 0, sizeof(w));
    for (long i = 0; i < n; i++) {
        if (i < drop_from || i >= drop_from + drop_n)
            hdv_packet(&w, src + i * TS);
        w.dbc = (w.dbc + 8) & 0xFF;
    }
    FILE *f = fopen(path, "wb");
    CHECK(f && fwrite(w.data, 1, w.len, f) == w.len);
    fclose(f);
    free(w.data);
}

/* Captures the whole dump as raw HDV; returns the ended capture's status. */
static pin_status_snapshot_t capture(const char *dump, const char *base)
{
    /* the capture is asked for before the first picture arrives, so it starts at the first GOP */
    pin_test_setenv("PIN_REPLAY_HOLD_MS", "400");
    pin_session_t *s = pin_test_open_replay(dump);
    pin_test_prepare_dv(s);
    pin_capture_opts_t opts;
    pin_capture_opts_defaults(&opts);
    strncpy(opts.path, base, sizeof(opts.path) - 1);
    opts.format_hdv = PIN_FMT_HDV_TS;
    CHECK(pin_capture_start(s, &opts, 1) == PIN_OK);
    pin_status_snapshot_t snap;
    pin_test_wait_state(s, PIN_STATE_CAPTURING, PIN_STATE_ERROR, 3000, &snap);
    CHECK(snap.state == PIN_STATE_CAPTURING);
    pin_test_wait_state(s, PIN_STATE_READY, PIN_STATE_ERROR, 20000, &snap);
    CHECK(snap.state == PIN_STATE_READY);
    pin_status_snapshot_t end;
    memset(&end, 0, sizeof(end));
    end.size = sizeof(end);
    CHECK(pin_get_capture_end_status(s, &end) == PIN_OK);
    pin_close(s);
    pin_test_setenv("PIN_REPLAY_HOLD_MS", "");
    printf("%s: stop %d, frames=%llu error=%llu dropped=%llu lost_blocks=%llu\n", base,
           (int)end.stop_reason, (unsigned long long)end.frames, (unsigned long long)end.frames_error,
           (unsigned long long)(end.frames_dropped + end.write_dropped),
           (unsigned long long)end.lost_blocks);
    CHECK_EQ_I(end.stop_reason, PIN_STOP_END_OF_TAPE);
    return end;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    pin_test_require_fixture(argv[1]);
    long len = 0;
    unsigned char *src = read_whole_file(argv[1], &len);
    CHECK(len % TS == 0 && src[0] == 0x47);
    long n = len / TS;

    /* the video PID and the 15th picture (coded order I P B B P B B ...: the first B of the
     * second GOP), and in it 16 video packets in a row that start nothing */
    int vpid = -1;
    long pic = -1, pics = 0;
    for (long i = 0; i < n && pic < 0; i++) {
        if (picture_start(src + i * TS)) {
            if (vpid < 0)
                vpid = ts_pid(src + i * TS);
            if (ts_pid(src + i * TS) == vpid && ++pics == 15)
                pic = i;
        }
    }
    CHECK(pic > 0);
    long drop = -1;
    for (long i = pic + 1, run = 0; i < n && drop < 0; i++) {
        const unsigned char *p = src + i * TS;
        if (ts_pid(p) == vpid && picture_start(p))
            break;   /* the next picture: no run found in this one */
        run = ts_pid(p) == vpid && !(p[1] & 0x40) ? run + 1 : 0;
        if (run == 16)
            drop = i - 15;
    }
    CHECK(drop > 0);

    write_dump("hdv_loss_clean.bin", src, n, 0, 0);
    write_dump("hdv_loss_hole.bin", src, n, drop, 16);

    pin_status_snapshot_t clean = capture("hdv_loss_clean.bin", "hdv_loss_clean");
    CHECK(clean.frames > 20);
    CHECK_EQ_I(clean.frames_error, 0);
    CHECK_EQ_I(clean.frames_dropped + clean.write_dropped, 0);

    pin_status_snapshot_t hole = capture("hdv_loss_hole.bin", "hdv_loss_hole");
    CHECK_EQ_I(hole.frames, clean.frames);
    CHECK_EQ_I(hole.frames_error, 1);
    CHECK_EQ_I(hole.frames_dropped + hole.write_dropped, 0);
    printf("OK: 16 lost video packets (unseen by the continuity counters) make their picture a "
           "frame with error, and only that one\n");
    free(src);
    return 0;
}
