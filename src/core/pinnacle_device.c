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

#include "pinnacle_device.h"
#include "pin_log.h"
#include "pinnacle_enum.h"
#include "protocol_data.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void pinnacle_tuning_defaults(pinnacle_tuning_t *t)
{
    memset(t, 0, sizeof(*t));
    t->debug_1394 = 0;
    t->probe_registers = 0;
    t->stop_drain = 1;      /* PINNACLE_STOP_DRAIN=0 turns this off */
    t->debug_ep88 = 0;
    t->raw_dump_path = NULL;
    t->queue_depth = 0;     /* 0 = use DV_QUEUE_DEPTH */
    t->debug_ep84 = 0;
    t->ep84_drain = 1;      /* PINNACLE_EP84_DRAIN=0 turns this off */
    t->video_queue = 0;     /* 0 = use VIDEO_QUEUE */
    t->video_xfer = 0;      /* 0 = use VIDEO_XFER */
    t->debug_analog = 0;
}

void pinnacle_tuning_from_env(pinnacle_tuning_t *t)
{
    const char *e;

    if ((e = getenv("PINNACLE_DEBUG_1394")))
        t->debug_1394 = atoi(e);
    if ((e = getenv("PINNACLE_PROBE")))
        t->probe_registers = (e[0] == '1');
    if ((e = getenv("PINNACLE_STOP_DRAIN")))
        t->stop_drain = !(e[0] == '0');
    if ((e = getenv("PINNACLE_DEBUG_EP88")))
        t->debug_ep88 = (e[0] == '1');
    if ((e = getenv("PINNACLE_RAW_DUMP")))
        t->raw_dump_path = e;
    if ((e = getenv("PINNACLE_QUEUE_DEPTH")))
        t->queue_depth = (unsigned)strtoul(e, NULL, 10);
    if ((e = getenv("PINNACLE_DEBUG_EP84")))
        t->debug_ep84 = (e[0] == '1');
    if ((e = getenv("PINNACLE_EP84_DRAIN")))
        t->ep84_drain = !(e[0] == '0');
    if ((e = getenv("PINNACLE_VIDEO_QUEUE")))
        t->video_queue = (unsigned)atoi(e);
    if ((e = getenv("PINNACLE_VIDEO_XFER")))
        t->video_xfer = (unsigned)atoi(e);
    if ((e = getenv("PINNACLE_DEBUG_ANALOG")))
        t->debug_analog = (e[0] == '1');
}

static void sleep_ms(unsigned ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

#define BULK_TIMEOUT_MS 2000

const char *pinnacle_strerror(pinnacle_status_t status)
{
    switch (status) {
    case PINNACLE_OK: return "ok";
    case PINNACLE_ERR_USB_INIT: return "libusb initialisation failed";
    case PINNACLE_ERR_NOT_FOUND: return "device 2304:0213 not found";
    case PINNACLE_ERR_USB_OPEN: return "failed to open device (check permissions, try sudo)";
    case PINNACLE_ERR_USB_CONFIG: return "failed to set USB configuration";
    case PINNACLE_ERR_USB_CLAIM: return "failed to claim interface (another driver/process attached?)";
    case PINNACLE_ERR_USB_TRANSFER: return "USB bulk transfer failed";
    case PINNACLE_ERR_BITSTREAM_READ: return "failed to read FPGA bitstream file";
    case PINNACLE_ERR_NOT_READY: return "device reports not ready (FPGA did not come up; needs a physical USB power cycle)";
    case PINNACLE_ERR_BUSY: return "device already open in another process";
    case PINNACLE_ERR_LOCK: return "internal locking error";
    }
    return "unknown error";
}

/* Finds the libusb_device matching device_id (NULL/"first" = first
 * supported-PID device) in ctx's current device list, without opening it.
 * Returns PINNACLE_OK with *match set (ref'd; caller must
 * libusb_unref_device() it) or PINNACLE_ERR_NOT_FOUND. list_out receives
 * the device list so the caller can free it after it's done with *match
 * (libusb_device* pointers from the list are only valid while it's alive,
 * or until individually ref'd, which we do here). */
static pinnacle_status_t find_device(libusb_context *ctx, const char *device_id,
                                      libusb_device **match_out)
{
    int use_first = (!device_id || strcmp(device_id, "first") == 0);

    libusb_device **list = NULL;
    ssize_t n = libusb_get_device_list(ctx, &list);
    pinnacle_status_t status = PINNACLE_ERR_NOT_FOUND;

    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) != 0)
            continue;
        if (desc.idVendor != PINNACLE_VID || desc.idProduct != PINNACLE_PID)
            continue; /* only 0213 is ever actually opened -- see pinnacle_enum.h for the rest of the model table */

        if (use_first) {
            *match_out = libusb_ref_device(list[i]);
            status = PINNACLE_OK;
            break;
        }

        char id[PINNACLE_ENUM_ID_MAX];
        pinnacle_enum_build_id(list[i], id, sizeof(id));
        if (strcmp(id, device_id) == 0) {
            *match_out = libusb_ref_device(list[i]);
            status = PINNACLE_OK;
            break;
        }
    }

    if (list)
        libusb_free_device_list(list, 1);
    return status;
}

