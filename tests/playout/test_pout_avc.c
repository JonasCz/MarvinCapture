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

/* pout_avc: the AV/C target. The commands are the bytes src/engine/pin_deck.c
 * sends (pin_deck_build_cmd, CMD_STATE, CMD_TIMECODE); they are copied here as
 * constants because pin_deck.c lives in the libusb part of the engine, and so
 * are the two parsers the capture side applies to the answers
 * (pin_deck_state_from_avc, pin_deck_parse_timecode). If either side changes,
 * this test must follow. */

#include "pout_avc.h"
#include <stdio.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);             \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

/* Fake tape: two recordings, 25 fps. Frames 0..749 carry timecode 00:00:00:00.., frames 750..1499
 * carry 01:00:00:00.. (so the timecode jumps by an hour between them, and 00:00:30:00 to
 * 00:59:59:24 is not on the tape). */
#define REC_LEN 750
#define REC_B_START_FRAMES (3600 * 25)
static int fake_tc_of_frame(void *ctx, int64_t f, pin_tc_t *tc)
{
    (void)ctx;
    if (f < 0 || f >= 2 * REC_LEN)
        return -1;
    int64_t n = f < REC_LEN ? f : REC_B_START_FRAMES + (f - REC_LEN);
    tc->f = (int)(n % 25);
    tc->s = (int)(n / 25 % 60);
    tc->m = (int)(n / 25 / 60 % 60);
    tc->h = (int)(n / 25 / 3600);
    tc->sep = ':';
    return 0;
}
static int64_t fake_frame_of_tc(void *ctx, const pin_tc_t *tc)
{
    (void)ctx;
    int64_t n = (((int64_t)tc->h * 60 + tc->m) * 60 + tc->s) * 25 + tc->f;
    if (n >= 0 && n < REC_LEN)
        return n;
    if (n >= REC_B_START_FRAMES && n < REC_B_START_FRAMES + REC_LEN)
        return REC_LEN + (n - REC_B_START_FRAMES);
    return -1;
}
static void fake_ops(pout_tape_ops_t *ops)
{
    ops->ctx = NULL;
    ops->frames = 2 * REC_LEN;
    ops->fps = 25.0;
    ops->tc_of_frame = fake_tc_of_frame;
    ops->frame_of_tc = fake_frame_of_tc;
}

/* pin_deck.c, pin_deck_build_cmd() */
static const uint8_t CMD_PLAY[4]  = { 0x00, 0x20, 0xc3, 0x75 };
static const uint8_t CMD_PAUSE[4] = { 0x00, 0x20, 0xc3, 0x7d };
static const uint8_t CMD_STOP[4]  = { 0x00, 0x20, 0xc4, 0x60 };
static const uint8_t CMD_FF[4]    = { 0x00, 0x20, 0xc4, 0x75 };
static const uint8_t CMD_REW[4]   = { 0x00, 0x20, 0xc4, 0x65 };
/* pin_deck.c, CMD_STATE and CMD_TIMECODE */
static const uint8_t CMD_STATE[4]    = { 0x01, 0x20, 0xd0, 0x7f };
static const uint8_t CMD_TIMECODE[8] = { 0x01, 0x20, 0x51, 0x71, 0xff, 0xff, 0xff, 0xff };

/* pin_deck.c, pin_deck_state_from_avc(), with the api's pin_deck_state_t spelled as letters */
enum { ST_UNKNOWN, ST_STOPPED, ST_PLAYING, ST_PAUSED, ST_FF, ST_REW, ST_REC };
static int state_from_avc(uint8_t op, uint8_t mode)
{
    if (op == 0xc3) {
        switch (mode) {
        case 0x75: case 0x6d: return ST_PLAYING;
        case 0x7d: case 0x65: return ST_PAUSED;
        default: return ST_PLAYING;
        }
    }
    if (op == 0xc4) {
        switch (mode) {
        case 0x60: return ST_STOPPED;
        case 0x65: case 0x45: return ST_REW;
        case 0x75: return ST_FF;
        default: return ST_UNKNOWN;
        }
    }
    if (op == 0xc2)
        return ST_REC;
    return ST_UNKNOWN;
}

