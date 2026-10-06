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
 * Core USB access to the Pinnacle 500-USB / 510-USB ("Marvin-Lite", 2304:0213 / 0223; see pinnacle_model.h).
 * See docs/hardware.md for the reverse-engineering background.
 *
 * This header intentionally exposes only device init/teardown. Streaming
 * (start/stop/read) lives in pinnacle_stream.h so a future GUI can link the
 * same core without pulling in file-writing concerns.
 */

#ifndef PINNACLE_DEVICE_H
#define PINNACLE_DEVICE_H

#include "pinnacle_model.h"

#include <libusb-1.0/libusb.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PINNACLE_VID 0x2304

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
    PINNACLE_ERR_BUSY,          /* device/lock already held by another process */
    PINNACLE_ERR_LOCK,          /* pinnacle_lock.[ch]: OS lock primitive failed (not a BUSY case) */
    PINNACLE_ERR_NO_FX2_FIRMWARE, /* the unit's FX2 has no firmware running and the host-side download failed or its file is missing */
} pinnacle_status_t;

const char *pinnacle_strerror(pinnacle_status_t status);

typedef struct {
    libusb_context *usb_ctx;
    libusb_device_handle *handle;
    int interface_claimed;
    const pinnacle_model_t *model; /* the table row of the opened unit (never NULL once open) */
    /* read from the device's configuration memory by pinnacle_init_hardware
     * (config-channel reads "80 00 08" and "80 03 08", 8 bytes each) */
    int have_guid;
    uint32_t guid_hi, guid_lo;  /* the 1394 EUI-64 our node publishes */
    uint8_t id0[8];             /* the block at index 0; meaning unknown */
    /* Optional bring-up progress: step is a short sentence, percent 0..100 or
     * -1 when the step has no measurable length. Called from the thread that
     * runs the bring-up. Survives pinnacle_open*(), which clears the rest, only
     * if set after it. */
    void (*progress)(void *user, const char *step, int percent);
    void *progress_user;
    /* set by pinnacle_stream_start */
    uint16_t camera_node;      /* 0xffc0 | node number, 0 if none found */
    int node_count;            /* nodes on the 1394 bus, us included (0 = not scanned) */
    int iso_channel;           /* channel IR context 0 listens on */
    int pcr_connected;         /* we hold a point-to-point connection on oPCR[0] */
    char fail_detail[200];     /* why the last pinnacle_stream_start failed, for the user */
    int warm_dv;               /* the last pinnacle_init_hardware reused a running OHCI design */
    char bitstream_path[512];  /* the DV bitstream path of the last pinnacle_init_hardware (cold fallback) */
    char progress_last[96];    /* last step pinnacle_progress() logged (debug log), "" = none */
    uint64_t progress_ms;      /* monotonic ms when it was logged */
} pinnacle_device_t;

/* Reports a bring-up step to the progress callback (if set) and logs it at
 * debug level, with the time since the previous step; a step repeated with a
 * new percentage is logged once. */
void pinnacle_progress(pinnacle_device_t *dev, const char *step, int percent);

/* Finds and opens the device, claims the vendor-class interface. Does not
 * touch alt settings or upload anything yet — call pinnacle_init_hardware
 * next. Equivalent to pinnacle_open_by_id(dev, NULL). */
pinnacle_status_t pinnacle_open(pinnacle_device_t *dev);

/* Same, but opens a specific device: device_id is a port-path id as
 * reported by pinnacle_enumerate() (src/core/pinnacle_enum.h), e.g.
 * "usb:1-4.2". NULL or "first" opens the first supported device found,
 * exactly like pinnacle_open() (the first supported model libusb finds).
 * Returns PINNACLE_ERR_NOT_FOUND if no device matches, PINNACLE_ERR_BUSY if
 * a matching device exists but another process (or driver) already has it
 * open -- see pinnacle_lock.h for the cross-process story; this is only the
 * libusb-level ACCESS/BUSY fallback for a caller that opens without taking
 * that lock first. */
pinnacle_status_t pinnacle_open_by_id(pinnacle_device_t *dev, const char *device_id);

/* Releases the interface (if claimed) and closes the device. Safe to call
 * on a zero-initialised or partially-opened dev. */