pinnacle_status_t pinnacle_open_by_id(pinnacle_device_t *dev, const char *device_id)
{
    memset(dev, 0, sizeof(*dev));
    pinnacle_tuning_defaults(&dev->tuning);

    if (libusb_init(&dev->usb_ctx) != 0)
        return PINNACLE_ERR_USB_INIT;

    libusb_device *match = NULL;
    pinnacle_status_t status = find_device(dev->usb_ctx, device_id, &match);
    if (status != PINNACLE_OK) {
        libusb_exit(dev->usb_ctx);
        dev->usb_ctx = NULL;
        return status;
    }

    int rc = libusb_open(match, &dev->handle);
    libusb_unref_device(match);
    if (rc != 0) {
        dev->handle = NULL;
        libusb_exit(dev->usb_ctx);
        dev->usb_ctx = NULL;
        /* ACCESS/BUSY here means some other opener (another process without
         * our lock, or -- on POSIX -- a permissions problem) got there
         * first or blocks us outright; pinnacle_lock.[ch] is the primary
         * way callers should avoid racing this in the first place. */
        return (rc == LIBUSB_ERROR_ACCESS || rc == LIBUSB_ERROR_BUSY)
                   ? PINNACLE_ERR_BUSY : PINNACLE_ERR_USB_OPEN;
    }

    libusb_set_auto_detach_kernel_driver(dev->handle, 1);

    /* libusb_reset_device() was tried here to clear bad state without a
     * physical replug, but is suspect: a wedge reproduced even right after
     * a fresh physical replug, with byte-exact cold-boot commands, and a
     * soft USB reset on this device is one of the few remaining variables.
     * Disabled pending investigation -- see git history if reintroducing. */

    /* On Linux a second process gets as far as libusb_open(); it's setting
     * the configuration of an interface someone else has claimed that fails,
     * with LIBUSB_ERROR_BUSY. Report that as "in use", not as a USB fault. */
    rc = libusb_set_configuration(dev->handle, 1);
    if (rc != 0) {
        libusb_close(dev->handle);
        libusb_exit(dev->usb_ctx);
        memset(dev, 0, sizeof(*dev));
        return rc == LIBUSB_ERROR_BUSY ? PINNACLE_ERR_BUSY : PINNACLE_ERR_USB_CONFIG;
    }

    rc = libusb_claim_interface(dev->handle, PINNACLE_INTERFACE_NUM);
    if (rc != 0) {
        libusb_close(dev->handle);
        libusb_exit(dev->usb_ctx);
        memset(dev, 0, sizeof(*dev));
        return rc == LIBUSB_ERROR_BUSY ? PINNACLE_ERR_BUSY : PINNACLE_ERR_USB_CLAIM;
    }
    dev->interface_claimed = 1;

    return PINNACLE_OK;
}

pinnacle_status_t pinnacle_open(pinnacle_device_t *dev)
{
    return pinnacle_open_by_id(dev, NULL);
}

void pinnacle_close(pinnacle_device_t *dev)
{
    if (!dev)
        return;
    if (dev->handle) {
        if (dev->interface_claimed)
            libusb_release_interface(dev->handle, PINNACLE_INTERFACE_NUM);
        libusb_close(dev->handle);
    }
    if (dev->usb_ctx)
        libusb_exit(dev->usb_ctx);
    memset(dev, 0, sizeof(*dev));
}