/* pin_deck.c, pin_deck_parse_timecode(): frame, second, minute, hour, one BCD byte each */
static int parse_timecode(const uint8_t *resp, int rl, int out[5])
{
    if (rl < 8 || resp[1] != 0x20 || resp[2] != 0x51 || (resp[0] != 0x0c && resp[0] != 0x0b))
        return -1;
    uint8_t f = resp[4], s = resp[5], m = resp[6], h = resp[7];
    if (((f & 0xf) > 9) || ((s & 0xf) > 9) || ((m & 0xf) > 9) || ((h & 0xf) > 9) ||
        ((f >> 4) & 0x7) > 5 || ((s >> 4) & 0x7) > 5 || ((m >> 4) & 0x7) > 5 || ((h >> 4) & 0xf) > 2)
        return -1;
    out[0] = ((f >> 4) & 0x3) * 10 + (f & 0xf);
    out[1] = ((s >> 4) & 0x7) * 10 + (s & 0xf);
    out[2] = ((m >> 4) & 0x7) * 10 + (m & 0xf);
    out[3] = ((h >> 4) & 0x3) * 10 + (h & 0xf);
    out[4] = (f & 0x80) != 0;
    return 0;
}

static pout_avc_t g_avc;
static pout_deck_t g_deck;

static void setup(pout_avc_media_t media)
{
    pout_tape_ops_t ops;
    fake_ops(&ops);
    pout_deck_init(&g_deck, &ops, NULL, 0);
    pout_avc_init(&g_avc, media);
}

/* one transaction; returns the response length */
static int tx(double now, const uint8_t *cmd, unsigned len, uint8_t *resp)
{
    return pout_avc_handle(&g_avc, &g_deck, now, cmd, len, resp, 64);
}

static int is(const uint8_t *got, int gl, const uint8_t *want, int wl)
{
    return gl == wl && memcmp(got, want, (size_t)wl) == 0;
}
#define EXPECT(now, cmd, want, msg)                                                            \
    do {                                                                                       \
        uint8_t r_[64];                                                                        \
        int n_ = tx(now, cmd, sizeof(cmd), r_);                                                \
        if (!is(r_, n_, want, sizeof(want))) {                                                 \
            printf("FAIL %s:%d: %s (got %d bytes:", __FILE__, __LINE__, msg, n_);              \
            for (int i_ = 0; i_ < n_; i_++) printf(" %02x", r_[i_]);                           \
            printf(")\n");                                                                     \
            g_failures++;                                                                      \
        }                                                                                      \
    } while (0)

