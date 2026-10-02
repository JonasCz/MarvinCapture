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
#include "pinnacle_cfg.h"
#include "pinnacle_enum.h"
#include "pinnacle_fx2.h"
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
    t->debug_ep88 = 0;
    t->raw_dump_path = NULL;
    t->debug_ep84 = 0;
    t->video_queue = 0;     /* 0 = use VIDEO_QUEUE */
    t->video_xfer = 0;      /* 0 = use VIDEO_XFER */
}

void pinnacle_tuning_from_env(pinnacle_tuning_t *t)
{
    const char *e;

    if ((e = getenv("PINNACLE_DEBUG_1394")))
        t->debug_1394 = atoi(e);
    if ((e = getenv("PINNACLE_PROBE")))
        t->probe_registers = (e[0] == '1');
    if ((e = getenv("PINNACLE_DEBUG_EP88")))
        t->debug_ep88 = (e[0] == '1');
    if ((e = getenv("PINNACLE_RAW_DUMP")))
        t->raw_dump_path = e;
    if ((e = getenv("PINNACLE_DEBUG_EP84")))
        t->debug_ep84 = (e[0] == '1');
    if ((e = getenv("PINNACLE_VIDEO_QUEUE")))
        t->video_queue = (unsigned)atoi(e);
    if ((e = getenv("PINNACLE_VIDEO_XFER")))
        t->video_xfer = (unsigned)atoi(e);
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
    case PINNACLE_ERR_NOT_FOUND: return "no supported Pinnacle USB capture device (2304:xxxx) found";
    case PINNACLE_ERR_USB_OPEN: return "failed to open device (check permissions, try sudo)";
    case PINNACLE_ERR_USB_CONFIG: return "failed to set USB configuration";
    case PINNACLE_ERR_USB_CLAIM: return "failed to claim interface (another driver/process attached?)";
    case PINNACLE_ERR_USB_TRANSFER: return "USB bulk transfer failed";
    case PINNACLE_ERR_BITSTREAM_READ: return "failed to read FPGA bitstream file";
    case PINNACLE_ERR_NOT_READY: return "device reports not ready (FPGA did not come up; needs a physical USB power cycle)";
    case PINNACLE_ERR_BUSY: return "device already open in another process";
    case PINNACLE_ERR_LOCK: return "internal locking error";
    case PINNACLE_ERR_NO_FX2_FIRMWARE:
        return "the device's USB controller has no firmware loaded and the host-side download failed (is firmware/fx2-marvin.bin present?)";
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
                                      libusb_device **match_out,
                                      const pinnacle_model_t **model_out)
{
    int use_first = (!device_id || strcmp(device_id, "first") == 0);

    libusb_device **list = NULL;
    ssize_t n = libusb_get_device_list(ctx, &list);
    pinnacle_status_t status = PINNACLE_ERR_NOT_FOUND;

    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) != 0)
            continue;
        if (desc.idVendor != PINNACLE_VID)
            continue;
        const pinnacle_model_t *model = pinnacle_model_lookup(desc.idProduct);
        if (!model || !model->supported)
            continue; /* not a model in pinnacle_model_table that we drive */

        if (use_first) {
            *match_out = libusb_ref_device(list[i]);
            *model_out = model;
            status = PINNACLE_OK;
            break;
        }

        char id[PINNACLE_ENUM_ID_MAX];
        pinnacle_enum_build_id(list[i], id, sizeof(id));
        if (strcmp(id, device_id) == 0) {
            *match_out = libusb_ref_device(list[i]);
            *model_out = model;
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
    const pinnacle_model_t *model = NULL;
    pinnacle_status_t status = find_device(dev->usb_ctx, device_id, &match, &model);
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

    dev->model = model;
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
 * in a real cold-boot trace (see docs/protocol.md). Reply
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

/* "Classic" firmware (no 80 <idx> 08 command): the vendor driver reads the
 * same 8 bytes as eight single-byte FX2 vendor requests, bRequest 0xA0 (the
 * Cypress "internal RAM" request), wValue 0x78 + i (MarvinAVS64.sys
 * FUN_0002cb5c, else branch). Untested: no classic unit has been available. */
static pinnacle_status_t read_guid_a0(pinnacle_device_t *dev)
{
    uint8_t b[8];
    for (int i = 0; i < 8; i++) {
        int rc = libusb_control_transfer(dev->handle, 0xC0, 0xA0, (uint16_t)(0x78 + i), 0,
                                          &b[i], 1, BULK_TIMEOUT_MS);
        if (rc != 1)
            return PINNACLE_ERR_USB_TRANSFER;
    }
    dev->guid_hi = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    dev->guid_lo = ((uint32_t)b[4] << 24) | ((uint32_t)b[5] << 16) | ((uint32_t)b[6] << 8) | b[7];
    dev->have_guid = 1;
    return PINNACLE_OK;
}

pinnacle_status_t pinnacle_read_guid(pinnacle_device_t *dev, uint32_t *guid_hi, uint32_t *guid_lo)
{
    static const uint8_t req[10] = { 0x80, 0x03, 0x08 };
    dev->have_guid = 0;
    if (dev->model && !dev->model->cr_config) {
        pinnacle_status_t st = read_guid_a0(dev);
        if (st != PINNACLE_OK)
            return st;
        *guid_hi = dev->guid_hi;
        *guid_lo = dev->guid_lo;
        return PINNACLE_OK;
    }
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
        /* classic firmware has no 0c power-up and no 80 <idx> 08 reads: the
         * vendor driver only sends those to the Marvin-CR family */
        if (dev->model && !dev->model->cr_config &&
            (seq[i].data[0] == 0x0c || seq[i].data[0] == 0x80))
            continue;
        if (seq[i].delay_ms > 0)
            sleep_ms(seq[i].delay_ms);
        pinnacle_status_t status = config_exchange(dev, seq[i].data, (int)seq[i].len);
        if (status == PINNACLE_ERR_NOT_READY)
            result = status;
    }
    return result;
}

