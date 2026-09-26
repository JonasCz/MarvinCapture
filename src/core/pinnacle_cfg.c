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
#include "pin_log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CFG_TIMEOUT_MS 2000
#define FPGA_BITSTREAM_BYTES 78422
#define FPGA_CHUNK 16384

static void sleep_ms(unsigned ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

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
    rc = libusb_bulk_transfer(dev->handle, PINNACLE_EP_CONFIG_IN, reply, reply_cap, &n,
                               CFG_TIMEOUT_MS);
    if (rc != 0) {
        pin_logf(PIN_LOG_ERROR, "pinnacle: config reply to %02x failed (%s)\n", req[0],
                libusb_error_name(rc));
        return PINNACLE_ERR_USB_TRANSFER;
    }
    if (reply_len)
        *reply_len = n;
    return PINNACLE_OK;
}

pinnacle_status_t pinnacle_cfg_op(pinnacle_device_t *dev, uint8_t op, uint8_t arg,
                                  uint8_t *result)
{
    uint8_t req[2] = { op, arg }, reply[64];
    int n = 0;
    pinnacle_status_t st = pinnacle_cfg_xfer(dev, req, 2, reply, sizeof(reply), &n);
    if (st != PINNACLE_OK)
        return st;
    if (result)
        *result = n >= 2 ? reply[1] : 0;
    return PINNACLE_OK;
}

/* The reply to a write is "01 01 08"; the vendor driver does not look at it,
 * and neither do we beyond the transfer succeeding. */
pinnacle_status_t pinnacle_i2c_write(pinnacle_device_t *dev, uint8_t addr, uint8_t sub,
                                     uint8_t val)
{
    uint8_t req[5] = { 0x01, addr, 2, sub, val };
    return pinnacle_cfg_xfer(dev, req, sizeof(req), NULL, 0, NULL);
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

pinnacle_status_t pinnacle_fpga_load(pinnacle_device_t *dev, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        pin_logf(PIN_LOG_ERROR, "pinnacle: cannot open bitstream '%s': %s\n", path, strerror(errno));
        return PINNACLE_ERR_BITSTREAM_READ;
    }
    uint8_t *buf = malloc(FPGA_BITSTREAM_BYTES + 1);
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
    sleep_ms(1100);

    st = pinnacle_cfg_op(dev, 0x06, 0x00, &ready);
    if (st == PINNACLE_OK && ready != 0x01) {
        pin_logf(PIN_LOG_ERROR, "pinnacle: FPGA did not come up (06 -> %02x)\n", ready);
        st = PINNACLE_ERR_NOT_READY;
    }
out:
    free(buf);
    return st;
}