static void test_unit_queries(void)
{
    setup(POUT_AVC_DV_625);
    static const uint8_t unit_info[]  = { 0x01, 0xff, 0x30, 0xff, 0xff, 0xff, 0xff, 0xff };
    static const uint8_t unit_info_a[] = { 0x0c, 0xff, 0x30, 0x07, 0x20, 0x00, 0x00, 0x00 };
    EXPECT(0, unit_info, unit_info_a, "UNIT INFO: tape recorder 0x20, company id 0");
    g_avc.company_id = 0x000085;
    static const uint8_t unit_info_b[] = { 0x0c, 0xff, 0x30, 0x07, 0x20, 0x00, 0x00, 0x85 };
    EXPECT(0, unit_info, unit_info_b, "company id settable (the Canon's is 0x000085)");
    g_avc.company_id = 0;

    static const uint8_t sub[]   = { 0x01, 0xff, 0x31, 0x07, 0xff, 0xff, 0xff, 0xff };
    static const uint8_t sub_a[] = { 0x0c, 0xff, 0x31, 0x07, 0x20, 0xff, 0xff, 0xff };
    EXPECT(0, sub, sub_a, "SUBUNIT INFO page 0: the VCR");
    static const uint8_t sub1[]   = { 0x01, 0xff, 0x31, 0x17, 0xff, 0xff, 0xff, 0xff };
    static const uint8_t sub1_a[] = { 0x0a, 0xff, 0x31, 0x17, 0xff, 0xff, 0xff, 0xff };
    EXPECT(0, sub1, sub1_a, "SUBUNIT INFO page 1: rejected");

    static const uint8_t plug[]   = { 0x01, 0xff, 0x02, 0x00, 0xff, 0xff, 0xff, 0xff };
    static const uint8_t plug_a[] = { 0x0c, 0xff, 0x02, 0x00, 0x01, 0x01, 0x00, 0x00 };
    EXPECT(0, plug, plug_a, "PLUG INFO");

    static const uint8_t outf[]   = { 0x01, 0xff, 0x18, 0x00, 0xff, 0xff, 0xff, 0xff };
    static const uint8_t outf_625[] = { 0x0c, 0xff, 0x18, 0x00, 0x80, 0x80, 0xff, 0xff };
    static const uint8_t inf[]    = { 0x01, 0xff, 0x19, 0x00, 0xff, 0xff, 0xff, 0xff };
    static const uint8_t inf_625[] = { 0x0c, 0xff, 0x19, 0x00, 0x80, 0x80, 0xff, 0xff };
    EXPECT(0, outf, outf_625, "OUTPUT PLUG SIGNAL FORMAT, DV 625/50");
    EXPECT(0, inf, inf_625, "INPUT PLUG SIGNAL FORMAT, DV 625/50");
    static const uint8_t inq_ok[]  = { 0x02, 0xff, 0x18, 0x00, 0x80, 0x80, 0xff, 0xff };
    static const uint8_t inq_ok_a[] = { 0x0c, 0xff, 0x18, 0x00, 0x80, 0x80, 0xff, 0xff };
    static const uint8_t inq_525[] = { 0x02, 0xff, 0x18, 0x00, 0x80, 0x00, 0xff, 0xff };
    static const uint8_t inq_525_a[] = { 0x08, 0xff, 0x18, 0x00, 0x80, 0x00, 0xff, 0xff };
    static const uint8_t inq_hdv[] = { 0x02, 0xff, 0x18, 0x00, 0xa0, 0x00, 0x00, 0x00 };
    static const uint8_t inq_hdv_a[] = { 0x08, 0xff, 0x18, 0x00, 0xa0, 0x00, 0x00, 0x00 };
    EXPECT(0, inq_ok, inq_ok_a, "specific inquiry for our format");
    EXPECT(0, inq_525, inq_525_a, "specific inquiry for 525/60 on a 625 tape: not implemented");
    EXPECT(0, inq_hdv, inq_hdv_a, "specific inquiry for HDV on a DV tape: not implemented");
    static const uint8_t ctl[]   = { 0x00, 0xff, 0x18, 0x00, 0xa0, 0x00, 0x00, 0x00 };
    static const uint8_t ctl_a[] = { 0x0a, 0xff, 0x18, 0x00, 0xa0, 0x00, 0x00, 0x00 };
    EXPECT(0, ctl, ctl_a, "plug format control: rejected");

    setup(POUT_AVC_DV_525);
    static const uint8_t outf_525[] = { 0x0c, 0xff, 0x18, 0x00, 0x80, 0x00, 0xff, 0xff };
    EXPECT(0, outf, outf_525, "DV 525/60");
    setup(POUT_AVC_HDV);
    static const uint8_t outf_hdv[] = { 0x0c, 0xff, 0x18, 0x00, 0xa0, 0x00, 0x00, 0x00 };
    static const uint8_t inf_hdv[]  = { 0x0c, 0xff, 0x19, 0x00, 0xa0, 0x00, 0x00, 0x00 };
    EXPECT(0, outf, outf_hdv, "HDV: a0 00 00 00 as in the CIP header");
    EXPECT(0, inf, inf_hdv, "HDV input plug");
    static const uint8_t inq_hdv_ok[] = { 0x0c, 0xff, 0x18, 0x00, 0xa0, 0x00, 0x00, 0x00 };
    EXPECT(0, inq_hdv, inq_hdv_ok, "specific inquiry for HDV on an HDV tape");
}

