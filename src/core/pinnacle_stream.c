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

#include "pinnacle_stream.h"
#include "pinnacle_1394.h"
#include "pin_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* winpthreads gives MinGW/UCRT64 the same pthread calls, nanosleep() and
 * clock_gettime() as any other POSIX target, so this file no longer needs a
 * _WIN32 fork: one code path, built the same everywhere the core supports
 * (Linux, and now Windows via MSYS2 UCRT64). */
#include <pthread.h>
#include <time.h>

static void pinnacle_sleep_ms(unsigned ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

#define CMD_TIMEOUT_MS 2000

/* Reads EP 0x84 until it goes quiet. Many command packets set the
 * "reply wanted" bit, and the device answers on this endpoint; leaving those
 * replies unread lets the FX2's IN buffer fill, at which point the command
 * processor stops accepting writes on EP 0x02 and every later command times
 * out. Draining once per command is not enough — a single command can
 * produce several replies. */
static void drain_replies(pinnacle_device_t *dev, unsigned timeout_ms)
{
    uint8_t reply[512];

    for (int i = 0; i < 64; i++) {
        int len = 0;
        int rc = libusb_bulk_transfer(dev->handle, PINNACLE_EP_CMD_IN, reply, sizeof(reply),
                                       &len, timeout_ms);
        if (rc != 0 || len == 0)
            return;
    }
}

/* --- EP 0x88 back-pressure ------------------------------------------------
 *
 * The command channel (EP 0x02 OUT) stops accepting writes whenever the DV
 * endpoint has a backlog nobody is reading. Observed twice, identically:
 *
 *   - the stop sequence times out on packet 3 of 4 (rc=-7) if we tear the
 *     read loop down first, and completes if we keep reading;
 *   - probe_registers(), which runs straight after the start sequence,
 *     succeeds when the camera is idle (no DV flowing) and every single
 *     write times out when the camera is playing.
 *
 * Same endpoint, same errno, and the only variable is whether DV is piling
 * up. The vendor driver never stops reading EP 0x88 while the isochronous
 * receive context is in "run" — it keeps a deep queue of URBs outstanding
 * throughout, including across its own shutdown sequence.
 *
 * So anything that talks on EP 0x02 while the stream is live must keep EP
 * 0x88 drained. That's what this thread is for: it reads and discards until
 * told to stop.
 */
struct dv_drain {
    pinnacle_device_t *dev;
    volatile int stop;
    unsigned long bytes;
};

static void *dv_drain_thread(void *arg)
{
    struct dv_drain *d = arg;
    uint8_t buf[32768];

    while (!d->stop) {
        int n = 0;
        int rc = libusb_bulk_transfer(d->dev->handle, PINNACLE_EP_DV_IN, buf, sizeof(buf),
                                       &n, 100);
        if (rc == 0 && n > 0)
            d->bytes += (unsigned long)n;
        else if (rc != 0 && rc != LIBUSB_ERROR_TIMEOUT)
            break;
    }
    return NULL;
}

static int dv_drain_start(struct dv_drain *d, pthread_t *tid, pinnacle_device_t *dev)
{
    d->dev = dev;
    d->stop = 0;
    d->bytes = 0;
    return pthread_create(tid, NULL, dv_drain_thread, d) == 0;
}

static void dv_drain_join(struct dv_drain *d, pthread_t tid, int started)
{
    if (!started)
        return;
    d->stop = 1;
    pthread_join(tid, NULL);
}

/* Reads a handful of OHCI registers and prints the replies, to tell "our
 * receiver is broken" apart from "the camera stopped sending". Command word
 * 0x38.. is an OHCI register read with the reply-wanted bit set and tag 1;
 * the address field is 0x10000 + the register offset. The reply comes back on
 * EP 0x84 as a type-3 message: the echoed command word then the value.
 * Enable with PINNACLE_PROBE=1. */
static void probe_registers(pinnacle_device_t *dev)
{
    static const struct { const char *name; uint16_t off; } regs[] = {
        { "IntEvent",        0x080 },
        { "SelfIDCount",     0x068 },
        { "LinkControlSet",  0x0E0 },
        { "NodeID",          0x0E8 },
        { "CycleTimer",      0x0F0 },
        { "IR0.CtrlSet",     0x400 },
        { "IR0.Match",       0x410 },
    };

    drain_replies(dev, 50);

    /* The probe runs after the start sequence, so DV may already be flowing;
     * without this every register write times out. See the back-pressure
     * comment above. */
    struct dv_drain drain;
    pthread_t drain_thread;
    int drain_started = dv_drain_start(&drain, &drain_thread, dev);

    for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        uint32_t cmd = 0x38000000u | (1u << 20) | (0x10000u + regs[i].off);
        uint8_t pkt[8] = {
            (uint8_t)(cmd), (uint8_t)(cmd >> 8), (uint8_t)(cmd >> 16), (uint8_t)(cmd >> 24),
            0, 0, 0, 0
        };
        int n = 0;
        int rc = libusb_bulk_transfer(dev->handle, PINNACLE_EP_CMD_OUT, pkt, sizeof(pkt),
                                       &n, CMD_TIMEOUT_MS);
        if (rc != 0) {
            pin_logf(PIN_LOG_DEBUG, "probe %-15s write failed: %s\n", regs[i].name, libusb_error_name(rc));
            continue;
        }

        uint8_t reply[512];
        int len = 0;
        rc = libusb_bulk_transfer(dev->handle, PINNACLE_EP_CMD_IN, reply, sizeof(reply),
                                   &len, 500);
        if (rc != 0 || len < 8) {
            pin_logf(PIN_LOG_DEBUG, "probe %-15s (0x%03x): no reply (%s, len=%d)\n",
                    regs[i].name, regs[i].off, libusb_error_name(rc), len);
            continue;
        }
        uint32_t val = (uint32_t)reply[4] | ((uint32_t)reply[5] << 8) |
                       ((uint32_t)reply[6] << 16) | ((uint32_t)reply[7] << 24);
        pin_logf(PIN_LOG_DEBUG, "probe %-15s (0x%03x) = 0x%08x\n", regs[i].name, regs[i].off, val);
    }

    dv_drain_join(&drain, drain_thread, drain_started);
}

