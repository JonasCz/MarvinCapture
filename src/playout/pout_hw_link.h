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
 * The 1394 link for MarvinPlayout: opens the device and brings the link up (the same calls the
 * capture path makes, with the AV/C config ROM), answers incoming requests through pout_csr,
 * hands AV/C command frames to a callback, allocates a channel and bandwidth at the IRM, and
 * sends the EP 0x06 stream pout_it builds. Everything here needs the device and has not been
 * run; every assumption is marked "GUESS:" in pout_hw_link.c.
 *
 * Threads: one thread owns the link (pout_link_open/pump/irm/cycle timer/send_fcp/close, all of
 * which talk on EP 0x02 / EP 0x84). The EP 0x06 stream is written by an internal sender thread
 * behind a queue of chunks, so pout_link_it_packet() may be called from the same thread or from
 * another one, but only one at a time.
 *
 * Typical order (the session loop):
 *   pout_link_open()
 *   loop { pout_link_pump(k, 10); ... if pout_link_tx() says on, and the stream is not running:
 *          pout_link_it_start(); then once per packet pout_link_it_packet() (and pout_link_it_flush()
 *          at the end of each frame period); when it says off: pout_link_it_stop() }
 *   pout_link_close()
 * For a play without a controller: pout_link_local_connect(k, 1) allocates a channel and
 * bandwidth and sets the broadcast connection bit, so pout_link_tx() turns on.
 */

#ifndef POUT_HW_LINK_H
#define POUT_HW_LINK_H

#include "pout_csr.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pout_link pout_link_t;

/* An AV/C command frame (first byte the ctype) arrived. Write the response frame to resp (cap
 * bytes) and return its length, or 0 for no response (e.g. a frame that is not a command).
 * Called from inside pout_link_pump(); keep it quick (the sender waits for the response, the
 * split-transaction timeout is 100 ms by default). Further responses (after an INTERIM) can be
 * sent later with pout_link_send_fcp(). src is the node that sent the command. */
typedef int (*pout_link_avc_fn)(void *user, uint16_t src, const uint8_t *cmd, unsigned len,
                                uint8_t *resp, unsigned cap);
/* The plug state changed because a controller locked our oPCR: tx_on says whether we should
 * transmit, on channel / speed (OHCI spd code). Called from inside pout_link_pump(). */
typedef void (*pout_link_plug_fn)(void *user, int tx_on, unsigned channel, unsigned speed);

typedef struct {
    const char *device_id;        /* "usb:1-4" as pinnacle_enumerate() reports it; NULL = the first Marvin found */
    const char *bitstream_path;   /* the OHCI design: firmware/fpga-ohci.bin (pin_session_firmware_path() finds it) */
    unsigned max_packet;          /* bytes per isochronous packet: 488 DV, 584 HDV; sizes oPCR payload and the RAM ring */
    unsigned speed;               /* OHCI spd code of the stream (0 S100 .. 2 S400): oPCR's initial data rate and what a local connection sends at */
    unsigned channel;             /* a local connection's channel: 63 = the broadcast channel (no IRM allocation), else allocated at the IRM */
    void (*progress)(void *user, const char *step, int percent);
    void *progress_user;
    pout_link_avc_fn avc;
    void *avc_user;
    pout_link_plug_fn plug;
    void *plug_user;
} pout_link_cfg_t;

/* Opens the device (taking the cross-process lock), uploads the design, brings up the link with
 * the AV/C unit directory in the config ROM and installs the request hook. 0 on success; on
 * failure -1 with a sentence in err and nothing left open. */
int pout_link_open(pout_link_t **out, const pout_link_cfg_t *cfg, char *err, size_t err_cap);
void pout_link_close(pout_link_t *k);   /* stops the stream, releases the IRM resources, closes, unlocks */

uint16_t pout_link_node(const pout_link_t *k);   /* our node ID (0xffc0 | n) */
int pout_link_node_count(const pout_link_t *k);
/* SelfIDCount's selfIDGeneration: changes with every bus reset (-1 on a read error). A reset
 * stops the IT context and renumbers the nodes: the session should stop, call
 * pout_link_rescan(), and restart. */