static void test_transport(void)
{
    setup(POUT_AVC_DV_625);
    uint8_t r[64];
    int n;
    static const uint8_t stopped[] = { 0x0c, 0x20, 0xc4, 0x60 };
    EXPECT(0, CMD_STATE, stopped, "stopped: WIND stop");

    static const uint8_t ok_play[] = { 0x09, 0x20, 0xc3, 0x75 };
    EXPECT(10.0, CMD_PLAY, ok_play, "PLAY accepted, echoed");
    static const uint8_t st_play_tr[] = { 0x0b, 0x20, 0xc3, 0x75 };
    EXPECT(10.1, CMD_STATE, st_play_tr, "state: PLAY at once, in transition while the tape threads");
    static const uint8_t st_play[] = { 0x0c, 0x20, 0xc3, 0x75 };
    EXPECT(11.5, CMD_STATE, st_play, "state: PLAY stable");
    n = tx(11.5, CMD_STATE, 4, r);
    CHECK(state_from_avc(r[2], r[3]) == ST_PLAYING, "pin_deck_state_from_avc says playing");

    static const uint8_t ok_pause[] = { 0x09, 0x20, 0xc3, 0x7d };
    EXPECT(12.0, CMD_PAUSE, ok_pause, "PAUSE accepted");
    n = tx(20.0, CMD_STATE, 4, r);
    CHECK(n == 4 && r[0] == 0x0c && state_from_avc(r[2], r[3]) == ST_PAUSED, "paused");

    static const uint8_t ok_ff[] = { 0x09, 0x20, 0xc4, 0x75 };
    EXPECT(30.0, CMD_FF, ok_ff, "FF accepted");
    n = tx(31.0, CMD_STATE, 4, r);
    CHECK(n == 4 && r[0] == 0x0c && state_from_avc(r[2], r[3]) == ST_FF, "FF winding");
    static const uint8_t ok_stop[] = { 0x09, 0x20, 0xc4, 0x60 };
    EXPECT(31.5, CMD_STOP, ok_stop, "STOP accepted");
    n = tx(31.5, CMD_STATE, 4, r);
    CHECK(n == 4 && state_from_avc(r[2], r[3]) == ST_STOPPED, "stopped");

    static const uint8_t ok_rew[] = { 0x09, 0x20, 0xc4, 0x65 };
    EXPECT(40.0, CMD_REW, ok_rew, "REW accepted");
    n = tx(40.1, CMD_STATE, 4, r);
    CHECK(n == 4 && r[0] == 0x0b && state_from_avc(r[2], r[3]) == ST_REW, "REW in transition, then");
    /* at frame 0 REW ends by itself (wind stop) */
    n = tx(45.0, CMD_STATE, 4, r);
    CHECK(n == 4 && r[0] == 0x0c && state_from_avc(r[2], r[3]) == ST_STOPPED, "REW at the start ends in WIND stop");

    /* other modes */
    static const uint8_t slow[]   = { 0x00, 0x20, 0xc3, 0x32 };
    static const uint8_t slow_a[] = { 0x09, 0x20, 0xc3, 0x32 };
    EXPECT(50.0, slow, slow_a, "slow forward: accepted, played at normal speed");
    CHECK(pout_deck_state(&g_deck, 50.0) == POUT_DECK_PLAY, "... as forward play");
    static const uint8_t rev[]   = { 0x00, 0x20, 0xc3, 0x65 };
    static const uint8_t rev_a[] = { 0x08, 0x20, 0xc3, 0x65 };
    EXPECT(51.0, rev, rev_a, "reverse play: not implemented");
    CHECK(pout_deck_state(&g_deck, 51.0) == POUT_DECK_PLAY, "... and the deck is left alone");
    static const uint8_t hs[]   = { 0x00, 0x20, 0xc4, 0x45 };
    static const uint8_t hs_a[] = { 0x09, 0x20, 0xc4, 0x45 };
    EXPECT(52.0, hs, hs_a, "high speed rewind: accepted");
    CHECK(pout_deck_state(&g_deck, 52.0) == POUT_DECK_REW, "... as rewind");
    static const uint8_t badwind[]   = { 0x00, 0x20, 0xc4, 0x10 };
    static const uint8_t badwind_a[] = { 0x08, 0x20, 0xc4, 0x10 };
    EXPECT(53.0, badwind, badwind_a, "unknown wind mode: not implemented");
    static const uint8_t shortplay[]   = { 0x00, 0x20, 0xc3 };
    static const uint8_t shortplay_a[] = { 0x0a, 0x20, 0xc3 };
    EXPECT(53.0, shortplay, shortplay_a, "PLAY without a mode: rejected");

    /* status of PLAY / WIND */
    pout_deck_play(&g_deck, 60.0);
    static const uint8_t splay[]   = { 0x01, 0x20, 0xc3, 0xff };
    static const uint8_t splay_a[] = { 0x0c, 0x20, 0xc3, 0x75 };
    EXPECT(62.0, splay, splay_a, "PLAY status while playing");
    static const uint8_t swind[]   = { 0x01, 0x20, 0xc4, 0xff };
    static const uint8_t swind_a[] = { 0x0a, 0x20, 0xc4, 0xff };
    EXPECT(62.0, swind, swind_a, "WIND status while playing: rejected");
}