/* Records, for the user, which bring-up step failed and why. */
static void stream_fail(pinnacle_device_t *dev, const pinnacle_1394_t *l, const char *what)
{
    const char *step = l->step ? l->step : "starting";
    if (l->last_usb_rc)
        snprintf(dev->fail_detail, sizeof(dev->fail_detail),
                 "%s while %s: USB transfer error (%s). Is the deck's FireWire cable seated, "
                 "and the deck on?", what, step, libusb_error_name(l->last_usb_rc));
    else
        snprintf(dev->fail_detail, sizeof(dev->fail_detail),
                 "%s while %s. Check the FireWire cable and that the deck is on; the deck's "
                 "FireWire port may be faulty.", what, step);
    pin_logf(PIN_LOG_ERROR, "pinnacle: %s\n", dev->fail_detail);
}

/* Link up, find the camera, connect to its output plug, and start
 * isochronous receive on the channel the plug reports. Every step is
 * explained in docs/startup.md.
 *
 * This used to be a verbatim replay of 232 packets from a Windows trace.
 * Besides the link setup it contained Windows' camera enumeration (config
 * ROM reads) and ~45 AV/C inquiries with the tlabels of the camera it was
 * recorded against; this camera answered them, and the leftovers confused
 * the first deck commands. None of it was needed for capture. */
