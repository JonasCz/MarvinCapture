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
#include <stdlib.h>
#include <string.h>

static int ts_pid(const uint8_t *pkt)
{
    return ((pkt[1] & 0x1F) << 8) | pkt[2];
}

/* Returns the payload offset (>=4) and *payload_len, or -1 if this packet
 * carries no payload for us (adaptation-only, or bad sync byte). */
static int ts_payload(const uint8_t *pkt, size_t *payload_len, int *discontinuity)
{
    if (pkt[0] != HDV_TS_SYNC_BYTE)
        return -1;
    int afc = (pkt[3] >> 4) & 0x3; /* adaptation_field_control */
    int off = 4;
    if (afc == 2 || afc == 3) {
        int af_len = pkt[4];
        if (discontinuity && af_len > 0 && (pkt[5] & 0x80))
            *discontinuity = 1;
        off += 1 + af_len;
    }
    if (afc == 0 || afc == 2) /* no payload */
        return -1;
    if (off >= HDV_TS_PACKET_SIZE)
        return -1;
    *payload_len = HDV_TS_PACKET_SIZE - (size_t)off;
    return off;
}

static void pat_pmt_scan_one(const uint8_t *pkt, hdv_pid_map_t *map)
{
    size_t plen;
    int off = ts_payload(pkt, &plen, NULL);
    if (off < 0)
        return;
    int pusi = (pkt[1] & 0x40) != 0;
    const uint8_t *p = pkt + off;
    if (pusi) {
        if (plen < 1)
            return;
        int pointer = p[0];
        if ((size_t)(1 + pointer) >= plen)
            return;
        p += 1 + pointer;
        plen -= (size_t)(1 + pointer);
    } else {
        return; /* PSI sections we care about always start with PUSI here */
    }

    int pid = ts_pid(pkt);
    if (pid == 0x0000 && !map->pat_found) {
        /* table_id(1) section_length hi/lo(2) ... skip to program loop */
        if (plen < 8)
            return;
        int section_length = ((p[1] & 0x0F) << 8) | p[2];
        if ((size_t)(3 + section_length) > plen)
            section_length = (int)plen - 3;
        const uint8_t *prog = p + 8; /* after table header + TSID/version/section nums */
        const uint8_t *end = p + 3 + section_length - 4 /* CRC */;
        for (; prog + 4 <= end; prog += 4) {
            int program_number = (prog[0] << 8) | prog[1];
            int pmt_pid = ((prog[2] & 0x1F) << 8) | prog[3];
            if (program_number != 0) {
                map->pmt_pid = pmt_pid;
                map->pat_found = 1;
                break;
            }
        }
    } else if (map->pat_found && pid == map->pmt_pid && !map->pmt_found) {
        if (plen < 12)
            return;
        int section_length = ((p[1] & 0x0F) << 8) | p[2];
        if ((size_t)(3 + section_length) > plen)
            section_length = (int)plen - 3;
        int program_info_length = ((p[10] & 0x0F) << 8) | p[11];
        const uint8_t *es = p + 12 + program_info_length;
        const uint8_t *end = p + 3 + section_length - 4;
        while (es + 5 <= end) {
            int stream_type = es[0];
            int es_pid = ((es[1] & 0x1F) << 8) | es[2];
            int es_info_length = ((es[3] & 0x0F) << 8) | es[4];
            if (stream_type == 0x02 && map->video_pid < 0)
                map->video_pid = es_pid;
            /* MPEG-1 Layer II (0x03) or MPEG-2 audio (0x04); HDV always uses
             * MPEG-1 Layer II in practice, but either stream_type decodes
             * with the same mp2 decoder. */
            if ((stream_type == 0x03 || stream_type == 0x04) && map->audio_pid < 0)
                map->audio_pid = es_pid;
            /* Sony/Canon HDV AUX-V/AUX-A private streams are commonly
             * signalled with stream_type 0xA0/0xA1; best-effort, see
             * header comment. */
            if ((stream_type == 0xA0 || stream_type == 0xA1) && map->aux_pid < 0)
                map->aux_pid = es_pid;
            es += 5 + es_info_length;
        }
        map->pmt_found = 1;
    }
}

void hdv_scan_pat_pmt(const uint8_t *ts_packets, size_t n_packets, hdv_pid_map_t *map)
{
    if (!ts_packets || !map)
        return;
    for (size_t i = 0; i < n_packets; i++)
        pat_pmt_scan_one(ts_packets + i * HDV_TS_PACKET_SIZE, map);
}

/* Skips a PES header (present because PUSI was set) to find where
 * elementary-stream bytes begin within `payload`. Returns the offset, or
 * payload_len if this doesn't look like a PES packet start (caller then
 * just appends nothing useful, which is safe). */
static size_t pes_header_skip(const uint8_t *payload, size_t payload_len)
{
    if (payload_len < 9 || payload[0] != 0x00 || payload[1] != 0x00 || payload[2] != 0x01)
        return payload_len;
    size_t header_data_length = payload[8];
    size_t es_off = 9 + header_data_length;
    if (es_off > payload_len)
        return payload_len;
    return es_off;
}

