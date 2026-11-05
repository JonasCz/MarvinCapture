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
 * ctest: a tape with DV, then HDV, then DV again (ep88-dv-hdv-dv.bin: PAL DV 14
 * frames, HDV 1080i 50 pictures, NTSC DV 30 frames, as one EP 0x88 stream), captured
 * end to end through pin_api.h on the replay device from its start to its end:
 *   - to files (raw DV / raw HDV, no scene split): "name.dv" with every PAL frame,
 *     "name-0002.m2t" starting with PAT, PMT and a picture with a sequence header,
 *     "name-0003.dv" with the NTSC frames (the replay drops the frame still being read
 *     at the end of the file);
 *   - to standard output ("-"): a pipe carries one format, so the capture ends at the
 *     change with PIN_STOP_ERROR and says why; what went out is every PAL frame.
 *
 * Usage: test_replay_mixed_kind <data dir>
 */

#include "replay_test_util.h"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#define DUP _dup
#define DUP2 _dup2
#define CLOSE _close
#define OPEN_NEW(path) _open(path, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, 0600)
#else
#include <fcntl.h>
#include <unistd.h>
#define DUP dup
#define DUP2 dup2
#define CLOSE close
#define OPEN_NEW(path) open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600)
#endif

#define TS 188

typedef struct {
    unsigned char *data;
    long len;
} file_t;

static file_t read_file(const char *path)
{
    file_t b = { NULL, 0 };
    FILE *f = fopen(path, "rb");
    CHECK(f);
    fseek(f, 0, SEEK_END);
    b.len = ftell(f);
    fseek(f, 0, SEEK_SET);
    b.data = malloc((size_t)b.len + 1);
    CHECK(b.data);
    CHECK(fread(b.data, 1, (size_t)b.len, f) == (size_t)b.len);
    fclose(f);
    return b;
}

static int pkt_pid(const unsigned char *p) { return ((p[1] & 0x1F) << 8) | p[2]; }

/* a video PES start whose payload carries a sequence header */
static int gop_start(const unsigned char *p)
{
    if (!(p[1] & 0x40) || !(p[3] & 0x10))
        return 0;
    size_t po = (p[3] & 0x20) ? 5u + p[4] : 4u;
    if (po + 4 > TS || p[po] || p[po + 1] || p[po + 2] != 1 || (p[po + 3] & 0xF0) != 0xE0)
        return 0;
    for (size_t i = po; i + 4 <= TS; i++)
        if (!p[i] && !p[i + 1] && p[i + 2] == 1 && p[i + 3] == 0xB3)
            return 1;
    return 0;
}

/* Plays the tape from its start (the replay winds back in 2 s, then PLAY and the capture
 * start together) and waits for the capture to end; returns the final status. */
