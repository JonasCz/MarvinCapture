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
 * Analog capture (composite / S-video + stereo line audio).
 *
 * Analog is a different FPGA design from DV: pinnacle_analog_open() loads
 * the Capture bitstream (it replaces the OHCI one if DV was running, no
 * replug needed) and selects alt setting 3. From then on everything is
 * I2C over the config channel:
 *
 *   0x4a  Philips SAA7113 video decoder
 *   0xf0  the FPGA's capture block, which also bridges an AC'97 codec
 *
 * Video arrives on EP 0x82, audio on EP 0x86. See docs/analog.md.
 */

#ifndef PINNACLE_ANALOG_H
#define PINNACLE_ANALOG_H

#include "pinnacle_device.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PINNACLE_ALT_SETTING_CAPTURE 3
#define PINNACLE_EP_VIDEO_IN 0x82
#define PINNACLE_EP_AUDIO_IN 0x86

typedef enum {
    PINNACLE_INPUT_COMPOSITE = 0,
    PINNACLE_INPUT_SVIDEO,
} pinnacle_input_t;

/* Colour standards the SAA7113 tables in the vendor driver cover. */
typedef enum {
    PINNACLE_STD_PAL = 0,    /* B/G/D/H/I, 625/50 */
    PINNACLE_STD_NTSC,       /* M, 525/60 */
    PINNACLE_STD_PAL_M,
    PINNACLE_STD_PAL_N,
    PINNACLE_STD_PAL_60,
    PINNACLE_STD_NTSC_443,
    PINNACLE_STD_NTSC_J,
    PINNACLE_STD_SECAM,
} pinnacle_std_t;

const char *pinnacle_std_name(pinnacle_std_t std);
int pinnacle_std_is_60hz(pinnacle_std_t std);

/* Picture controls, in SAA7113 register units. */
typedef struct {
    int brightness;   /* reg 0x0a, 0..255, 128 = neutral */
    int contrast;     /* reg 0x0b, 0..127, 64 = 1.0 */
    int saturation;   /* reg 0x0c, 0..127, 64 = 1.0 */
    int hue;          /* reg 0x0d, -128..127; NTSC only (the chip ignores it for PAL/SECAM) */
    int sharpness;    /* reg 0x09 aperture factor, 0..3 */
} pinnacle_picture_t;

void pinnacle_picture_defaults(pinnacle_picture_t *p);

typedef struct {
    pinnacle_input_t input;
    pinnacle_std_t standard;
    pinnacle_picture_t picture;
    int vcr_mode;       /* SAA7113 VTR timing (tape sources); the vendor default */
} pinnacle_analog_config_t;

void pinnacle_analog_config_defaults(pinnacle_analog_config_t *cfg);

typedef struct {
    uint8_t raw;        /* SAA7113 status byte, reg 0x1f */
    int locked;         /* horizontal PLL locked: a signal is present */
    int is_60hz;        /* the decoder sees a 60 Hz field rate */
    int interlaced;
} pinnacle_analog_status_t;

typedef struct {
    pinnacle_device_t *dev;
    pinnacle_analog_config_t cfg;
    uint8_t fpga[16];   /* shadow of the capture block's registers */
    uint8_t saa09;      /* shadow of SAA7113 reg 0x09 (chroma trap bypass | aperture) */
    unsigned width, height;
    unsigned audio_samples_per_packet;
} pinnacle_analog_t;

/* Brings the device into analog capture mode: powers it up if it is cold,
 * loads the Capture bitstream (capture_bitstream_path, extracted from the
 * user's own driver, see tools/extract-bitstreams.py), selects alt 3,
 * initialises the decoder and the audio codec and applies cfg. dev must be
 * open (pinnacle_open) but needs no pinnacle_init_hardware. */
pinnacle_status_t pinnacle_analog_open(pinnacle_analog_t *a, pinnacle_device_t *dev,
                                       const char *capture_bitstream_path,
                                       const pinnacle_analog_config_t *cfg);

pinnacle_status_t pinnacle_analog_set_input(pinnacle_analog_t *a, pinnacle_input_t input);
pinnacle_status_t pinnacle_analog_set_standard(pinnacle_analog_t *a, pinnacle_std_t std);
pinnacle_status_t pinnacle_analog_set_picture(pinnacle_analog_t *a, const pinnacle_picture_t *p);
pinnacle_status_t pinnacle_analog_get_status(pinnacle_analog_t *a, pinnacle_analog_status_t *st);