int pout_link_bus_generation(pout_link_t *k);
/* Re-reads our node ID / node count after a bus reset and resets the plugs: connections do not
 * survive a bus reset (IEC 61883-1), and the IRM frees the channel and bandwidth, so a local
 * connection has to be made again (pout_link_local_connect) and a controller reconnects by itself.
 * The stream must have been stopped (pout_link_it_stop) before. */
int pout_link_rescan(pout_link_t *k);

/* Reads EP 0x84 once (up to timeout_ms), then answers the requests that arrived: plug register
 * reads and locks, FCP writes (with the AV/C callback). Returns the number of requests handled,
 * -1 on a USB error. */
int pout_link_pump(pout_link_t *k, unsigned timeout_ms);

/* The plug state (pout_csr_tx()). */
int pout_link_tx(const pout_link_t *k, unsigned *channel, unsigned *speed);

/* Sends an AV/C frame to `dest`'s FCP_RESPONSE (an INTERIM followed by the final response, or an
 * unsolicited NOTIFY response). 0 on success, -1 if the controller did not ack it. */
int pout_link_send_fcp(pout_link_t *k, uint16_t dest, const uint8_t *frame, unsigned len);

/* --- IRM (IEC 61883-1 resource allocation) --- */

/* Bandwidth units a stream of packets up to max_packet bytes at OHCI speed code `speed` needs. */
unsigned pout_link_bandwidth_units(unsigned max_packet, unsigned speed);
/* Takes the lowest free channel (0..62) and the bandwidth with compare-swap locks on the IRM's
 * CHANNELS_AVAILABLE and BANDWIDTH_AVAILABLE registers (FUN_0003f850); on failure nothing is held.
 * Returns the channel or -1. */
int pout_link_irm_alloc(pout_link_t *k, unsigned bandwidth_units);
void pout_link_irm_release(pout_link_t *k, unsigned channel, unsigned bandwidth_units);
/* on = 1: the broadcast connection in oPCR0 (pout_csr_local_connect) on cfg.channel, after allocating
 * that channel and the bandwidth at the IRM unless it is 63 (the broadcast channel needs neither; a
 * receiver listens on it by default); on = 0: back. Returns the channel / 0 / -1. */
int pout_link_local_connect(pout_link_t *k, int on);

/* --- isochronous transmit --- */

/* Clears the IT context, resets the pout_it state for this channel / speed and starts the
 * sender thread. Call pout_link_cycle_timer() just before to put the first packet's cycle
 * a few ms ahead. */
int pout_link_it_start(pout_link_t *k, unsigned channel, unsigned speed);
/* Adds one isochronous packet (CIP header + payload, wire order) to the stream. Blocks (up to
 * ~2 s) while the queue to the sender is full, which paces the caller to the bus rate. 0, or -1
 * if the stream has failed (then call pout_link_it_stop()). */
int pout_link_it_packet(pout_link_t *k, const uint8_t *pkt, unsigned len);
/* Hands the partly filled chunk to the sender now (call at the end of a frame period). */
int pout_link_it_flush(pout_link_t *k);
/* Flushes, waits for the queue to drain, clears the IT context and waits for it to go idle. */
int pout_link_it_stop(pout_link_t *k);
int pout_link_it_running(const pout_link_t *k);

/* IsochronousCycleTimer (OHCI 0xF0): cycleSeconds 31:25, cycleCount 24:12, cycleOffset 11:0.
 * *cycle gets seconds * 8000 + count (what pout_cip_set_cycle() takes), *offset the 12-bit
 * offset. 0 or -1. One EP 0x02 round trip, a millisecond or two of uncertainty: read it
 * rarely and keep time with the host clock in between. */
int pout_link_cycle_timer(pout_link_t *k, uint32_t *cycle, unsigned *offset);

#ifdef __cplusplus
}
#endif

#endif /* POUT_HW_LINK_H */