static void test_timecode(void)
{
    setup(POUT_AVC_DV_625);
    uint8_t r[64];
    int tc[5], n;
    /* frame 0 */
    n = tx(0, CMD_TIMECODE, 8, r);
    CHECK(n == 8 && parse_timecode(r, n, tc) == 0 && !tc[0] && !tc[1] && !tc[2] && !tc[3], "00:00:00:00 at the start");

    /* go to 00:00:12:03 = frame 303 by a frame search of the deck, then ask over AV/C */
    pout_deck_search_frame(&g_deck, 0.0, 303);
    n = tx(10.0, CMD_TIMECODE, 8, r);
    CHECK(n == 8 && r[0] == 0x0c && r[3] == 0x71, "TIME CODE status is stable");
    CHECK(parse_timecode(r, n, tc) == 0 && tc[0] == 3 && tc[1] == 12 && tc[2] == 0 && tc[3] == 0 && tc[4] == 0,
          "pin_deck_parse_timecode reads 00:00:12:03");
    static const uint8_t bytes[] = { 0x0c, 0x20, 0x51, 0x71, 0x03, 0x12, 0x00, 0x00 };
    CHECK(is(r, n, bytes, sizeof(bytes)), "BCD, frame first and hour last, as the Canon sends it");

    /* TIME CODE search into the second recording, across the gap: 01:00:05:00 */
    static const uint8_t seek[]   = { 0x00, 0x20, 0x51, 0x20, 0x00, 0x05, 0x00, 0x01 };
    static const uint8_t seek_a[] = { 0x09, 0x20, 0x51, 0x20, 0x00, 0x05, 0x00, 0x01 };
    EXPECT(20.0, seek, seek_a, "search accepted");
    n = tx(20.1, CMD_STATE, 4, r);
    CHECK(n == 4 && r[0] == 0x0b && state_from_avc(r[2], r[3]) == ST_FF, "TRANSPORT STATE shows the search: FF, in transition");
    n = tx(21.5, CMD_STATE, 4, r);       /* settling */
    CHECK(n == 4 && r[0] == 0x0b && state_from_avc(r[2], r[3]) == ST_FF, "still in transition while settling");
    n = tx(25.0, CMD_STATE, 4, r);
    CHECK(n == 4 && r[0] == 0x0c && state_from_avc(r[2], r[3]) == ST_PAUSED, "ends paused");
    n = tx(25.0, CMD_TIMECODE, 8, r);
    CHECK(n == 8 && parse_timecode(r, n, tc) == 0 && tc[0] == 0 && tc[1] == 5 && tc[2] == 0 && tc[3] == 1, "at 01:00:05:00");
    CHECK(pout_deck_position(&g_deck, 25.0) == 875, "frame 875");

    /* backwards search in the same recording: REW */
    static const uint8_t back[]   = { 0x00, 0x20, 0x51, 0x20, 0x00, 0x01, 0x00, 0x01 };
    static const uint8_t back_a[] = { 0x09, 0x20, 0x51, 0x20, 0x00, 0x01, 0x00, 0x01 };
    EXPECT(30.0, back, back_a, "search back accepted");
    n = tx(30.1, CMD_STATE, 4, r);
    CHECK(n == 4 && state_from_avc(r[2], r[3]) == ST_REW, "REW while searching back");

    /* not on the tape (the gap), not valid BCD, hour 25 */
    static const uint8_t gap[]   = { 0x00, 0x20, 0x51, 0x20, 0x00, 0x00, 0x30, 0x00 };
    static const uint8_t gap_a[] = { 0x0a, 0x20, 0x51, 0x20, 0x00, 0x00, 0x30, 0x00 };
    EXPECT(40.0, gap, gap_a, "timecode in the gap: rejected");
    static const uint8_t junk[]   = { 0x00, 0x20, 0x51, 0x20, 0xff, 0xff, 0xff, 0xff };
    static const uint8_t junk_a[] = { 0x0a, 0x20, 0x51, 0x20, 0xff, 0xff, 0xff, 0xff };
    EXPECT(40.0, junk, junk_a, "not a time code: rejected");
    static const uint8_t hour[]   = { 0x00, 0x20, 0x51, 0x20, 0x00, 0x00, 0x00, 0x25 };
    static const uint8_t hour_a[] = { 0x0a, 0x20, 0x51, 0x20, 0x00, 0x00, 0x00, 0x25 };
    EXPECT(40.0, hour, hour_a, "hour 25: rejected");

    /* RTC mirrors the time code, ATN answers, medium info answers */
    static const uint8_t rtc[] = { 0x01, 0x20, 0x57, 0x71, 0xff, 0xff, 0xff, 0xff };
    n = tx(50.0, rtc, 8, r);
    CHECK(n == 8 && r[0] == 0x0c && r[2] == 0x57 && r[3] == 0x71, "RELATIVE TIME COUNTER answered");
    uint8_t r2[64];
    int n2 = tx(50.0, CMD_TIMECODE, 8, r2);
    CHECK(n2 == 8 && memcmp(r + 4, r2 + 4, 4) == 0, "... with the time code");
    static const uint8_t atn[] = { 0x01, 0x20, 0x52, 0x71, 0xff, 0xff, 0xff, 0xff };
    n = tx(50.0, atn, 8, r);
    CHECK(n == 8 && r[0] == 0x0c && r[3] == 0x71, "ABSOLUTE TRACK NUMBER status answered");
    static const uint8_t atn_c[]   = { 0x00, 0x20, 0x52, 0x20, 0x00, 0x00, 0x01, 0xff };
    static const uint8_t atn_c_a[] = { 0x08, 0x20, 0x52, 0x20, 0x00, 0x00, 0x01, 0xff };
    EXPECT(50.0, atn_c, atn_c_a, "ATN control: not implemented");
    static const uint8_t medium[] = { 0x01, 0x20, 0xda, 0xff, 0xff };
    n = tx(50.0, medium, 5, r);
    CHECK(n == 5 && r[0] == 0x0c && r[2] == 0xda, "MEDIUM INFO answered");
}