pinnacle_status_t pinnacle_stream_start(pinnacle_device_t *dev)
{
    pinnacle_1394_t link;
    uint32_t ompr = 0, opcr = 0;

    p1394_init(&link, dev);
    link.verbose = dev->tuning.debug_1394;
    dev->camera_node = 0;
    dev->pcr_connected = 0;
    dev->iso_channel = 63;

    pinnacle_progress(dev, "Starting the FireWire (1394) link", -1);
    if (p1394_link_init(&link) != 0) {
        stream_fail(dev, &link, "FireWire link init failed");
        return PINNACLE_ERR_USB_TRANSFER;
    }
    dev->node_count = link.node_count;
    pin_logf(PIN_LOG_INFO, "pinnacle: 1394 bus has %d node(s), we are node %u\n",
            link.node_count, link.local_node & 0x3f);

    pinnacle_progress(dev, "Looking for a camera on the FireWire bus", -1);
    if (p1394_find_camera(&link, &dev->camera_node, &ompr) != 0) {
        dev->camera_node = 0;
        pin_logf(PIN_LOG_INFO, "pinnacle: no camera answered on the 1394 bus; listening on "
                        "broadcast channel 63 anyway\n");
    } else if (pinnacle_progress(dev, "Connecting to the camera", -1),
               p1394_connect(&link, dev->camera_node, &opcr) == 0) {
        dev->pcr_connected = 1;
        dev->iso_channel = (int)((opcr >> 16) & 0x3f);
        pin_logf(PIN_LOG_INFO, "pinnacle: camera is node %u (oMPR 0x%08x); connected to oPCR[0] "
                        "0x%08x, channel %d\n", dev->camera_node & 0x3f, ompr, opcr,
                dev->iso_channel);
    } else {
        /* still listen on whatever channel the plug names, if we got it */
        if (opcr & 0x80000000u)
            dev->iso_channel = (int)((opcr >> 16) & 0x3f);
        pin_logf(PIN_LOG_WARN, "pinnacle: camera is node %u but connecting to oPCR[0] failed "
                        "(0x%08x); listening on channel %d\n",
                dev->camera_node & 0x3f, opcr, dev->iso_channel);
    }

    pinnacle_progress(dev, "Starting video reception", -1);
    if (p1394_ir_start(&link, (unsigned)dev->iso_channel) != 0) {
        stream_fail(dev, &link, "FireWire receive start failed");
        return PINNACLE_ERR_USB_TRANSFER;
    }

    if (dev->tuning.probe_registers)
        probe_registers(dev);

    return PINNACLE_OK;
}

pinnacle_status_t pinnacle_stream_stop(pinnacle_device_t *dev)
{
    /* Clear out anything the device queued while we were streaming so the
     * command channel starts the stop unblocked. */
    drain_replies(dev, 50);

    /* Keep EP 0x88 moving throughout — see the comment above. Data keeps
     * arriving until IR0 is out of "run". */
    struct dv_drain drain;
    pthread_t drain_thread;
    int drain_started = dv_drain_start(&drain, &drain_thread, dev);

    pinnacle_status_t status = PINNACLE_OK;
    pinnacle_1394_t link;
    p1394_init(&link, dev);
    link.verbose = dev->tuning.debug_1394;

    /* isochronous-to-USB off, IR0 out of run, wait for it to go idle */
    if (p1394_ir_stop(&link) != 0) {
        status = PINNACLE_ERR_USB_TRANSFER;
    } else {
        uint32_t ctl = 0;
        for (int i = 0; i < 50; i++) {
            if (p1394_reg_read(&link, 0x400, &ctl) == 0 && !(ctl & 0x400))
                break;
            pinnacle_sleep_ms(2);
        }
        /* hand the camera's plug back */
        if (dev->pcr_connected && dev->camera_node) {
            uint32_t opcr = 0;
            if (p1394_disconnect(&link, dev->camera_node, &opcr) == 0) {
                dev->pcr_connected = 0;
                if (link.verbose)
                    pin_logf(PIN_LOG_DEBUG, "pinnacle: released oPCR[0], now 0x%08x\n", opcr);
            } else
                pin_logf(PIN_LOG_INFO, "pinnacle: releasing the camera's oPCR[0] failed (0x%08x)\n",
                        opcr);
        }
    }

    dv_drain_join(&drain, drain_thread, drain_started);
    if (drain_started && drain.bytes)
        pin_logf(PIN_LOG_INFO, "pinnacle: discarded %lu bytes of DV during stop\n", drain.bytes);

    return status;
}

