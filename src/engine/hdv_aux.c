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

/* A PSI section of table_id that starts in pkt and ends in it (pointer_field,
 * section_length): 1, and *pmt_pid for a PAT (its first program's PMT PID). */
static int psi_section(const uint8_t *pkt, int table_id, int *pmt_pid)
{
    size_t plen;
    int off = ts_payload(pkt, &plen, NULL);
    if (off < 0 || !(pkt[1] & 0x40) || plen < 1)
        return 0;
    const uint8_t *p = pkt + off;
    size_t sec = 1 + (size_t)p[0]; /* skip pointer_field */
    if (sec + 3 > plen || p[sec] != table_id)
        return 0;
    size_t section_length = ((size_t)(p[sec + 1] & 0x0F) << 8) | p[sec + 2];
    if (sec + 3 + section_length > plen || section_length < 9)
        return 0;
    if (pmt_pid) {
        *pmt_pid = 0;
        const uint8_t *prog = p + sec + 8, *end = p + sec + 3 + section_length - 4 /* CRC */;
        for (; prog + 4 <= end; prog += 4) {
            if (((prog[0] << 8) | prog[1]) != 0) { /* program 0 is the NIT */
                *pmt_pid = ((prog[2] & 0x1F) << 8) | prog[3];
                break;
            }
        }
        if (*pmt_pid == 0)
            return 0;
    }
    return 1;
}

void hdv_psi_scan(hdv_psi_t *psi, const uint8_t *ts_packets, size_t n_packets)
{
    if (!psi || !ts_packets)
        return;
    for (size_t i = 0; i < n_packets; i++) {
        const uint8_t *pkt = ts_packets + i * HDV_TS_PACKET_SIZE;
        if (pkt[0] != HDV_TS_SYNC_BYTE || !(pkt[1] & 0x40))
            continue;
        int pid = ts_pid(pkt), pmt_pid;
        if (pid == 0 && psi_section(pkt, 0x00, &pmt_pid)) {
            memcpy(psi->pat, pkt, HDV_TS_PACKET_SIZE);
            psi->pat_valid = 1;
            if (pmt_pid != psi->pmt_pid)
                psi->pmt_valid = 0;
            psi->pmt_pid = pmt_pid;
        } else if (psi->pmt_pid && pid == psi->pmt_pid && psi_section(pkt, 0x02, NULL)) {
            memcpy(psi->pmt, pkt, HDV_TS_PACKET_SIZE);
            psi->pmt_valid = 1;
        }
    }
}

size_t hdv_ts_count_pid(const uint8_t *ts_packets, size_t n_packets, int pid)
{
    size_t n = 0;
    for (size_t i = 0; ts_packets && i < n_packets; i++)
        n += ts_pid(ts_packets + i * HDV_TS_PACKET_SIZE) == pid;
    return n;
}

size_t hdv_ts_drop_pid(const uint8_t *ts_packets, size_t n_packets, int pid, uint8_t *out)
{
    size_t n = 0;
    for (size_t i = 0; ts_packets && out && i < n_packets; i++) {
        const uint8_t *pkt = ts_packets + i * HDV_TS_PACKET_SIZE;
        if (ts_pid(pkt) != pid)
            memmove(out + n++ * HDV_TS_PACKET_SIZE, pkt, HDV_TS_PACKET_SIZE);
    }
    return n;
}

uint32_t hdv_crc32_mpeg(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; data && i < len; i++) {
        crc ^= (uint32_t)data[i] << 24;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
    }
    return crc;
}

/* The transport_stream_id and the first program's number (not 0, the NIT) of a PAT packet. */
static int pat_first_program(const uint8_t *pkt, int *tsid, int *program)
{
    size_t plen;
    int off = ts_payload(pkt, &plen, NULL);
    if (off < 0 || !psi_section(pkt, 0x00, NULL))
        return 0;
    const uint8_t *sec = pkt + off + 1 + pkt[off]; /* behind the pointer_field */
    size_t section_length = ((size_t)(sec[1] & 0x0F) << 8) | sec[2];
    const uint8_t *prog = sec + 8, *end = sec + 3 + section_length - 4 /* CRC */;
    for (; prog + 4 <= end; prog += 4) {
        int n = (prog[0] << 8) | prog[1];
        if (n) {
            *tsid = (sec[3] << 8) | sec[4];
            *program = n;
            return 1;
        }
    }
    return 0;
}

/* s as a DVB string (EN 300 468 Annex A) of at most cap bytes: the 0x15 selector (UTF-8), then
 * as much of s as fits, cut before a character, not inside one. Always the selector, also for
 * plain ASCII: the default table (ISO/IEC 6937) differs from ASCII in places ('$', '#'). Returns
 * the length, 0 for an empty s. */