int hdv_parse_picture(const uint8_t *ts_packets, size_t n_packets, int video_pid,
                       hdv_gop_time_t *gop_out, int *discontinuity_seen)
{
    if (!ts_packets || !gop_out || video_pid < 0)
        return -1;

    memset(gop_out, 0, sizeof(*gop_out));

    uint8_t *es = malloc(n_packets * (HDV_TS_PACKET_SIZE - 4) + 8);
    if (!es)
        return -1;
    size_t es_len = 0;

    for (size_t i = 0; i < n_packets; i++) {
        const uint8_t *pkt = ts_packets + i * HDV_TS_PACKET_SIZE;
        if (pkt[0] != HDV_TS_SYNC_BYTE || ts_pid(pkt) != video_pid)
            continue;
        int local_disc = 0;
        size_t plen;
        int off = ts_payload(pkt, &plen, &local_disc);
        if (local_disc && discontinuity_seen)
            *discontinuity_seen = 1;
        if (off < 0)
            continue;
        const uint8_t *payload = pkt + off;
        int pusi = (pkt[1] & 0x40) != 0;
        size_t start = pusi ? pes_header_skip(payload, plen) : 0;
        size_t n = plen - start;
        memcpy(es + es_len, payload + start, n);
        es_len += n;
    }

    gop_out->found = 0;
    for (size_t i = 0; i + 8 <= es_len; i++) {
        if (es[i] == 0x00 && es[i + 1] == 0x00 && es[i + 2] == 0x01 && es[i + 3] == 0xB8) {
            uint32_t v = ((uint32_t)es[i + 4] << 24) | ((uint32_t)es[i + 5] << 16) |
                         ((uint32_t)es[i + 6] << 8) | es[i + 7];
            gop_out->drop_frame = (v >> 31) & 1;
            gop_out->hours = (v >> 26) & 0x1F;
            gop_out->minutes = (v >> 20) & 0x3F;
            gop_out->seconds = (v >> 13) & 0x3F;
            gop_out->pictures = (v >> 7) & 0x3F;
            gop_out->closed_gop = (v >> 6) & 1;
            gop_out->broken_link = (v >> 5) & 1;
            gop_out->found = 1;
            break;
        }
    }

    free(es);
    return 0;
}

unsigned hdv_ts_discontinuity_count(const uint8_t *ts_packets, size_t n_packets)
{
    unsigned count = 0;
    for (size_t i = 0; i < n_packets; i++) {
        const uint8_t *pkt = ts_packets + i * HDV_TS_PACKET_SIZE;
        if (pkt[0] != HDV_TS_SYNC_BYTE)
            continue;
        int afc = (pkt[3] >> 4) & 0x3;
        if ((afc == 2 || afc == 3) && pkt[4] > 0 && (pkt[5] & 0x80))
            count++;
    }
    return count;
}

static int bcd2(uint8_t b, int tens_mask, int tens_shift)
{
    return ((b >> tens_shift) & tens_mask) * 10 + (b & 0x0F);
}

void hdv_aux_scan_datetime(const uint8_t *ts_packets, size_t n_packets, int aux_pid,
                            hdv_aux_info_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!ts_packets || !out || aux_pid < 0)
        return;

    for (size_t i = 0; i < n_packets; i++) {
        const uint8_t *pkt = ts_packets + i * HDV_TS_PACKET_SIZE;
        if (pkt[0] != HDV_TS_SYNC_BYTE || ts_pid(pkt) != aux_pid)
            continue;
        size_t plen;
        int off = ts_payload(pkt, &plen, NULL);
        if (off < 0)
            continue;
        const uint8_t *p = pkt + off;
        for (size_t k = 0; k + 5 <= plen; k++) {
            uint8_t pc0 = p[k];
            const uint8_t *d = p + k + 1;
            if (pc0 == 0x62 && !out->rec_date.valid) {
                out->rec_date.day = bcd2(d[0], 0x3, 4);
                out->rec_date.month = bcd2(d[1], 0x1, 4);
                int y2 = bcd2(d[2], 0xF, 4);
                out->rec_date.year = (y2 < 75) ? (2000 + y2) : (1900 + y2);
                out->rec_date.valid = 1;
            } else if (pc0 == 0x63 && !out->rec_time.valid) {
                out->rec_time.second = bcd2(d[0], 0x7, 4);
                out->rec_time.minute = bcd2(d[1], 0x7, 4);
                out->rec_time.hour = bcd2(d[2], 0x3, 4);
                out->rec_time.valid = 1;
            } else if (pc0 == 0x13 && !out->timecode.valid) {
                out->timecode.frame = bcd2(d[0], 0x3, 4);
                out->timecode.drop_frame = (d[0] >> 7) & 1;
                out->timecode.second = bcd2(d[1], 0x7, 4);
                out->timecode.minute = bcd2(d[2], 0x7, 4);
                out->timecode.hour = bcd2(d[3], 0x3, 4);
                out->timecode.valid = 1;
            }
        }
    }
}
