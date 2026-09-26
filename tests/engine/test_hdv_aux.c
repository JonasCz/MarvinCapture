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

#include "hdv_aux.h"
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

static void ts_header(uint8_t *pkt, int pid, int pusi, int cc)
{
    memset(pkt, 0xFF, HDV_TS_PACKET_SIZE); /* stuffing beyond payload_len */
    pkt[0] = HDV_TS_SYNC_BYTE;
    pkt[1] = (uint8_t)((pusi ? 0x40 : 0) | ((pid >> 8) & 0x1F));
    pkt[2] = (uint8_t)(pid & 0xFF);
    pkt[3] = (uint8_t)(0x10 | (cc & 0x0F)); /* afc = 01 (payload only) */
}

static void put16(uint8_t *b, int v) { b[0] = (uint8_t)(v >> 8); b[1] = (uint8_t)(v & 0xFF); }

static void build_pat(uint8_t *pkt, int pmt_pid)
{
    ts_header(pkt, 0x0000, 1, 0);
    uint8_t *p = pkt + 4;
    p[0] = 0x00; /* pointer_field */
    uint8_t *s = p + 1;
    int section_length = 9 + 1 * 4; /* tsid(2)+ver(1)+secnum(1)+lastsecnum(1)+1 program(4)+crc(4) */
    s[0] = 0x00; /* table_id: PAT */
    s[1] = (uint8_t)(0xB0 | ((section_length >> 8) & 0x0F));
    s[2] = (uint8_t)(section_length & 0xFF);
    put16(s + 3, 1);      /* transport_stream_id */
    s[5] = 0xC1;           /* version/current_next */
    s[6] = 0x00;           /* section_number */
    s[7] = 0x00;           /* last_section_number */
    put16(s + 8, 1);       /* program_number = 1 */
    put16(s + 10, (uint16_t)(0xE000 | (pmt_pid & 0x1FFF))); /* reserved(3)+pmt_pid(13) */
    /* CRC not checked by our parser; leave as zero. */
}

static void build_pmt(uint8_t *pkt, int pmt_pid, int video_pid, int aux_pid)
{
    ts_header(pkt, pmt_pid, 1, 0);
    uint8_t *p = pkt + 4;
    p[0] = 0x00; /* pointer_field */
    uint8_t *s = p + 1;
    int section_length = 9 + 5 + 5 + 4; /* progno+ver+secnum+lastsec+pcr+proginfolen(9) + 2 streams*5 + crc4 */
    s[0] = 0x02; /* table_id: PMT */
    s[1] = (uint8_t)(0xB0 | ((section_length >> 8) & 0x0F));
    s[2] = (uint8_t)(section_length & 0xFF);
    put16(s + 3, 1);       /* program_number */
    s[5] = 0xC1;
    s[6] = 0x00;
    s[7] = 0x00;
    put16(s + 8, (uint16_t)(0xE000 | (video_pid & 0x1FFF))); /* PCR_PID */
    put16(s + 10, 0);      /* program_info_length = 0 */
    uint8_t *es = s + 12;
    es[0] = 0x02; /* MPEG2 video */
    put16(es + 1, (uint16_t)(0xE000 | (video_pid & 0x1FFF)));
    put16(es + 3, 0);
    es += 5;
    es[0] = 0xA0; /* HDV AUX private stream (best-effort convention) */
    put16(es + 1, (uint16_t)(0xE000 | (aux_pid & 0x1FFF)));
    put16(es + 3, 0);
}

static uint32_t encode_gop_time(int drop, int h, int m, int s, int pics, int closed, int broken)
{
    uint32_t v = 0;
    v |= (uint32_t)(drop & 1) << 31;
    v |= (uint32_t)(h & 0x1F) << 26;
    v |= (uint32_t)(m & 0x3F) << 20;
    v |= 1u << 19; /* marker_bit */
    v |= (uint32_t)(s & 0x3F) << 13;
    v |= (uint32_t)(pics & 0x3F) << 7;
    v |= (uint32_t)(closed & 1) << 6;
    v |= (uint32_t)(broken & 1) << 5;
    v |= 0x1F; /* stuffing */
    return v;
}

static void build_video_picture_packet(uint8_t *pkt, int video_pid,
                                        int drop, int h, int m, int s, int pics,
                                        int closed, int broken)
{
    ts_header(pkt, video_pid, 1, 0);
    uint8_t *payload = pkt + 4; /* 184 bytes available */
    /* Minimal PES header: 00 00 01 E0, len(2)=0, flags1=0x80, flags2=0x00, hdl=0x00 */
    uint8_t pes[9] = { 0x00, 0x00, 0x01, 0xE0, 0x00, 0x00, 0x80, 0x00, 0x00 };
    memcpy(payload, pes, sizeof(pes));
    uint8_t *es = payload + sizeof(pes);
    es[0] = 0x00; es[1] = 0x00; es[2] = 0x01; es[3] = 0xB8;
    uint32_t v = encode_gop_time(drop, h, m, s, pics, closed, broken);
    es[4] = (uint8_t)(v >> 24);
    es[5] = (uint8_t)(v >> 16);
    es[6] = (uint8_t)(v >> 8);
    es[7] = (uint8_t)v;
}