void pinnacle_close(pinnacle_device_t *dev);

/* 1 if the open unit still answers a standard GET_STATUS on EP0 (handled by
 * the FX2 itself, so it works whatever the FPGA is doing), 0 if it is gone
 * (unplugged, powered off). For telling a vanished device from a transfer
 * that failed for another reason. */
int pinnacle_device_responds(pinnacle_device_t *dev);

/* Reads the unit's 1394 GUID ("80 03 08" on the config channel) and nothing
 * else, so a device list can show a stable per-unit id before anyone has
 * brought the device up. The EZ-USB side answers it without the power-up
 * sequence and without an FPGA design (checked on the rig with a warm
 * device, and a normal bring-up afterwards is unaffected). dev must be open
 * (pinnacle_open_by_id); the caller should hold the device's pinnacle_lock
 * so this never runs under another process's capture. */
pinnacle_status_t pinnacle_read_guid(pinnacle_device_t *dev, uint32_t *guid_hi, uint32_t *guid_lo);

/* For models with an FX2 image in the table (MovieBox Deluxe): probes the
 * config channel ("07 00" -> "07 01"); if the unit does not answer, downloads
 * the model's FX2 firmware (fx2_firmware, looked up in the directory of
 * any_firmware_path, e.g. the bitstream path), waits for the unit to
 * re-enumerate on the same USB port and re-opens it (dev->handle changes).
 * No-op for other models. PINNACLE_ERR_NO_FX2_FIRMWARE if the file is missing
 * or the unit does not come back. Called by pinnacle_init_hardware and
 * pinnacle_analog_open. Untested on real hardware. */
pinnacle_status_t pinnacle_ensure_fx2(pinnacle_device_t *dev, const char *any_firmware_path);

/* Replays the config-channel bring-up sequence, the FPGA bitstream upload,
 * and selects the operational alt setting. bitstream_path must point to the
 * raw .rbf blob (firmware/fpga-ohci.bin; see firmware/README.md for where
 * it comes from and its licence).
 *
 * The config-channel sequence (which includes the SAA7113 analog-decoder
 * I2C init) turned out to be required even for pure DV capture: skipping it
 * leaves the command channel (EP 0x02) accepting only 2 writes before it
 * NAKs indefinitely. See docs/protocol.md. */
pinnacle_status_t pinnacle_init_hardware(pinnacle_device_t *dev, const char *bitstream_path);

/* What the FPGA holds right now, from pinnacle_probe_fpga(). */
typedef enum {
    PINNACLE_FPGA_UNKNOWN = 0, /* not probed (model not enabled) or the probe failed */
    PINNACLE_FPGA_NONE,        /* no design running ("06 00" -> 0): a cold device, the power-up is needed */
    PINNACLE_FPGA_OHCI,        /* the DV/HDV design, and it answers on EP 0x02 */
    PINNACLE_FPGA_CAPTURE,     /* the analog Capture design */
    PINNACLE_FPGA_OTHER,       /* some design runs but it is not Capture (and, if asked, did not answer as OHCI) */
} pinnacle_fpga_t;

const char *pinnacle_fpga_name(pinnacle_fpga_t state);

/* Warm-start detection: which design is in the FPGA, without uploading anything.
 * Only for models with warm_ok in the table (else PINNACLE_FPGA_UNKNOWN: take the full
 * cold path).
 * "06 00" says whether any design runs; with alt 1 selected the Capture design
 * acknowledges an I2C read of its block at 0xf0 and the OHCI design does not;
 * with check_ohci the OHCI design is then confirmed by a vendor read on EP 0x02
 * (only asked when the I2C read ruled Capture out, so a Capture design never gets
 * EP 0x02 traffic). Leaves alt 1 selected. A wrong answer can only cost the cold
 * path: callers fall back to it on any failure. dev must be open, FX2 alive. */
pinnacle_fpga_t pinnacle_probe_fpga(pinnacle_device_t *dev, int check_ohci);

/* pinnacle_init_hardware with the warm path switched off. */
pinnacle_status_t pinnacle_init_hardware_cold(pinnacle_device_t *dev, const char *bitstream_path);

#ifdef __cplusplus
}
#endif

#endif /* PINNACLE_DEVICE_H */
