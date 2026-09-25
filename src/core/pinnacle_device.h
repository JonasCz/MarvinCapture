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
 * Core USB access to the Pinnacle 500-USB ("Marvin-Lite", 2304:0213).
 * See ../../HANDOFF.md for the reverse-engineering background.
 *
 * This header intentionally exposes only device init/teardown. Streaming
 * (start/stop/read) lives in pinnacle_stream.h so a future GUI can link the
 * same core without pulling in file-writing concerns.
 */

#ifndef PINNACLE_DEVICE_H
#define PINNACLE_DEVICE_H

#include <libusb-1.0/libusb.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PINNACLE_VID 0x2304
#define PINNACLE_PID 0x0213

#define PINNACLE_INTERFACE_NUM 0
#define PINNACLE_ALT_SETTING_IDLE 0
#define PINNACLE_ALT_SETTING_OPERATIONAL 1

#define PINNACLE_EP_CONFIG_OUT 0x01 /* low-level config channel (I2C to SAA7113 + firmware handshake); required even for DV, see pinnacle_init_hardware */
#define PINNACLE_EP_CONFIG_IN  0x81
#define PINNACLE_EP_CMD_OUT    0x02 /* FPGA bitstream at init, then the 1394/stream command channel */
#define PINNACLE_EP_CMD_IN     0x84 /* replies/status for the command channel */
#define PINNACLE_EP_DV_IN      0x88 /* the DV stream, nothing else */

typedef enum {
    PINNACLE_OK = 0,
    PINNACLE_ERR_USB_INIT,
    PINNACLE_ERR_NOT_FOUND,
    PINNACLE_ERR_USB_OPEN,
    PINNACLE_ERR_USB_CONFIG,
    PINNACLE_ERR_USB_CLAIM,
    PINNACLE_ERR_USB_TRANSFER,
    PINNACLE_ERR_BITSTREAM_READ,
    PINNACLE_ERR_NOT_READY,
} pinnacle_status_t;

const char *pinnacle_strerror(pinnacle_status_t status);

typedef struct {
    libusb_context *usb_ctx;
    libusb_device_handle *handle;
    int interface_claimed;
    /* read from the device's configuration memory by pinnacle_init_hardware
     * (config-channel reads "80 00 08" and "80 03 08", 8 bytes each) */
    int have_guid;
    uint32_t guid_hi, guid_lo;  /* the 1394 EUI-64 our node publishes */
    uint8_t id0[8];             /* the block at index 0; meaning unknown */
    /* set by pinnacle_stream_start */
    uint16_t camera_node;      /* 0xffc0 | node number, 0 if none found */
    int iso_channel;           /* channel IR context 0 listens on */
    int pcr_connected;         /* we hold a point-to-point connection on oPCR[0] */
} pinnacle_device_t;

/* Finds and opens the device, claims the vendor-class interface. Does not
 * touch alt settings or upload anything yet — call pinnacle_init_hardware
 * next. */
pinnacle_status_t pinnacle_open(pinnacle_device_t *dev);

/* Releases the interface (if claimed) and closes the device. Safe to call
 * on a zero-initialised or partially-opened dev. */
void pinnacle_close(pinnacle_device_t *dev);

/* Replays the config-channel bring-up sequence, the FPGA bitstream upload,
 * and selects the operational alt setting. bitstream_path must point to the
 * raw .rbf blob extracted from the user's own vendor driver install (see
 * traces/README.md) — never redistribute it.
 *
 * The config-channel sequence (which includes the SAA7113 analog-decoder
 * I2C init) turned out to be required even for pure DV capture: skipping it
 * leaves the command channel (EP 0x02) accepting only 2 writes before it
 * NAKs indefinitely. See docs/command-channel-findings.md. */
pinnacle_status_t pinnacle_init_hardware(pinnacle_device_t *dev, const char *bitstream_path);

#ifdef __cplusplus
}
#endif

#endif /* PINNACLE_DEVICE_H */
