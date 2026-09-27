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
 * See docs/hardware.md for the reverse-engineering background.
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
    PINNACLE_ERR_BUSY,          /* device/lock already held by another process */
    PINNACLE_ERR_LOCK,          /* pinnacle_lock.[ch]: OS lock primitive failed (not a BUSY case) */
} pinnacle_status_t;

const char *pinnacle_strerror(pinnacle_status_t status);

/* The PINNACLE_* getenv() knobs the core used to read on every call, now
 * collected into one struct so a GUI can set them programmatically (and so
 * the value is read once, not on every hot-path call). Defaults reproduce
 * the pre-refactor behaviour exactly; pinnacle_tuning_from_env() applies the
 * same env vars the core used to read directly, so the CLIs -- which call it
 * once right after pinnacle_open() -- behave exactly as before. Core code
 * itself never calls getenv(); it only reads dev->tuning. */
typedef struct {
    /* pinnacle_stream.c (DV/HDV) */
    int debug_1394;             /* PINNACLE_DEBUG_1394: p1394 verbose level (0/1/2) */
    int probe_registers;        /* PINNACLE_PROBE=1: probe OHCI registers after stream start */
    int debug_ep88;             /* PINNACLE_DEBUG_EP88=1: log every EP 0x88 completion */
    const char *raw_dump_path;  /* PINNACLE_RAW_DUMP: also write raw EP 0x88 bytes here, or NULL */
    int debug_ep84;             /* PINNACLE_DEBUG_EP84=1: log every EP 0x84 record */

    /* pinnacle_analog.c */
    unsigned video_queue;       /* PINNACLE_VIDEO_QUEUE: video transfer queue depth, 0 = default */
    unsigned video_xfer;        /* PINNACLE_VIDEO_XFER: video transfer size, 0 = default */
    int debug_analog;           /* PINNACLE_DEBUG_ANALOG=1: log queue-depth/loop-gap stats */
    int no_raw_io;              /* PINNACLE_NO_RAW_IO=1: leave WinUSB's RAW_IO policy off */
} pinnacle_tuning_t;

/* Fills *t with the same defaults the core used to fall back to when an env
 * var was unset. Called by pinnacle_open(), so dev->tuning is always usable
 * even if the CLI never calls pinnacle_tuning_from_env(). */
void pinnacle_tuning_defaults(pinnacle_tuning_t *t);

/* Overrides *t from the PINNACLE_* environment variables, exactly as the
 * core used to read them inline. CLI-only: a GUI has no controlling
 * terminal/environment to speak of and should set fields directly. */
void pinnacle_tuning_from_env(pinnacle_tuning_t *t);

typedef struct {
    libusb_context *usb_ctx;
    libusb_device_handle *handle;
    int interface_claimed;
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
    pinnacle_tuning_t tuning;  /* PINNACLE_* knobs; defaulted by pinnacle_open() */
} pinnacle_device_t;

static inline void pinnacle_progress(const pinnacle_device_t *dev, const char *step, int percent)
{
    if (dev && dev->progress)
        dev->progress(dev->progress_user, step, percent);
}

/* Finds and opens the device, claims the vendor-class interface. Does not
 * touch alt settings or upload anything yet — call pinnacle_init_hardware
 * next. Equivalent to pinnacle_open_by_id(dev, NULL). */
pinnacle_status_t pinnacle_open(pinnacle_device_t *dev);

/* Same, but opens a specific device: device_id is a port-path id as
 * reported by pinnacle_enumerate() (src/core/pinnacle_enum.h), e.g.
 * "usb:1-4.2". NULL or "first" opens the first supported device found,
 * exactly like pinnacle_open() (today: the first 2304:0213 libusb finds).
 * Returns PINNACLE_ERR_NOT_FOUND if no device matches, PINNACLE_ERR_BUSY if
 * a matching device exists but another process (or driver) already has it
 * open -- see pinnacle_lock.h for the cross-process story; this is only the
 * libusb-level ACCESS/BUSY fallback for a caller that opens without taking
 * that lock first. */
pinnacle_status_t pinnacle_open_by_id(pinnacle_device_t *dev, const char *device_id);

/* Releases the interface (if claimed) and closes the device. Safe to call
 * on a zero-initialised or partially-opened dev. */
void pinnacle_close(pinnacle_device_t *dev);

/* Reads the unit's 1394 GUID ("80 03 08" on the config channel) and nothing
 * else, so a device list can show a stable per-unit id before anyone has
 * brought the device up. The EZ-USB side answers it without the power-up
 * sequence and without an FPGA design (checked on the rig with a warm
 * device, and a normal bring-up afterwards is unaffected). dev must be open
 * (pinnacle_open_by_id); the caller should hold the device's pinnacle_lock
 * so this never runs under another process's capture. */
pinnacle_status_t pinnacle_read_guid(pinnacle_device_t *dev, uint32_t *guid_hi, uint32_t *guid_lo);

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

#ifdef __cplusplus
}
#endif

#endif /* PINNACLE_DEVICE_H */