static size_t dvb_utf8_text(const char *s, uint8_t *out, size_t cap)
{
    size_t len = s ? strlen(s) : 0;
    if (!len || cap < 2)
        return 0;
    size_t n = len < cap - 1 ? len : cap - 1;
    while (n > 0 && n < len && ((unsigned char)s[n] & 0xC0) == 0x80) /* s[n] continues a character */
        n--;
    if (!n)
        return 0;
    out[0] = 0x15;
    memcpy(out + 1, s, n);
    return 1 + n;
}

int hdv_sdt_packet(const hdv_psi_t *psi, const char *provider, const char *name, unsigned cc,
                   uint8_t out[HDV_TS_PACKET_SIZE])
{
    int tsid, program;
    if (!psi || !out || !psi->pat_valid || !pat_first_program(psi->pat, &tsid, &program))
        return 0;
    uint8_t prov[HDV_SDT_TEXT_ROOM], text[HDV_SDT_TEXT_ROOM];
    size_t pl = dvb_utf8_text(provider, prov, HDV_SDT_TEXT_ROOM / 2);
    size_t nl = dvb_utf8_text(name, text, HDV_SDT_TEXT_ROOM - pl);
    if (!nl)
        return 0;

    memset(out, 0xFF, HDV_TS_PACKET_SIZE); /* stuffing behind the section */
    uint8_t *p = out;
    *p++ = HDV_TS_SYNC_BYTE;
    *p++ = 0x40 | (HDV_SDT_PID >> 8); /* payload_unit_start */
    *p++ = HDV_SDT_PID & 0xFF;
    *p++ = (uint8_t)(0x10 | (cc & 0x0F)); /* payload only */
    *p++ = 0x00;                          /* pointer_field */
    uint8_t *sec = p;
    size_t desc_len = 3 + pl + nl;        /* service_type, two length bytes, the texts */
    size_t section_length = 5 + 3 + 5 + 2 + desc_len + 4;
    *p++ = 0x42; /* table_id: SDT of the actual transport stream */
    *p++ = (uint8_t)(0xF0 | (section_length >> 8)); /* section_syntax_indicator, reserved bits */
    *p++ = (uint8_t)(section_length & 0xFF);
    *p++ = (uint8_t)(tsid >> 8);
    *p++ = (uint8_t)(tsid & 0xFF);
    *p++ = 0xC1; /* reserved, version_number 0, current_next_indicator */
    *p++ = 0x00; /* section_number */
    *p++ = 0x00; /* last_section_number */
    *p++ = HDV_SDT_NETWORK_ID >> 8;
    *p++ = HDV_SDT_NETWORK_ID & 0xFF;
    *p++ = 0xFF; /* reserved_future_use */
    *p++ = (uint8_t)(program >> 8); /* service_id */
    *p++ = (uint8_t)(program & 0xFF);
    *p++ = 0xFC; /* reserved_future_use, no EIT schedule, no EIT present/following */
    *p++ = (uint8_t)(0x80 | ((2 + desc_len) >> 8)); /* running_status 4 (running), not scrambled */
    *p++ = (uint8_t)((2 + desc_len) & 0xFF);         /* descriptors_loop_length */
    *p++ = 0x48;                                     /* service_descriptor */
    *p++ = (uint8_t)desc_len;
    *p++ = 0x01; /* service_type: digital television */
    *p++ = (uint8_t)pl;
    memcpy(p, prov, pl);
    p += pl;
    *p++ = (uint8_t)nl;
    memcpy(p, text, nl);
    p += nl;
    uint32_t crc = hdv_crc32_mpeg(sec, (size_t)(p - sec));
    *p++ = (uint8_t)(crc >> 24);
    *p++ = (uint8_t)(crc >> 16);
    *p++ = (uint8_t)(crc >> 8);
    *p++ = (uint8_t)crc;
    return 1;
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
                /* as the DV pack (dv_subcode.c): colour frame bit 7, drop frame bit 6 */
                out->timecode.frame = bcd2(d[0], 0x3, 4);
                out->timecode.drop_frame = (d[0] >> 6) & 1;
                out->timecode.second = bcd2(d[1], 0x7, 4);
                out->timecode.minute = bcd2(d[2], 0x7, 4);
                out->timecode.hour = bcd2(d[3], 0x3, 4);
                out->timecode.valid = 1;
            }
        }
    }
}

size_t hdv_find_sequence_header(const uint8_t *es, size_t len)
{
    for (size_t i = 0; i + 4 <= len; i++)
        if (es[i] == 0 && es[i + 1] == 0 && es[i + 2] == 1 && es[i + 3] == 0xb3)
            return i;
    return (size_t)-1;
}

int hdv_gop_time_nonzero(const hdv_gop_time_t *g)
{
    return g && g->found && (g->hours || g->minutes || g->seconds || g->pictures);
}
