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
 * Where the packetizer's isochronous packets go: the 1394 IT output on the device
 * (pout_hw_out1394.c, EP 0x06, not written yet) or the file/loopback output below (--dry-run,
 * no device). The interface is the one in docs/handoff/playout-plan.md; `priv` is the only
 * addition, for an implementation's own state. An implementation embeds pout_out_t as its first
 * member (so a pointer converts both ways) or points `priv` at its state.
 */

#ifndef POUT_OUT_H
#define POUT_OUT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pout_out {
    int  (*start)(struct pout_out *o);
    int  (*packet)(struct pout_out *o, const uint8_t *pkt, unsigned len); /* one isochronous packet */
    void (*stop)(struct pout_out *o);
    void (*destroy)(struct pout_out *o);
    void *priv;
} pout_out_t;

/* Adapter for pout_cip_frame(): user is a pout_out_t *. */
static inline int pout_out_send(void *user, const uint8_t *pkt, unsigned len)
{
    pout_out_t *o = (pout_out_t *)user;
    return o->packet(o, pkt, len);
}

/* The no-device output: wraps every packet as the device's EP 0x88 stream would carry it (type-9
 * messages, OHCI buffer-fill records) and feeds src/core/dv_reassembler, which writes a raw .dv
 * (DV) or a transport stream (HDV, from the first GOP on) to `path`. The file is created here
 * (NULL on failure, with the reason in err); start() may be called once, stop() finishes the
 * file (HDV: the picture in progress is dropped), destroy() stops if needed and frees. */
pout_out_t *pout_out_file_create(const char *path, char *err, size_t err_cap);

#ifdef __cplusplus
}
#endif

#endif /* POUT_OUT_H */