static int fx2_probe(pinnacle_device_t *dev)
{
    uint8_t probe = 0;
    return pinnacle_cfg_op(dev, 0x07, 0x00, &probe) == PINNACLE_OK && probe == 0x01;
}

static int fx2_write(void *user, uint16_t addr, const uint8_t *data, uint16_t len)
{
    pinnacle_device_t *dev = user;
    int rc = libusb_control_transfer(dev->handle, 0x40, 0xA0, addr, 0, (uint8_t *)data, len,
                                     BULK_TIMEOUT_MS);
    return rc == (int)len ? 0 : 1;
}

static void fx2_drop_handle(pinnacle_device_t *dev)
{
    if (!dev->handle)
        return;
    if (dev->interface_claimed)
        libusb_release_interface(dev->handle, PINNACLE_INTERFACE_NUM);
    libusb_close(dev->handle);
    dev->handle = NULL;
    dev->interface_claimed = 0;
}

/* Re-opens the unit at the USB port path `id` after it re-enumerated:
 * same steps as the tail of pinnacle_open_by_id. */
static pinnacle_status_t fx2_reopen(pinnacle_device_t *dev, const char *id)
{
    libusb_device *match = NULL;
    const pinnacle_model_t *model = NULL;
    if (find_device(dev->usb_ctx, id, &match, &model) != PINNACLE_OK)
        return PINNACLE_ERR_NOT_FOUND;
    int rc = libusb_open(match, &dev->handle);
    libusb_unref_device(match);
    if (rc != 0) {
        dev->handle = NULL;
        return PINNACLE_ERR_USB_OPEN;
    }
    dev->model = model;
    libusb_set_auto_detach_kernel_driver(dev->handle, 1);
    if (libusb_set_configuration(dev->handle, 1) != 0)
        return PINNACLE_ERR_USB_CONFIG;
    if (libusb_claim_interface(dev->handle, PINNACLE_INTERFACE_NUM) != 0)
        return PINNACLE_ERR_USB_CLAIM;
    dev->interface_claimed = 1;
    return PINNACLE_OK;
}