/* --- the capture read loop -----------------------------------------------
 *
 * Synchronous libusb_bulk_transfer() leaves the endpoint unserviced between
 * the return of one transfer and the submission of the next. At 3.6 MB/s with
 * 32 KB reads that is a gap every ~9 ms, and DV arriving in it has nowhere to
 * go: the FX2's FIFO backs up and data is lost. Measured over a 20 s capture,
 * that cost one 3-sequence hole -- ~0.05% of data blocks, confirmed
 * independently by the CIP/DBC continuity counter.
 *
 * The vendor driver never does this; it keeps a deep queue of URBs
 * outstanding so the endpoint is always serviced. We do the same: a ring of
 * asynchronous transfers, all submitted up front, each resubmitted from its
 * own completion callback.
 *
 * Completion order is not guaranteed but delivery order matters (the DV
 * reassembler needs a byte-exact stream), so the ring is consumed strictly in
 * submission order: the loop only takes slots[head] once its callback has
 * marked it done.
 *
 * The EP 0x84 status drain rides the same event loop rather than a thread of
 * its own, so exactly one thread ever calls libusb_handle_events().
 */

/* Depth is counted in transfers, not bytes: the device ends each transfer with
 * a short packet after ~4 KB (8 KB at most), so a slot holds far less than its
 * buffer size. 256 slots is ~1 MB of stream, ~290 ms of slack at HDV/DV rates.
 * Measured: 32 slots (~36 ms) overflowed whenever a disk flush stalled the
 * consumer for longer than that, and the FPGA then dropped ~25 bus cycles.
 * Buffers stay small so the total (4 MB) is well inside usbfs_memory_mb (16). */
#define DV_QUEUE_DEPTH 256
#define DV_XFER_BYTES  16384

struct rx_slot {
    struct libusb_transfer *xfer;
    volatile int submitted;    /* libusb owns the buffer while this is set */
    volatile int done;         /* set by the callback, cleared on resubmit */
    volatile int failed;
    int status;                /* libusb transfer status, when it failed */
    struct timespec t_done;    /* when the completion callback ran (debug timing) */
};

static void LIBUSB_CALL dv_xfer_cb(struct libusb_transfer *xfer)
{
    struct rx_slot *slot = xfer->user_data;

    clock_gettime(CLOCK_MONOTONIC, &slot->t_done);
    slot->status = xfer->status;
    slot->failed = (xfer->status != LIBUSB_TRANSFER_COMPLETED);
    slot->submitted = 0;
    slot->done = 1;
}

/* EP 0x84 carries unsolicited status/event records while streaming: a real
 * vendor-driver capture of a steady session has them arriving seconds apart
 * with no host request to prompt them, and MarvinBus64 has a worker thread for
 * exactly this. Leaving them unread lets the FX2's IN buffer fill, which is
 * one way to stop the command processor accepting writes. One transfer,
 * resubmitted forever, costs nothing.
 * PINNACLE_DEBUG_EP84=1 logs each record. */
struct ep84_state {
    unsigned char buf[512];
    volatile int active;
    int log;
    struct timespec t0;
    unsigned long packets;
    unsigned long bytes;
    pinnacle_1394_t *link;   /* non-NULL: also feed completions to p1394_parse_ep84() */
};

static void LIBUSB_CALL ep84_cb(struct libusb_transfer *xfer)
{
    struct ep84_state *st = xfer->user_data;

    if (xfer->status == LIBUSB_TRANSFER_COMPLETED && xfer->actual_length > 0) {
        st->packets++;
        st->bytes += (unsigned long)xfer->actual_length;
        if (st->link)
            p1394_parse_ep84(st->link, xfer->buffer, xfer->actual_length);
        if (st->log) {
            /* One fwrite, not a sequence of fprintfs: stderr is shared with
             * the EP 0x88 logging and interleaved fragments are unparseable. */
            char line[1400];
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double ms = (now.tv_sec - st->t0.tv_sec) * 1000.0 +
                        (now.tv_nsec - st->t0.tv_nsec) / 1e6;
            int off = snprintf(line, sizeof(line), "ep84 t=%.3fms len=%d ",
                               ms, xfer->actual_length);
            for (int i = 0; i < xfer->actual_length && off + 3 < (int)sizeof(line); i++)
                off += snprintf(line + off, sizeof(line) - off, "%02x", xfer->buffer[i]);
            off += snprintf(line + off, sizeof(line) - off, "\n");
            pin_logf(PIN_LOG_DEBUG, "%s", line);
        }
    }

    if (xfer->status == LIBUSB_TRANSFER_CANCELLED ||
        xfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
        st->active = 0;
        return;
    }
    if (libusb_submit_transfer(xfer) != 0)
        st->active = 0;
}