static pinnacle_status_t upload_bitstream(pinnacle_device_t *dev, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        pin_logf(PIN_LOG_ERROR, "pinnacle: cannot open bitstream '%s': %s\n", path, strerror(errno));
        return PINNACLE_ERR_BITSTREAM_READ;
    }

    uint8_t *buf = malloc(PINNACLE_BITSTREAM_TOTAL_SIZE);
    if (!buf) {
        fclose(f);
        return PINNACLE_ERR_BITSTREAM_READ;
    }

    size_t n = fread(buf, 1, PINNACLE_BITSTREAM_TOTAL_SIZE, f);
    fclose(f);
    if (n != PINNACLE_BITSTREAM_TOTAL_SIZE) {
        pin_logf(PIN_LOG_ERROR, "pinnacle: bitstream '%s' is %zu bytes, expected %d\n",
                path, n, PINNACLE_BITSTREAM_TOTAL_SIZE);
        free(buf);
        return PINNACLE_ERR_BITSTREAM_READ;
    }

    pinnacle_status_t status = PINNACLE_OK;
    size_t offset = 0;
    pinnacle_progress(dev, "Uploading FPGA firmware", 0);
    for (unsigned i = 0; i < PINNACLE_BITSTREAM_CHUNK_COUNT; i++) {
        unsigned chunk_len = PINNACLE_BITSTREAM_CHUNK_SIZES[i];
        int transferred = 0;
        int rc = libusb_bulk_transfer(dev->handle, PINNACLE_EP_CMD_OUT,
                                       buf + offset, (int)chunk_len,
                                       &transferred, BULK_TIMEOUT_MS);
        if (rc != 0 || (unsigned)transferred != chunk_len) {
            pin_logf(PIN_LOG_ERROR, "pinnacle: bitstream chunk %u/%u failed (rc=%d, sent=%d/%u)\n",
                    i + 1, PINNACLE_BITSTREAM_CHUNK_COUNT, rc, transferred, chunk_len);
            status = PINNACLE_ERR_USB_TRANSFER;
            break;
        }
        offset += chunk_len;
        pinnacle_progress(dev, "Uploading FPGA firmware",
                          (int)(offset * 100 / PINNACLE_BITSTREAM_TOTAL_SIZE));
    }

    free(buf);
    return status;
}

/* Tiny request/reply exchange on the low-level config channel (EP 0x01 OUT /
 * 0x81 IN). Distinct from the SAA7113 I2C register writes on this same
 * endpoint pair (those carry a constant 0x4a "slave address" byte) -- these
 * are short firmware handshake packets seen bracketing the bitstream upload
 * in a real cold-boot trace (see docs/command-channel-findings.md). Reply
 * content isn't currently interpreted, only that the exchange completes. */
static pinnacle_status_t config_exchange(pinnacle_device_t *dev,
                                          const uint8_t *req, int req_len)
{
    uint8_t reply[64];
    int transferred = 0;

    int rc = libusb_bulk_transfer(dev->handle, PINNACLE_EP_CONFIG_OUT,
                                   (uint8_t *)req, req_len, &transferred, BULK_TIMEOUT_MS);
    if (rc != 0 || transferred != req_len) {
        pin_logf(PIN_LOG_ERROR, "pinnacle: config write failed (rc=%d, sent=%d/%d)\n",
                rc, transferred, req_len);
        return PINNACLE_ERR_USB_TRANSFER;
    }

    rc = libusb_bulk_transfer(dev->handle, PINNACLE_EP_CONFIG_IN,
                               reply, sizeof(reply), &transferred, BULK_TIMEOUT_MS);
    if (rc != 0) {
        pin_logf(PIN_LOG_ERROR, "pinnacle: config reply read failed (rc=%d)\n", rc);
        return PINNACLE_ERR_USB_TRANSFER;
    }

    /* "80 <index> 08 00..." reads 8 bytes of the device's configuration
     * memory; the reply echoes the request header in bytes 0..3. Index 3 is
     * the 1394 GUID that MarvinBus64 publishes in the node's config ROM
     * (byte-identical in the cold-boot trace). Index 0 is another 8-byte ID
     * (fb82ad03 8d615d0e on the development unit; meaning unknown). */
    if (req_len >= 3 && req[0] == 0x80 && req[2] == 0x08 && transferred >= 12) {
        if (req[1] == 0x03) {
            dev->guid_hi = ((uint32_t)reply[4] << 24) | ((uint32_t)reply[5] << 16) |
                           ((uint32_t)reply[6] << 8) | reply[7];
            dev->guid_lo = ((uint32_t)reply[8] << 24) | ((uint32_t)reply[9] << 16) |
                           ((uint32_t)reply[10] << 8) | reply[11];
            dev->have_guid = 1;
        } else if (req[1] == 0x00) {
            memcpy(dev->id0, reply + 4, 8);
        }
    }

    /* The 2-byte 05/06 exchanges bracketing the bitstream upload are status
     * reads: the device answers "<cmd> 01" when it is ready and "<cmd> 00"
     * when it is not. We used to ignore the reply entirely and carry on, so a
     * device that never came up looked identical to a healthy one until the
     * command channel silently refused to answer much later. Report it. */
    if (req_len == 2 && (req[0] == 0x05 || req[0] == 0x06) && transferred >= 2) {
        if (reply[1] != 0x01) {
            pin_logf(PIN_LOG_WARN,
                    "pinnacle: WARNING: device reports NOT READY to status read %02x "
                    "(replied %02x %02x, expected %02x 01).\n"
                    "pinnacle: the FPGA has not come up; this normally needs a physical "
                    "power cycle (unplug/replug USB) -- a warm reboot or a USB controller "
                    "reset does not clear it.\n",
                    req[0], reply[0], reply[1], req[0]);
            return PINNACLE_ERR_NOT_READY;
        }
    }

    return PINNACLE_OK;
}