/* Line-in gain, in dB * 10 (e.g. -345 = -34.5 dB, 120 = +12.0 dB), the same
 * unit pin_api.h's PIN_CTL_AUDIO_GAIN uses; clamped to the AC'97 register's
 * range. See pinnacle_analog.c for the register-0x10 mapping. */
pinnacle_status_t pinnacle_analog_set_audio_gain(pinnacle_analog_t *a, int32_t db_tenths);

/* Starts / stops video and audio. Set the standard first: it decides the
 * frame geometry and the audio packet size. */
pinnacle_status_t pinnacle_analog_start(pinnacle_analog_t *a);
pinnacle_status_t pinnacle_analog_stop(pinnacle_analog_t *a);

/* Raw completions, in arrival order per endpoint. Return non-zero to stop.
 * Also called with ep 0 (no data) on every pass of the loop, at least every
 * 50 ms, so a consumer can stop even when no data arrives. */
typedef int (*pinnacle_analog_raw_cb)(uint8_t ep, const uint8_t *data, size_t len, void *user);

/* Keeps a queue of transfers outstanding on both endpoints until the
 * callback asks to stop or *stop_flag is set. */
pinnacle_status_t pinnacle_analog_read_loop(pinnacle_analog_t *a, pinnacle_analog_raw_cb cb,
                                            void *user, volatile int *stop_flag);

/* --- assembled capture ---------------------------------------------------
 *
 * Both streams carry the same 12-byte header,
 *     ff 00 <counter u16 LE> <device time u32 LE> 00 00 00 00
 * and both counters start at 1 when capture starts. Audio packet N holds
 * the samples captured during video frame N. The device time is a ~10 MHz
 * clock of the device's own; the frame rate is the source's (the SAA7113
 * locks to the input) and the audio clock is locked to it: exactly 1920
 * samples per PAL frame. See docs/analog.md, "Clocks".
 *
 * The capture loop keeps the output timeline intact whatever the USB side
 * does: a frame that never arrived is replaced by a repeat of the previous
 * one (and its audio by silence), a frame that arrived short keeps the
 * previous frame's pixels below the point where data stopped. Every such
 * repair is counted, so a capture can prove it had none. */

typedef struct {
    const uint8_t *yuyv;     /* width * height * 2 bytes, YUYV 4:2:2, both fields interleaved */
    unsigned width, height;
    uint32_t index;          /* position in the output, from 0 */
    uint16_t seq;            /* device frame counter */
    uint32_t device_time;
    int repeated;            /* stand-in for a frame that never arrived */
    size_t received;         /* bytes that arrived; below width*height*2 = truncated */
} pinnacle_video_frame_t;

typedef struct {
    const uint8_t *pcm;      /* 16-bit LE stereo, interleaved */
    unsigned samples;        /* sample frames (4 bytes each) */
    uint16_t seq;
    uint32_t device_time;
    int silence;             /* stand-in for a packet that never arrived */
} pinnacle_audio_block_t;

typedef struct {
    unsigned long frames;          /* delivered, including repeats */
    unsigned long frames_missing;  /* counter gaps, filled with repeats */
    unsigned long frames_truncated;
    unsigned long audio_blocks;
    unsigned long audio_missing;   /* counter gaps, filled with silence */
    unsigned long resyncs;         /* video data outside a frame, discarded */
} pinnacle_capture_stats_t;

typedef struct {
    /* Called in output order. The buffers are only valid during the call. */
    int (*video)(const pinnacle_video_frame_t *f, void *user);
    int (*audio)(const pinnacle_audio_block_t *b, void *user);
    void *user;
    /* Optional. Called on every pass of the loop, at least every 50 ms, even
     * when nothing arrives -- with no input signal the decoder sends no
     * frames at all, so work that must keep happening (commands, signal
     * polling, idle timeouts) can't live in video(). Non-zero stops. */
    int (*tick)(void *user);
} pinnacle_capture_sink_t;

/* Runs pinnacle_analog_read_loop with the assembler in between. Either
 * callback returning non-zero stops the loop. */
pinnacle_status_t pinnacle_analog_capture_loop(pinnacle_analog_t *a,
                                               const pinnacle_capture_sink_t *sink,
                                               pinnacle_capture_stats_t *stats,
                                               volatile int *stop_flag);

#ifdef __cplusplus
}
#endif

#endif /* PINNACLE_ANALOG_H */