static void test_pat_pmt_discovery(void)
{
    uint8_t pat[HDV_TS_PACKET_SIZE], pmt[HDV_TS_PACKET_SIZE];
    build_pat(pat, 0x0100);
    build_pmt(pmt, 0x0100, 0x0200, 0x0811);

    hdv_pid_map_t map;
    memset(&map, 0, sizeof(map));
    map.pmt_pid = -1;
    map.video_pid = -1;
    map.aux_pid = -1;

    uint8_t packets[2 * HDV_TS_PACKET_SIZE];
    memcpy(packets, pat, HDV_TS_PACKET_SIZE);
    memcpy(packets + HDV_TS_PACKET_SIZE, pmt, HDV_TS_PACKET_SIZE);
    hdv_scan_pat_pmt(packets, 2, &map);

    CHECK(map.pat_found && map.pmt_pid == 0x0100, "PAT gives the PMT PID");
    CHECK(map.pmt_found, "PMT parsed");
    CHECK(map.video_pid == 0x0200, "video PID from PMT");
    CHECK(map.aux_pid == 0x0811, "aux PID from PMT");
}

static void test_gop_header_decode(void)
{
    uint8_t pkt[HDV_TS_PACKET_SIZE];
    build_video_picture_packet(pkt, 0x0200, 1, 1, 23, 45, 5, 1, 0);

    hdv_gop_time_t gop;
    int disc = 0;
    int rc = hdv_parse_picture(pkt, 1, 0x0200, &gop, &disc);
    CHECK(rc == 0, "parse should succeed");
    CHECK(gop.found, "GOP header found");
    CHECK(gop.drop_frame == 1, "drop frame flag");
    CHECK(gop.hours == 1 && gop.minutes == 23 && gop.seconds == 45 && gop.pictures == 5,
          "GOP time_code fields");
    CHECK(gop.closed_gop == 1 && gop.broken_link == 0, "closed_gop/broken_link");
    CHECK(disc == 0, "no discontinuity expected");
}

static void test_no_gop_header(void)
{
    uint8_t pkt[HDV_TS_PACKET_SIZE];
    ts_header(pkt, 0x0200, 1, 0);
    memset(pkt + 4, 0, HDV_TS_PACKET_SIZE - 4);

    hdv_gop_time_t gop;
    int disc = 0;
    int rc = hdv_parse_picture(pkt, 1, 0x0200, &gop, &disc);
    CHECK(rc == 0, "parse should succeed even with no GOP header");
    CHECK(!gop.found, "no GOP header -> found = 0");
}

static void test_discontinuity_indicator(void)
{
    uint8_t pkt[HDV_TS_PACKET_SIZE];
    ts_header(pkt, 0x0200, 0, 1);
    pkt[3] = 0x30; /* afc = 11: adaptation + payload */
    pkt[4] = 1;    /* adaptation_field_length */
    pkt[5] = 0x80; /* discontinuity_indicator set */

    CHECK(hdv_ts_discontinuity_count(pkt, 1) == 1, "discontinuity indicator detected");

    uint8_t clean[HDV_TS_PACKET_SIZE];
    ts_header(clean, 0x0200, 0, 1);
    CHECK(hdv_ts_discontinuity_count(clean, 1) == 0, "no false positive without adaptation field");
}

static void test_aux_datetime_scan(void)
{
    uint8_t pkt[HDV_TS_PACKET_SIZE];
    ts_header(pkt, 0x0811, 0, 0);
    uint8_t *payload = pkt + 4;
    memset(payload, 0xFF, HDV_TS_PACKET_SIZE - 4);

    /* date pack 0x62: day=9, month=6, year=2024 -> BCD "24" */
    payload[10] = 0x62;
    payload[11] = 0x09; /* day */
    payload[12] = 0x06; /* month */
    payload[13] = 0x24; /* year (2-digit BCD) */
    payload[14] = 0xFF;

    /* time pack 0x63: 14:05:59 */
    payload[30] = 0x63;
    payload[31] = 0x59; /* seconds */
    payload[32] = 0x05; /* minutes */
    payload[33] = 0x14; /* hours */
    payload[34] = 0xFF;

    hdv_aux_info_t info;
    hdv_aux_scan_datetime(pkt, 1, 0x0811, &info);

    CHECK(info.rec_date.valid, "aux rec date found");
    CHECK(info.rec_date.day == 9 && info.rec_date.month == 6 && info.rec_date.year == 2024,
          "aux rec date fields");
    CHECK(info.rec_time.valid, "aux rec time found");
    CHECK(info.rec_time.hour == 14 && info.rec_time.minute == 5 && info.rec_time.second == 59,
          "aux rec time fields");
    CHECK(!info.timecode.valid, "no TC pack present -> invalid");
}

int main(void)
{
    test_pat_pmt_discovery();
    test_gop_header_decode();
    test_no_gop_header();
    test_discontinuity_indicator();
    test_aux_datetime_scan();

    if (g_failures == 0) {
        printf("test_hdv_aux: all tests passed\n");
        return 0;
    }
    printf("test_hdv_aux: %d failure(s)\n", g_failures);
    return g_failures;
}