pinnacle_status_t pinnacle_read_guid(pinnacle_device_t *dev, uint32_t *guid_hi, uint32_t *guid_lo)
{
    static const uint8_t req[10] = { 0x80, 0x03, 0x08 };
    dev->have_guid = 0;
    pinnacle_status_t st = config_exchange(dev, req, sizeof(req));
    if (st != PINNACLE_OK)
        return st;
    if (!dev->have_guid)
        return PINNACLE_ERR_USB_TRANSFER; /* short or malformed reply */
    *guid_hi = dev->guid_hi;
    *guid_lo = dev->guid_lo;
    return PINNACLE_OK;
}

/* Returns PINNACLE_ERR_NOT_READY if a status read said the device isn't up.
 * Plain transfer errors are reported by config_exchange and then tolerated,
 * which is the long-standing behaviour: several of these exchanges are
 * SAA7113 I2C writes whose replies we don't model, and aborting on them used
 * to be worse than continuing. */
static pinnacle_status_t config_replay_seq(pinnacle_device_t *dev,
                                            const pinnacle_pkt_t *seq, unsigned count)
{
    pinnacle_status_t result = PINNACLE_OK;

    for (unsigned i = 0; i < count; i++) {
        if (seq[i].delay_ms > 0)
            sleep_ms(seq[i].delay_ms);
        pinnacle_status_t status = config_exchange(dev, seq[i].data, (int)seq[i].len);
        if (status == PINNACLE_ERR_NOT_READY)
            result = status;
    }
    return result;
}

pinnacle_status_t pinnacle_init_hardware(pinnacle_device_t *dev, const char *bitstream_path)
{
    dev->have_guid = 0;
    pinnacle_progress(dev, "Resetting the device and reading its identity", -1);
    int rc = libusb_set_interface_alt_setting(dev->handle, PINNACLE_INTERFACE_NUM,
                                               PINNACLE_ALT_SETTING_IDLE);
    if (rc != 0)
        return PINNACLE_ERR_USB_TRANSFER;

    /* Full captured cold-boot sequence on the low-level config channel,
     * immediately before the FPGA bitstream upload begins -- see
     * PINNACLE_CONFIG_PREBITSTREAM_SEQ in protocol_data.h. Confirmed required:
     * without it, the two status reads bracketing the bitstream upload (see
     * below) come back "not ready" and the command channel (EP 0x02) accepts
     * only 2 writes before NAKing every subsequent one indefinitely. With it,
     * both come back "ready" and the full command sequence goes through. */
    pinnacle_status_t ready = config_replay_seq(dev, PINNACLE_CONFIG_PREBITSTREAM_SEQ,
                                                 PINNACLE_CONFIG_PREBITSTREAM_SEQ_COUNT);
    if (ready != PINNACLE_OK)
        return ready;

    pinnacle_status_t status = upload_bitstream(dev, bitstream_path);
    if (status != PINNACLE_OK)
        return status;

    /* The captured trace waits ~1.17s between the last bitstream byte and
     * selecting alt 1 -- almost certainly letting the Cyclone FPGA finish
     * configuring from the bitstream just clocked in. Skipping this wait
     * reproduced a consistent wedge (device stops ACKing bulk OUT writes
     * after exactly 2 command packets, regardless of their content) even
     * right after a fresh physical replug, consistent with the FX2's
     * small OUT-endpoint buffer filling because the FPGA-side consumer
     * wasn't booted yet. */
    pinnacle_progress(dev, "Waiting for the FPGA to start up", -1);
    sleep_ms(1500);

    pinnacle_progress(dev, "Finishing FPGA start-up", -1);
    /* "06 00" -> "06 01" exchange on the config channel right after the
     * bitstream upload, before switching to the operational alt setting --
     * the second half of the fix described above. */
    ready = config_replay_seq(dev, PINNACLE_CONFIG_POSTBITSTREAM_SEQ,
                               PINNACLE_CONFIG_POSTBITSTREAM_SEQ_COUNT);
    if (ready != PINNACLE_OK)
        return ready;

    rc = libusb_set_interface_alt_setting(dev->handle, PINNACLE_INTERFACE_NUM,
                                           PINNACLE_ALT_SETTING_OPERATIONAL);
    if (rc != 0)
        return PINNACLE_ERR_USB_TRANSFER;

    return PINNACLE_OK;
}
