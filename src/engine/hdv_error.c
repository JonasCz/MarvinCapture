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

#include "hdv_error.h"
#include "hdv_aux.h"
#include <string.h>

void hdv_error_init(hdv_err_state_t *st)
{
    memset(st, 0, sizeof(*st));
    for (int i = 0; i < 8192; i++) st->cc[i] = -1;
    st->init = 1;
}

void hdv_error_analyze(hdv_err_state_t *st, const uint8_t *ts, size_t n,
                       int video_pid, hdv_unit_errors_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!st->init) hdv_error_init(st);

    int video_cc_err = 0, pes_seen = 0, pes_bad = 0;
    unsigned long vpayload = 0;
    unsigned pes_len = 0;
    /* ES start code scanner, runs across packet boundaries */
    uint32_t win = 0;
    int need = 0, need_kind = 0;
    uint8_t cap[4];
    int capn = 0, pics = 0;

    for (size_t i = 0; i < n; i++) {
        const uint8_t *p = ts + i * 188;
        if (p[0] != 0x47) { out->sync_lost++; continue; }
        if (p[1] & 0x80) out->tei++;
        int pid = ((p[1] & 0x1F) << 8) | p[2];
        int afc = (p[3] >> 4) & 3;
        if (pid == 0x1FFF) continue;
        if (afc & 1) {
            int cc = p[3] & 15;
            int last = st->cc[pid];
            if (last >= 0 && cc != last && cc != ((last + 1) & 15)) {
                out->cc_errors++;
                if (pid == video_pid) video_cc_err = 1;
            }
            /* an adaptation discontinuity resets the expectation legitimately */
            if (afc & 2 && p[4] > 0 && (p[5] & 0x80)) { /* keep cc, just no error */ }
            st->cc[pid] = (int16_t)cc;
        }
        if (pid != video_pid || !(afc & 1))
            continue;
        int off = 4;
        if (afc & 2) off = 5 + p[4];
        if (off >= 188) continue;
        const uint8_t *pl = p + off;
        size_t plen = 188 - off;
        vpayload += plen;
        size_t start = 0;
        if (p[1] & 0x40) {
            if (!pes_seen) {
                pes_seen = 1;
                if (plen < 9 || pl[0] || pl[1] || pl[2] != 1 || (pl[3] & 0xF0) != 0xE0) {
                    pes_bad = 1;
                } else {
                    pes_len = ((unsigned)pl[4] << 8) | pl[5];
                    start = 9 + pl[8];
                    if (start > plen) start = plen;
                }
            } else {
                continue; /* a second PES start: not part of this unit's picture */
            }
        } else if (!pes_seen) {
            pes_bad = 1; /* unit does not begin at a PES start */
            pes_seen = 1;
        }
        for (size_t k = start; k < plen; k++) {
            uint8_t c = pl[k];
            if (need > 0) {
                cap[capn++] = c;
                if (--need == 0) {
                    if (need_kind == 2) {               /* sequence: w(12) h(12) aspect(4) rate(4) */
                        out->seq_found = 1;
                        out->seq_width = (cap[0] << 4) | (cap[1] >> 4);
                        out->seq_height = ((cap[1] & 15) << 8) | cap[2];
                        out->seq_frame_rate_code = cap[3] & 15;
                    } else if (need_kind == 0 && pics == 0) {  /* picture: tr(10) type(3) */
                        out->pic_type = (cap[1] >> 3) & 7;
                        if (out->pic_type > 3) out->pic_type = 0;
                    } else if (need_kind == 1) {        /* GOP: ... closed_gop */
                        if (cap[3] & 0x40) out->closed_gop = 1;
                    }
                    if (need_kind == 0) pics++;
                }
                win = (win << 8) | c;
                continue;
            }
            win = (win << 8) | c;
            if ((win >> 8) == 0x000001) {
                if ((win & 0xFF) == 0x00) { need = 2; need_kind = 0; capn = 0; }
                else if ((win & 0xFF) == 0xB8) { need = 4; need_kind = 1; capn = 0; }
                else if ((win & 0xFF) == 0xB3) { need = 4; need_kind = 2; capn = 0; }
            }
        }
    }

    if (video_pid > 0) {
        if (pes_len && pes_len + 6 != vpayload) out->pes_errors++;
        if (pes_bad || pics == 0) out->pes_errors++;
    }
    int own = out->tei || out->sync_lost || video_cc_err || out->pes_errors;
    if (video_pid > 0 && (out->tei || out->sync_lost)) own = 1;
    out->video_damaged = own;

    /* propagate */
    int tainted = 0;
    switch (out->pic_type) {
    case 1: /* I */
        st->ref_dmg_prev = st->ref_dmg_last;
        st->ref_dmg_last = own;
        if (out->closed_gop) st->ref_dmg_prev = 0;
        break;
    case 2: /* P */
        tainted = st->ref_dmg_last && !own;
        st->ref_dmg_prev = st->ref_dmg_last;
        st->ref_dmg_last = own || st->ref_dmg_last;
        break;
    case 3: /* B */
        tainted = (st->ref_dmg_last || st->ref_dmg_prev) && !own;
        break;
    default:
        break;
    }
    out->tainted = tainted;
    out->frame_error = own || tainted || out->cc_errors;
}