pinnacle_status_t pinnacle_stream_read_loop(pinnacle_device_t *dev,
                                             pinnacle_data_cb cb, void *user,
                                             volatile int *stop_flag)
{
    return pinnacle_stream_read_loop_ex(dev, cb, user, stop_flag, NULL, NULL, NULL);
}

pinnacle_status_t pinnacle_stream_read_loop_ex(pinnacle_device_t *dev,
                                                pinnacle_data_cb cb, void *user,
                                                volatile int *stop_flag,
                                                pinnacle_1394_t *link,
                                                pinnacle_stream_tick_fn tick, void *tick_user)
{
    /* PINNACLE_DEBUG_EP88=1 logs every completion's size and arrival time, to
     * characterise the raw receive pattern independently of usbmon (which was
     * found unreliable for this process's own high-frequency reads). */
    int debug = dev->tuning.debug_ep88;
    struct timespec t0, tnow;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    const char *raw_dump_path = dev->tuning.raw_dump_path;
    FILE *raw_dump = raw_dump_path ? fopen(raw_dump_path, "wb") : NULL;

    unsigned depth = DV_QUEUE_DEPTH;

    struct rx_slot slots[DV_QUEUE_DEPTH];
    memset(slots, 0, sizeof(slots));

    pinnacle_status_t status = PINNACLE_OK;
    unsigned allocated = 0;

    for (unsigned i = 0; i < depth; i++) {
        struct libusb_transfer *xfer = libusb_alloc_transfer(0);
        unsigned char *buf = xfer ? malloc(DV_XFER_BYTES) : NULL;
        if (!xfer || !buf) {
            if (xfer)
                libusb_free_transfer(xfer);
            free(buf);
            break;
        }
        libusb_fill_bulk_transfer(xfer, dev->handle, PINNACLE_EP_DV_IN,
                                   buf, DV_XFER_BYTES, dv_xfer_cb,
                                   &slots[i], 0 /* no timeout: stop is by cancel */);
        slots[i].xfer = xfer;
        allocated++;
    }
    if (allocated == 0) {
        if (raw_dump)
            fclose(raw_dump);
        return PINNACLE_ERR_USB_TRANSFER;
    }

    unsigned submitted = 0;
    for (unsigned i = 0; i < allocated; i++) {
        if (libusb_submit_transfer(slots[i].xfer) != 0) {
            slots[i].done = 1;
            slots[i].failed = 1;
            slots[i].status = LIBUSB_TRANSFER_ERROR;
        } else {
            slots[i].submitted = 1;
            submitted++;
        }
    }
    if (submitted == 0)
        status = PINNACLE_ERR_USB_TRANSFER;

    /* EP 0x84 drain, on this same event loop (async AV/C, p1394_avc_begin/poll,
     * has nothing to poll without it). */
    struct ep84_state ep84;
    memset(&ep84, 0, sizeof(ep84));
    ep84.log = dev->tuning.debug_ep84;
    ep84.t0 = t0;
    ep84.link = link;
    struct libusb_transfer *ep84_xfer = NULL;
    {
        ep84_xfer = libusb_alloc_transfer(0);
        if (ep84_xfer) {
            libusb_fill_bulk_transfer(ep84_xfer, dev->handle, PINNACLE_EP_CMD_IN,
                                       ep84.buf, sizeof(ep84.buf), ep84_cb,
                                       &ep84, 0);
            if (libusb_submit_transfer(ep84_xfer) == 0)
                ep84.active = 1;
        }
    }

    unsigned long completions = 0;
    unsigned head = 0;
    int finished = (status != PINNACLE_OK);

    while (!*stop_flag && !finished) {
        struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 };
        if (libusb_handle_events_timeout_completed(dev->usb_ctx, &tv, NULL) != 0)
            break;

        /* Any EP 0x84 bytes from this pass already reached p1394_parse_ep84()
         * via ep84_cb above (libusb_handle_events_timeout_completed() is what
         * runs the completion callbacks), so it's safe for the tick to poll
         * p1394_avc_poll() / check transport state now. */
        if (tick)
            tick(tick_user);

        /* Deliver in submission order, whatever order completions arrived. */
        while (slots[head].done) {
            struct rx_slot *slot = &slots[head];
            struct libusb_transfer *xfer = slot->xfer;

            if (slot->failed) {
                if (!*stop_flag && slot->status != LIBUSB_TRANSFER_CANCELLED) {
                    pin_logf(PIN_LOG_ERROR, "pinnacle: EP 0x88 transfer failed (status %d)\n",
                            slot->status);
                    status = PINNACLE_ERR_USB_TRANSFER;
                }
                finished = 1;
                break;
            }

            int n = xfer->actual_length;
            if (n > 0) {
                completions++;
                if (debug) {
                    clock_gettime(CLOCK_MONOTONIC, &tnow);
                    double ms = (tnow.tv_sec - t0.tv_sec) * 1000.0 +
                                 (tnow.tv_nsec - t0.tv_nsec) / 1e6;
                    double cms = (slot->t_done.tv_sec - t0.tv_sec) * 1000.0 +
                                 (slot->t_done.tv_nsec - t0.tv_nsec) / 1e6;
                    char line[128];
                    snprintf(line, sizeof(line),
                             "ep88 t=%.3fms size=%d n=%lu c=%.3f\n",
                             ms, n, completions, cms);
                    pin_logf(PIN_LOG_DEBUG, "%s", line);
                }
                if (raw_dump)
                    fwrite(xfer->buffer, 1, (size_t)n, raw_dump);
                struct timespec tcb0, tcb1;
                if (debug)
                    clock_gettime(CLOCK_MONOTONIC, &tcb0);
                if (cb(xfer->buffer, (size_t)n, user) != 0)
                    finished = 1;
                if (debug) {
                    clock_gettime(CLOCK_MONOTONIC, &tcb1);
                    double us = (tcb1.tv_sec - tcb0.tv_sec) * 1e6 +
                                (tcb1.tv_nsec - tcb0.tv_nsec) / 1e3;
                    if (us > 1000.0) {
                        char sl[96];
                        snprintf(sl, sizeof(sl), "cbslow n=%lu us=%.0f\n",
                                 completions, us);
                        pin_logf(PIN_LOG_DEBUG, "%s", sl);
                    }
                }
            }

            slot->done = 0;
            if (finished || *stop_flag)
                break;
            if (libusb_submit_transfer(xfer) != 0) {
                slot->done = 1;
                slot->failed = 1;
                slot->status = LIBUSB_TRANSFER_ERROR;
                finished = 1;
                break;
            }
            slot->submitted = 1;
            head = (head + 1) % allocated;
        }
    }

    /* Cancel whatever libusb still owns and pump the event loop until every
     * callback has fired, so nothing writes into a buffer we are about to
     * free. Only `submitted` slots are outstanding — a slot we stopped on
     * mid-delivery has already completed and must not be cancelled, or we'd
     * wait out the whole spin for a callback that is never coming. */
    for (unsigned i = 0; i < allocated; i++) {
        if (slots[i].submitted)
            libusb_cancel_transfer(slots[i].xfer);
    }
    if (ep84_xfer && ep84.active)
        libusb_cancel_transfer(ep84_xfer);

    for (int spin = 0; spin < 200; spin++) {
        int pending = ep84.active ? 1 : 0;
        for (unsigned i = 0; i < allocated; i++) {
            if (slots[i].submitted)
                pending = 1;
        }
        if (!pending)
            break;
        struct timeval tv = { .tv_sec = 0, .tv_usec = 20000 };
        if (libusb_handle_events_timeout_completed(dev->usb_ctx, &tv, NULL) != 0)
            break;
    }

    for (unsigned i = 0; i < allocated; i++) {
        if (slots[i].xfer) {
            free(slots[i].xfer->buffer);
            libusb_free_transfer(slots[i].xfer);
        }
    }
    if (ep84_xfer)
        libusb_free_transfer(ep84_xfer);

    if (ep84.log)
        pin_logf(PIN_LOG_DEBUG, "ep84 total: %lu records, %lu bytes\n",
                ep84.packets, ep84.bytes);

    if (raw_dump)
        fclose(raw_dump);
    return status;
}