static pin_status_snapshot_t capture_tape(const char *dump, const pin_capture_opts_t *opts)
{
    pin_session_t *s = pin_test_open_replay(dump);
    pin_test_prepare_dv(s);
    pin_status_snapshot_t snap;
    CHECK(pin_deck(s, PIN_DECK_CMD_REW) == PIN_OK);
    int stopped = 0;
    for (int i = 0; i < 300 && !stopped; i++) {
        pin_test_sleep_ms(20);
        pin_get_status(s, &snap);
        stopped = snap.deck == PIN_DECK_STOPPED;
    }
    CHECK(stopped);
    CHECK(pin_deck(s, PIN_DECK_CMD_PLAY) == PIN_OK);
    CHECK(pin_capture_start(s, opts, 1) == PIN_OK);
    pin_test_wait_state(s, PIN_STATE_CAPTURING, PIN_STATE_ERROR, 3000, &snap);
    CHECK(snap.state == PIN_STATE_CAPTURING);
    pin_test_wait_state(s, PIN_STATE_READY, PIN_STATE_ERROR, 20000, &snap);
    CHECK(snap.state == PIN_STATE_READY);
    pin_close(s);
    return snap;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    char dump[4096], pal_path[4096], ntsc_path[4096];
    snprintf(dump, sizeof(dump), "%s/ep88-dv-hdv-dv.bin", argv[1]);
    snprintf(pal_path, sizeof(pal_path), "%s/dv-pal-32k.dv", argv[1]);
    snprintf(ntsc_path, sizeof(ntsc_path), "%s/dv-ntsc.dv", argv[1]);
    pin_test_require_fixture(dump);
    file_t pal = read_file(pal_path), ntsc = read_file(ntsc_path);

    /* 1. to files: .dv, .m2t, .dv */
    const char *names[] = { "mixed_kind.dv", "mixed_kind-0002.m2t", "mixed_kind-0003.dv",
                            "mixed_kind-0004.dv", "mixed_kind-0004.m2t", "mixed_kind.m2t",
                            "mixed_kind-0002.dv", "mixed_kind-0003.m2t" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        remove(names[i]);
    pin_capture_opts_t opts;
    pin_capture_opts_defaults(&opts);
    strncpy(opts.path, "mixed_kind", sizeof(opts.path) - 1);
    opts.format_dv = PIN_FMT_DV_RAW;
    opts.format_hdv = PIN_FMT_HDV_TS;
    pin_status_snapshot_t snap = capture_tape(dump, &opts);
    CHECK_EQ_I(snap.stop_reason, PIN_STOP_END_OF_TAPE);
    printf("captured %llu unit(s), %llu with errors\n", (unsigned long long)snap.frames,
           (unsigned long long)snap.frames_error);
    CHECK(snap.frames_error == 0);
    for (size_t i = 3; i < sizeof(names) / sizeof(names[0]); i++)
        CHECK(!pin_test_file_exists(names[i]));
    file_t f1 = read_file("mixed_kind.dv"), f2 = read_file("mixed_kind-0002.m2t"),
           f3 = read_file("mixed_kind-0003.dv");
    printf("files: %ld PAL frame(s), %ld TS packet(s), %ld NTSC frame(s)\n", f1.len / 144000,
           f2.len / TS, f3.len / 120000);
    CHECK(f1.len == pal.len && !memcmp(f1.data, pal.data, (size_t)pal.len));  /* every PAL frame */
    CHECK(f2.len > 3 * TS && f2.len % TS == 0);
    CHECK(pkt_pid(f2.data) == 0 && pkt_pid(f2.data + TS) == 0x81);           /* PAT, PMT */
    CHECK(pkt_pid(f2.data + 2 * TS) == 0x810 && gop_start(f2.data + 2 * TS)); /* a GOP */
    for (long i = 0; i < f2.len; i += TS)
        CHECK(f2.data[i] == 0x47);
    CHECK(f3.len == ntsc.len - 120000 && !memcmp(f3.data, ntsc.data, (size_t)f3.len));
    printf("OK: DV -> HDV -> DV gives .dv, .m2t (PAT, PMT, GOP first), .dv\n");

    /* 2. to standard output: the capture ends at the change, with the reason */
    int fd = OPEN_NEW("mixed_kind_pipe.out");
    CHECK(fd >= 0);
    fflush(stdout);
    int saved = DUP(1);
    CHECK(saved >= 0);
    CHECK(DUP2(fd, 1) >= 0);
    CLOSE(fd);
    pin_capture_opts_defaults(&opts);
    strncpy(opts.path, "-", sizeof(opts.path) - 1);
    snap = capture_tape(dump, &opts);
    fflush(stdout);
    CHECK(DUP2(saved, 1) >= 0);
    CLOSE(saved);
    printf("pipe: %s\n", snap.stop_text);
    CHECK_EQ_I(snap.stop_reason, PIN_STOP_ERROR);
    CHECK(strstr(snap.stop_text, "the input changed from DV to HDV; a pipe carries one format only"));
    file_t p = read_file("mixed_kind_pipe.out");
    CHECK(p.len == pal.len && !memcmp(p.data, pal.data, (size_t)pal.len));
    printf("OK: a pipe stops at DV -> HDV, after every DV frame\n");

    free(pal.data);
    free(ntsc.data);
    free(f1.data);
    free(f2.data);
    free(f3.data);
    free(p.data);
    return 0;
}