pinnacle_status_t pinnacle_ensure_fx2(pinnacle_device_t *dev, const char *any_firmware_path)
{
    if (!dev || !dev->model || !dev->model->fx2_firmware)
        return PINNACLE_OK;
    pin_logf(PIN_LOG_DEBUG, "pinnacle: probing for FX2 firmware (classic model)\n");
    if (fx2_probe(dev))
        return PINNACLE_OK;

    /* image: <dir of any_firmware_path>/<fx2_firmware> */
    const char *base = any_firmware_path ? any_firmware_path : "";
    char path[1024];
    size_t dirlen = 0;
    for (const char *p = base; *p; p++)
        if (*p == '/' || *p == '\\')
            dirlen = (size_t)(p - base) + 1;
    if (dirlen + strlen(dev->model->fx2_firmware) + 1 > sizeof(path))
        return PINNACLE_ERR_NO_FX2_FIRMWARE;
    memcpy(path, base, dirlen);
    strcpy(path + dirlen, dev->model->fx2_firmware);

    uint8_t *img = malloc(PINNACLE_FX2_MAX_IMAGE + 1);
    if (!img)
        return PINNACLE_ERR_NO_FX2_FIRMWARE;
    FILE *f = fopen(path, "rb");
    size_t n = f ? fread(img, 1, PINNACLE_FX2_MAX_IMAGE + 1, f) : 0;
    if (f)
        fclose(f);
    if (!f || pinnacle_fx2_validate(img, n) != 0) {
        pin_logf(PIN_LOG_ERROR, "pinnacle: FX2 firmware '%s' missing or invalid\n", path);
        free(img);
        return PINNACLE_ERR_NO_FX2_FIRMWARE;
    }

    char id[PINNACLE_ENUM_ID_MAX];
    pinnacle_enum_build_id(libusb_get_device(dev->handle), id, sizeof(id));

    pinnacle_progress(dev, "Loading the USB controller firmware", -1);
    pin_logf(PIN_LOG_INFO, "pinnacle: no answer to the FX2 probe, downloading %s (%zu bytes)\n",
             path, n);
    int rc = pinnacle_fx2_download(img, n, fx2_write, dev, 50);
    free(img);
    if (rc != 0) {
        pin_logf(PIN_LOG_ERROR, "pinnacle: FX2 firmware download failed\n");
        return PINNACLE_ERR_NO_FX2_FIRMWARE;
    }

    /* The FX2 drops off the bus and comes back (new descriptors) on the same
     * port: close our handle, then re-open by port path until the probe works. */
    fx2_drop_handle(dev);
    pinnacle_progress(dev, "Waiting for the USB controller to restart", -1);
    for (int waited = 0; waited < 10000; waited += 250) {
        sleep_ms(250);
        if (fx2_reopen(dev, id) == PINNACLE_OK && fx2_probe(dev)) {
            pin_logf(PIN_LOG_INFO, "pinnacle: FX2 firmware running\n");
            return PINNACLE_OK;
        }
        fx2_drop_handle(dev);
    }
    pin_logf(PIN_LOG_ERROR, "pinnacle: the unit did not come back after the FX2 download\n");
    return PINNACLE_ERR_NO_FX2_FIRMWARE;
}

pinnacle_status_t pinnacle_init_hardware(pinnacle_device_t *dev, const char *bitstream_path)
{
    dev->have_guid = 0;
    pinnacle_progress(dev, "Resetting the device and reading its identity", -1);
    int rc = libusb_set_interface_alt_setting(dev->handle, PINNACLE_INTERFACE_NUM,
                                               PINNACLE_ALT_SETTING_IDLE);
    if (rc != 0)
        return PINNACLE_ERR_USB_TRANSFER;

    /* Classic units may boot without firmware (VID/PID-only EEPROM): see
     * pinnacle_ensure_fx2. */
    pinnacle_status_t fx2 = pinnacle_ensure_fx2(dev, bitstream_path);
    if (fx2 != PINNACLE_OK)
        return fx2;

    /* Full captured cold-boot sequence on the low-level config channel,
     * immediately before the FPGA bitstream upload begins -- see
     * PINNACLE_CONFIG_PREBITSTREAM_SEQ in protocol_data.h. Confirmed required:
     * without it, the two status reads bracketing the bitstream upload (see
     * below) come back "not ready" and the command channel (EP 0x02) accepts
     * only 2 writes before NAKing every subsequent one indefinitely. With it,
     * both come back "ready" and the full command sequence goes through.
     *
     * Right after a fresh physical replug, this status read can still come
     * back "not ready" for real (the FX2/FPGA hasn't finished its own
     * power-up) rather than because the sequence above was skipped -- this
     * is the same class of post-replug settling the 1.5 s wait below and
     * docs/startup.md's step 2 already document, just before the sequence
     * instead of after the bitstream. Retry with backoff instead of failing
     * the whole open on the first check. */
    static const unsigned prebitstream_retry_delays_ms[] = { 300, 600, 1200 };
    pinnacle_status_t ready;
    for (unsigned attempt = 0; ; attempt++) {
        ready = config_replay_seq(dev, PINNACLE_CONFIG_PREBITSTREAM_SEQ,
                                   PINNACLE_CONFIG_PREBITSTREAM_SEQ_COUNT);
        if (ready == PINNACLE_OK ||
            attempt >= sizeof(prebitstream_retry_delays_ms) / sizeof(prebitstream_retry_delays_ms[0]))
            break;
        pin_logf(PIN_LOG_WARN,
                "pinnacle: prebitstream handshake not ready yet, retrying in %u ms "
                "(settling after a fresh replug)\n", prebitstream_retry_delays_ms[attempt]);
        sleep_ms(prebitstream_retry_delays_ms[attempt]);
    }
    if (ready != PINNACLE_OK)
        return ready;

    /* The replay above carried the CR family's GUID read; classic units get theirs here. */
    if (dev->model && !dev->model->cr_config && read_guid_a0(dev) != PINNACLE_OK)
        pin_logf(PIN_LOG_WARN, "pinnacle: could not read the device GUID (classic model)\n");

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
