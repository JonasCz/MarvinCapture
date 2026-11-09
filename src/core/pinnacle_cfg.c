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

#include "pinnacle_cfg.h"
#include "pinnacle_analog.h"
#include "pin_log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CFG_TIMEOUT_MS 2000
#define FPGA_BITSTREAM_BYTES 78422
#define FPGA_CHUNK 16384

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void sleep_ms(unsigned ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

int pinnacle_cfg_reply_matches(const uint8_t *req, int req_len, const uint8_t *reply, int n)
{
    if (req_len < 1 || n < 1 || reply[0] != req[0])
        return 0;
    /* "80 idx 08": the 20-byte reply repeats the request header before the 8 bytes */
    if (req[0] == 0x80 && req_len >= 3)
        return n >= 12 && reply[1] == req[1] && reply[2] == req[2];
    return 1;
}

/* Replies that belong to an earlier request (one whose reply came after its read had timed
 * out) are read past, so every later exchange is not one reply off. */
#define CFG_STALE_REPLIES 4

pinnacle_status_t pinnacle_cfg_xfer(pinnacle_device_t *dev, const uint8_t *req, int req_len,
                                    uint8_t *reply, int reply_cap, int *reply_len)
{
    uint8_t scratch[64];
    int n = 0;

    int rc = libusb_bulk_transfer(dev->handle, PINNACLE_EP_CONFIG_OUT, (uint8_t *)req, req_len,
                                   &n, CFG_TIMEOUT_MS);
    if (rc != 0 || n != req_len) {
        pin_logf(PIN_LOG_ERROR, "pinnacle: config write %02x failed (%s)\n", req[0], libusb_error_name(rc));
        return PINNACLE_ERR_USB_TRANSFER;
    }
    if (!reply) {
        reply = scratch;
        reply_cap = sizeof(scratch);
    }
    for (int i = 0; i <= CFG_STALE_REPLIES; i++) {
        n = 0;
        rc = libusb_bulk_transfer(dev->handle, PINNACLE_EP_CONFIG_IN, reply, reply_cap, &n,
                                   CFG_TIMEOUT_MS);
        if (rc != 0) {
            pin_logf(PIN_LOG_ERROR, "pinnacle: config reply to %02x failed (%s)\n", req[0],
                    libusb_error_name(rc));
            return PINNACLE_ERR_USB_TRANSFER;
        }
        if (pinnacle_cfg_reply_matches(req, req_len, reply, n)) {
            if (reply_len)
                *reply_len = n;
            return PINNACLE_OK;
        }
        pin_logf(PIN_LOG_DEBUG, "pinnacle: config reply %02x (%d bytes) does not answer %02x, "
                 "reading on\n", n > 0 ? reply[0] : 0, n, req[0]);
    }
    pin_logf(PIN_LOG_ERROR, "pinnacle: no matching config reply to %02x\n", req[0]);
    return PINNACLE_ERR_USB_TRANSFER;
}

pinnacle_status_t pinnacle_cfg_op(pinnacle_device_t *dev, uint8_t op, uint8_t arg,
                                  uint8_t *result)
{
    uint8_t req[2] = { op, arg }, reply[64];
    int n = 0;
    pinnacle_status_t st = pinnacle_cfg_xfer(dev, req, 2, reply, sizeof(reply), &n);
    if (st != PINNACLE_OK)
        return st;
    if (result) {
        if (n < 2) {
            pin_logf(PIN_LOG_ERROR, "pinnacle: config reply to %02x %02x is %d byte(s) long\n",
                     op, arg, n);
            return PINNACLE_ERR_USB_TRANSFER;
        }
        *result = reply[1];
    }
    return PINNACLE_OK;
}

/* The reply to a write is "01 01 08", 01 = acknowledged (as for a read); anything else is a
 * NAK: the chip did not take the value. */
pinnacle_status_t pinnacle_i2c_write(pinnacle_device_t *dev, uint8_t addr, uint8_t sub,
                                     uint8_t val)
{
    uint8_t req[5] = { 0x01, addr, 2, sub, val }, reply[64];
    int n = 0;
    pinnacle_status_t st = pinnacle_cfg_xfer(dev, req, sizeof(req), reply, sizeof(reply), &n);
    if (st != PINNACLE_OK)
        return st;
    if (n < 2 || reply[1] != 0x01) {
        pin_logf(PIN_LOG_ERROR, "pinnacle: I2C write %02x.%02x = %02x not acknowledged "
                 "(reply %02x %02x, %d bytes)\n", addr, sub, val, reply[0], n >= 2 ? reply[1] : 0, n);
        return PINNACLE_ERR_USB_TRANSFER;
    }
    return PINNACLE_OK;
}

pinnacle_status_t pinnacle_i2c_read(pinnacle_device_t *dev, uint8_t addr, uint8_t sub,
                                    uint8_t *val)
{
    uint8_t req[5] = { 0x02, addr, 1, 1, sub }, reply[64];
    int n = 0;
    pinnacle_status_t st = pinnacle_cfg_xfer(dev, req, sizeof(req), reply, sizeof(reply), &n);
    if (st != PINNACLE_OK)
        return st;
    if (n < 3 || reply[0] != 0x02 || reply[1] != 0x01)
        return PINNACLE_ERR_USB_TRANSFER;   /* not acknowledged */
    *val = reply[2];
    return PINNACLE_OK;
}

pinnacle_status_t pinnacle_cfg_chip_reset(pinnacle_device_t *dev, uint8_t addr)
{
    pinnacle_status_t st = pinnacle_cfg_op(dev, 0x03, addr, NULL);
    if (st == PINNACLE_OK)
        st = pinnacle_cfg_op(dev, 0x04, addr, NULL);
    return st;
}

pinnacle_status_t pinnacle_fpga_load(pinnacle_device_t *dev, const char *path, int fast)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        pin_logf(PIN_LOG_ERROR, "pinnacle: cannot open bitstream '%s': %s\n", path, strerror(errno));
        return PINNACLE_ERR_BITSTREAM_READ;
    }
    uint8_t *buf = malloc(FPGA_BITSTREAM_BYTES + 1);   /* + 1: a longer file is refused */
    size_t len = buf ? fread(buf, 1, FPGA_BITSTREAM_BYTES + 1, f) : 0;
    fclose(f);
    if (len != FPGA_BITSTREAM_BYTES) {
        pin_logf(PIN_LOG_ERROR, "pinnacle: bitstream '%s' is not %d bytes\n", path, FPGA_BITSTREAM_BYTES);
        free(buf);
        return PINNACLE_ERR_BITSTREAM_READ;
    }

    pinnacle_status_t st = PINNACLE_OK;
    uint8_t ready = 0;
    if (libusb_set_interface_alt_setting(dev->handle, PINNACLE_INTERFACE_NUM, 0) != 0) {
        st = PINNACLE_ERR_USB_TRANSFER;
        goto out;
    }
    st = pinnacle_cfg_op(dev, 0x05, 0x00, &ready);
    if (st != PINNACLE_OK)
        goto out;
    if (ready != 0x01) {
        pin_logf(PIN_LOG_ERROR, "pinnacle: FPGA loader not ready (05 -> %02x)\n", ready);
        st = PINNACLE_ERR_NOT_READY;
        goto out;
    }

    pin_logf(PIN_LOG_DEBUG, "pinnacle: uploading the FPGA bitstream '%s' (%zu bytes)\n", path, len);
    pinnacle_progress(dev, "Uploading FPGA firmware", 0);
    for (size_t off = 0; off < len; off += FPGA_CHUNK) {
        pinnacle_progress(dev, "Uploading FPGA firmware", (int)(off * 100 / len));
        int chunk = (int)(len - off < FPGA_CHUNK ? len - off : FPGA_CHUNK), n = 0;
        int rc = libusb_bulk_transfer(dev->handle, PINNACLE_EP_CMD_OUT, buf + off, chunk, &n,
                                       CFG_TIMEOUT_MS);
        if (rc != 0 || n != chunk) {
            pin_logf(PIN_LOG_ERROR, "pinnacle: bitstream upload failed at %zu (%s)\n", off,
                    libusb_error_name(rc));
            st = PINNACLE_ERR_USB_TRANSFER;
            goto out;
        }
    }

    /* The vendor driver sleeps 10 ms here, but its traces show ~1 s between
     * the last bitstream byte and "06 00"; pinnacle_device.c found that
     * asking too early wedges the DV design. Keep the long wait. */
    pinnacle_progress(dev, "Waiting for the FPGA to start up", -1);
    if (fast) {
        /* Over a running design: the FPGA is configured when the upload returns (the
         * transfer itself takes ~1 s) and the capture block's I2C slave answers a
         * millisecond later (measured 0..1 ms on both machines). Poll for it, then give
         * it 100 ms more; with no answer the whole 1.1 s has passed. */
        uint64_t t0 = now_ms();
        int acked = 0;
        if (libusb_set_interface_alt_setting(dev->handle, PINNACLE_INTERFACE_NUM,
                                             PINNACLE_ALT_SETTING_CAPTURE) == 0) {
            while (now_ms() - t0 < 1100) {
                uint8_t v = 0;
                if (pinnacle_i2c_read(dev, 0xf0, 1, &v) == PINNACLE_OK) {
                    acked = 1;
                    break;
                }
                sleep_ms(10);
            }
        }
        if (acked) {
            pin_logf(PIN_LOG_DEBUG, "pinnacle: capture block answered %u ms after the upload\n",
                     (unsigned)(now_ms() - t0));
            sleep_ms(100);
        } else {
            pin_logf(PIN_LOG_DEBUG, "pinnacle: capture block did not answer, waited the full 1.1 s\n");
            uint64_t spent = now_ms() - t0;
            if (spent < 1100)
                sleep_ms((unsigned)(1100 - spent));
        }
    } else {
        sleep_ms(1100);
    }

    st = pinnacle_cfg_op(dev, 0x06, 0x00, &ready);
    if (st == PINNACLE_OK && ready != 0x01) {
        pin_logf(PIN_LOG_ERROR, "pinnacle: FPGA did not come up (06 -> %02x)\n", ready);
        st = PINNACLE_ERR_NOT_READY;
    } else if (st == PINNACLE_OK) {
        pin_logf(PIN_LOG_DEBUG, "pinnacle: FPGA up (06 -> %02x)\n", ready);
    }
out:
    free(buf);
    return st;
}