static void test_refusals(void)
{
    setup(POUT_AVC_DV_625);
    uint8_t r[64];
    static const uint8_t rec[]   = { 0x00, 0x20, 0xc2, 0x75 };
    static const uint8_t rec_a[] = { 0x0a, 0x20, 0xc2, 0x75 };
    EXPECT(0, rec, rec_a, "RECORD: rejected");
    CHECK(pout_deck_state(&g_deck, 0) == POUT_DECK_STOP, "... and nothing happened");
    static const uint8_t vendor[]   = { 0x00, 0x20, 0x00, 0x00, 0x00, 0x85 };
    static const uint8_t vendor_a[] = { 0x08, 0x20, 0x00, 0x00, 0x00, 0x85 };
    EXPECT(0, vendor, vendor_a, "vendor dependent (Canon id): not implemented");
    static const uint8_t vendor2[]   = { 0x00, 0x20, 0x00, 0x00, 0x80, 0x45 };
    static const uint8_t vendor2_a[] = { 0x08, 0x20, 0x00, 0x00, 0x80, 0x45 };
    EXPECT(0, vendor2, vendor2_a, "vendor dependent (Panasonic id): not implemented");
    static const uint8_t unk[]   = { 0x01, 0x20, 0x71, 0xff, 0xff };
    static const uint8_t unk_a[] = { 0x08, 0x20, 0x71, 0xff, 0xff };
    EXPECT(0, unk, unk_a, "opcode 0x71: not implemented");
    static const uint8_t sigmode[]   = { 0x01, 0x20, 0x78, 0xff };
    static const uint8_t sigmode_a[] = { 0x08, 0x20, 0x78, 0xff };
    EXPECT(0, sigmode, sigmode_a, "OUTPUT SIGNAL MODE: not implemented");
    static const uint8_t notify[]   = { 0x03, 0x20, 0xd0, 0x7f };
    static const uint8_t notify_a[] = { 0x08, 0x20, 0xd0, 0x7f };
    EXPECT(0, notify, notify_a, "NOTIFY: not implemented");
    static const uint8_t panel[]   = { 0x01, 0x28, 0xd0, 0x7f };
    static const uint8_t panel_a[] = { 0x08, 0x28, 0xd0, 0x7f };
    EXPECT(0, panel, panel_a, "a subunit we do not have: not implemented");
    static const uint8_t unit_unk[]   = { 0x01, 0xff, 0x60, 0xff };
    static const uint8_t unit_unk_a[] = { 0x08, 0xff, 0x60, 0xff };
    EXPECT(0, unit_unk, unit_unk_a, "unknown unit opcode: not implemented");
    static const uint8_t ctl_state[]   = { 0x00, 0x20, 0xd0, 0x7f };
    static const uint8_t ctl_state_a[] = { 0x08, 0x20, 0xd0, 0x7f };
    EXPECT(0, ctl_state, ctl_state_a, "TRANSPORT STATE as a control: not implemented");

    /* inquiries */
    static const uint8_t i1[]   = { 0x02, 0x20, 0xc3, 0x75 };
    static const uint8_t i1_a[] = { 0x0c, 0x20, 0xc3, 0x75 };
    static const uint8_t i2[]   = { 0x02, 0x20, 0xc3, 0x65 };
    static const uint8_t i2_a[] = { 0x08, 0x20, 0xc3, 0x65 };
    static const uint8_t i3[]   = { 0x04, 0x20, 0xc4 };
    static const uint8_t i3_a[] = { 0x0c, 0x20, 0xc4 };
    static const uint8_t i4[]   = { 0x04, 0x20, 0xc2 };
    static const uint8_t i4_a[] = { 0x08, 0x20, 0xc2 };
    EXPECT(0, i1, i1_a, "specific inquiry PLAY forward");
    EXPECT(0, i2, i2_a, "specific inquiry PLAY reverse");
    EXPECT(0, i3, i3_a, "general inquiry WIND");
    EXPECT(0, i4, i4_a, "general inquiry RECORD");

    /* not command frames, and buffers */
    static const uint8_t resp_frame[] = { 0x0c, 0x20, 0xc3, 0x75 };
    CHECK(tx(0, resp_frame, 4, r) == 0, "a response frame gets no answer");
    CHECK(tx(0, resp_frame, 2, r) == 0, "a frame of 2 bytes gets no answer");
    CHECK(pout_avc_handle(&g_avc, &g_deck, 0, CMD_STATE, 4, r, 3) == 0, "response buffer too small: nothing");
    uint8_t short_tc[4] = { 0x01, 0x20, 0x51, 0x71 };
    int n = pout_avc_handle(&g_avc, &g_deck, 0, short_tc, 4, r, 16);
    CHECK(n == 8 && r[0] == 0x0c, "a 4 byte time code query still gets the 8 byte answer");
    CHECK(pout_avc_handle(&g_avc, &g_deck, 0, short_tc, 4, r, 6) == 0, "... unless the buffer is too small");
}

int main(void)
{
    test_unit_queries();
    test_transport();
    test_timecode();
    test_refusals();
    if (g_failures) {
        printf("%d failure(s)\n", g_failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
