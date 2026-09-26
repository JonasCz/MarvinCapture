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
 * Analog capture. Every register value here comes from MarvinAVS64.sys
 * (the SAA7113, AC'97 and capture-block objects) and was checked against
 * traces/20260925-154656-analog-plug-virtualdub-preview.pcapng; see
 * docs/analog.md for the function-by-function map.
 */

#include "pinnacle_analog.h"
#include "pinnacle_cfg.h"
#include "pin_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SAA7113_ADDR 0x4a
#define CAPTURE_ADDR 0xf0

/* Capture block registers (I2C 0xf0). */
#define CAP_FORMAT     0x00   /* geometry, see capture_format() */
#define CAP_CONTROL    0x01   /* see CTL_* */
#define CAP_AC97_REG   0x02
#define CAP_AC97_LO    0x03
#define CAP_AC97_HI    0x04
#define CAP_AC97_CMD   0x05   /* bit0 write, bit1 busy, bit7 audio packets on */
#define CAP_AUDIO_LO   0x06   /* bytes per audio packet incl. 12-byte header */
#define CAP_AUDIO_HI   0x07

#define CTL_RUN        0x01
#define CTL_VIDEO      0x02
#define CTL_AUDIO      0x04
#define CTL_BUSY       0x80   /* read: still flushing after RUN was cleared */
#define CTL_CODEC_INIT 0x20   /* set only while the codec is first programmed (inferred) */

#define AUDIO_RATE 48000
#define PACKET_HEADER 12

static void sleep_ms(unsigned ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* --- standards and defaults --------------------------------------------- */

static const char *const std_names[] = {
    "PAL", "NTSC", "PAL-M", "PAL-N", "PAL-60", "NTSC-4.43", "NTSC-J", "SECAM",
};

const char *pinnacle_std_name(pinnacle_std_t std)
{
    return (unsigned)std < sizeof(std_names) / sizeof(std_names[0]) ? std_names[std] : "?";
}

int pinnacle_std_is_60hz(pinnacle_std_t std)
{
    return std == PINNACLE_STD_NTSC || std == PINNACLE_STD_PAL_M || std == PINNACLE_STD_PAL_60 ||
           std == PINNACLE_STD_NTSC_443 || std == PINNACLE_STD_NTSC_J;
}

void pinnacle_picture_defaults(pinnacle_picture_t *p)
{
    /* The values of the vendor's SAA7113 init table. */
    p->brightness = 0x80;
    p->contrast = 0x47;
    p->saturation = 0x40;
    p->hue = 0;
    p->sharpness = 1;
}

void pinnacle_analog_config_defaults(pinnacle_analog_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->input = PINNACLE_INPUT_COMPOSITE;
    cfg->standard = PINNACLE_STD_PAL;
    pinnacle_picture_defaults(&cfg->picture);
    cfg->vcr_mode = 1;
}

/* --- SAA7113 ------------------------------------------------------------ */

typedef struct { uint8_t reg, val; } regval_t;

/* The vendor's init table (FUN_00025bc4), identical to the writes in the
 * trace. Register 0x10 is left to the standard tables. The VBI slicer
 * registers 0x41..0x57 are copied as-is. */
static const regval_t saa7113_init[] = {
    { 0x01, 0x08 }, { 0x02, 0xc0 }, { 0x03, 0x33 }, { 0x04, 0x00 }, { 0x05, 0x00 },
    { 0x06, 0xe9 }, { 0x07, 0x0d }, { 0x08, 0xb8 }, { 0x09, 0x01 }, { 0x0a, 0x80 },
    { 0x0b, 0x47 }, { 0x0c, 0x40 }, { 0x0d, 0x00 }, { 0x0e, 0x01 }, { 0x0f, 0x2a },
    { 0x11, 0x04 }, { 0x12, 0x02 }, { 0x13, 0x00 }, { 0x15, 0x00 }, { 0x16, 0x00 },
    { 0x17, 0x00 }, { 0x40, 0x02 }, { 0x41, 0x00 }, { 0x42, 0x00 }, { 0x43, 0x00 },
    { 0x44, 0x00 }, { 0x45, 0x00 }, { 0x46, 0x00 }, { 0x47, 0x00 }, { 0x48, 0x00 },
    { 0x49, 0x00 }, { 0x4a, 0x00 }, { 0x4b, 0x00 }, { 0x4c, 0x00 }, { 0x4d, 0x00 },
    { 0x4e, 0x00 }, { 0x4f, 0x00 }, { 0x50, 0x00 }, { 0x51, 0x00 }, { 0x52, 0x00 },
    { 0x53, 0x00 }, { 0x54, 0x00 }, { 0x55, 0xff }, { 0x56, 0xff }, { 0x57, 0xff },
    { 0x58, 0x00 }, { 0x59, 0x54 }, { 0x5a, 0x07 }, { 0x5b, 0x83 }, { 0x5e, 0x00 },
};

static pinnacle_status_t saa_write(pinnacle_analog_t *a, uint8_t reg, uint8_t val)
{
    return pinnacle_i2c_write(a->dev, SAA7113_ADDR, reg, val);
}

static pinnacle_status_t saa_write_table(pinnacle_analog_t *a, const regval_t *t, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        pinnacle_status_t st = saa_write(a, t[i].reg, t[i].val);
        if (st != PINNACLE_OK)
            return st;
    }
    return PINNACLE_OK;
}

/* Reg 0x08: bit7 AUFD (automatic field detection) is always on; bit6 FSEL
 * (60 Hz when AUFD is off); bits 4:3 HTC, 01 = VTR, 11 = fast locking.
 * The vendor's VcrMode switches between exactly those two. */
static uint8_t saa_reg08(const pinnacle_analog_t *a)
{
    uint8_t v = 0x80 | (a->cfg.vcr_mode ? 0x08 : 0x18);
    if (pinnacle_std_is_60hz(a->cfg.standard))
        v |= 0x40;
    return v;
}

/* Tables from FUN_00025fe8: a 50 or 60 Hz base, then a chroma-standard
 * override of reg 0x0e (bits 6:4 CSTD). */
static pinnacle_status_t saa_apply_standard(pinnacle_analog_t *a)
{
    pinnacle_std_t std = a->cfg.standard;
    int is60 = pinnacle_std_is_60hz(std);
    uint8_t r0e = is60 ? 0x89 : 0x81, r0f = 0x2a;

    switch (std) {
    case PINNACLE_STD_PAL_N:    r0e = 0xad; break;
    case PINNACLE_STD_NTSC_443: r0e = 0xad; break;
    case PINNACLE_STD_PAL_M:    r0e = 0xbd; break;
    case PINNACLE_STD_PAL_60:   r0e = 0x95; break;
    case PINNACLE_STD_NTSC_J:   r0e = 0xcd; break;
    case PINNACLE_STD_SECAM:    r0e = 0x50; r0f = 0x80; break;
    default: break;
    }

    const regval_t t[] = {
        { 0x0e, r0e }, { 0x0f, r0f },
        { 0x10, is60 ? 0x40 : 0x00 },   /* OFTS: the vendor sets 01 for 60 Hz */
        { 0x08, saa_reg08(a) },
        { 0x40, is60 ? 0x82 : 0x02 },   /* slicer: 60 Hz field timing */
        { 0x5a, is60 ? 0x0a : 0x07 },   /* slicer vertical offset */
    };
    return saa_write_table(a, t, sizeof(t) / sizeof(t[0]));
}

static pinnacle_status_t saa_apply_picture(pinnacle_analog_t *a)
{
    const pinnacle_picture_t *p = &a->cfg.picture;
    a->saa09 = (uint8_t)((a->saa09 & 0x80) | (p->sharpness & 0x03));
    const regval_t t[] = {
        { 0x09, a->saa09 },
        { 0x0a, (uint8_t)p->brightness },
        { 0x0b, (uint8_t)(p->contrast & 0x7f) },
        { 0x0c, (uint8_t)(p->saturation & 0x7f) },
        { 0x0d, (uint8_t)(int8_t)p->hue },
    };
    return saa_write_table(a, t, sizeof(t) / sizeof(t[0]));
}

/* FUN_00039fd0: reg 0x02 selects the analog inputs (FUSE = 11, amplifier
 * and anti-alias on). Composite is AI11 (mode 0). S-video is mode 9, Y on
 * AI12 and C on AI22, with reg 0x09 bit 7 (BYPS) bypassing the chroma trap
 * so the luma keeps its full bandwidth. The driver also knows modes 4 and 8
 * for inputs that only other Marvin models wire up. */
static pinnacle_status_t saa_apply_input(pinnacle_analog_t *a)
{
    int svideo = a->cfg.input == PINNACLE_INPUT_SVIDEO;
    pinnacle_status_t st = saa_write(a, 0x02, svideo ? 0xc9 : 0xc0);
    if (st != PINNACLE_OK)
        return st;
    a->saa09 = (uint8_t)((a->saa09 & 0x7f) | (svideo ? 0x80 : 0x00));
    return saa_write(a, 0x09, a->saa09);
}

/* --- capture block ------------------------------------------------------- */

static pinnacle_status_t cap_write(pinnacle_analog_t *a, uint8_t reg, uint8_t val)
{
    pinnacle_status_t st = pinnacle_i2c_write(a->dev, CAPTURE_ADDR, reg, val);
    if (st == PINNACLE_OK)
        a->fpga[reg & 15] = val;
    return st;
}

static pinnacle_status_t cap_update(pinnacle_analog_t *a, uint8_t reg, uint8_t mask, uint8_t val)
{
    return cap_write(a, reg, (uint8_t)((a->fpga[reg & 15] & ~mask) | val));
}

/* "03 f0" / "04 f0", then the vendor waits 50 ms and forgets its shadow. */
static pinnacle_status_t cap_reset(pinnacle_analog_t *a)
{
    pinnacle_status_t st = pinnacle_cfg_chip_reset(a->dev, CAPTURE_ADDR);
    sleep_ms(50);
    memset(a->fpga, 0, sizeof(a->fpga));
    return st;
}

/* AC'97 register write through the capture block (FUN_0002f0a4): index,
 * data low, data high, then set the write+go bits and wait for busy (bit1)
 * to clear. */
static pinnacle_status_t ac97_write(pinnacle_analog_t *a, uint8_t reg, uint16_t val)
{
    pinnacle_status_t st = cap_write(a, CAP_AC97_REG, reg);
    if (st == PINNACLE_OK)
        st = cap_write(a, CAP_AC97_LO, (uint8_t)val);
    if (st == PINNACLE_OK)
        st = cap_write(a, CAP_AC97_HI, (uint8_t)(val >> 8));
    if (st == PINNACLE_OK)
        st = cap_update(a, CAP_AC97_CMD, 0, 0x03);
    for (int i = 0; st == PINNACLE_OK && i < 5; i++) {
        uint8_t v = 0;
        st = pinnacle_i2c_read(a->dev, CAPTURE_ADDR, CAP_AC97_CMD, &v);
        if (st == PINNACLE_OK && !(v & 0x02))
            return PINNACLE_OK;
    }
    return st == PINNACLE_OK ? PINNACLE_ERR_NOT_READY : st;
}

/* The codec set-up the vendor driver sends. Record source 5 (stereo mix)
 * with line-in at 0 dB in the mix, 48 kHz via variable-rate audio. Every
 * output that is not needed stays muted. */
static pinnacle_status_t ac97_init(pinnacle_analog_t *a)
{
    static const struct { uint8_t reg; uint16_t val; } t[] = {
        { 0x04, 0x8000 },   /* headphone / aux out: mute */
        { 0x06, 0x8000 },   /* mono out: mute */
        { 0x0a, 0x8000 },   /* PC beep: mute */
        { 0x1a, 0x0505 },   /* record select: stereo mix */
        { 0x1c, 0x0000 },   /* record gain 0 dB */
        { 0x18, 0x8000 },   /* PCM out: mute */
        { 0x2a, 0x0001 },   /* variable rate audio on */
        { 0x10, 0x0808 },   /* line in 0 dB */
        { 0x02, 0x0000 },   /* master: 0 dB, unmuted */
        { 0x20, 0x0000 },   /* general purpose */
        { 0x2c, AUDIO_RATE },
        { 0x32, AUDIO_RATE },
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        pinnacle_status_t st = ac97_write(a, t[i].reg, t[i].val);
        if (st != PINNACLE_OK) {
            pin_logf(PIN_LOG_ERROR, "pinnacle: AC'97 write %02x failed\n", t[i].reg);
            return st;
        }
    }
    return PINNACLE_OK;
}

/* Capture block register 0 (FUN_0001aec4): bit6 = 525-line frame, bits
 * 7/2/1 = horizontal scaling (720: 0, 704: 0x80, 480: 0x04, 360: 0x02,
 * 352: 0x82), bits 4:3 = field selection for half-height capture. We always
 * capture full frames at 720 wide, both fields, top field first. */
static uint8_t capture_format(const pinnacle_analog_t *a)
{
    return pinnacle_std_is_60hz(a->cfg.standard) ? 0x40 : 0x00;
}

static void apply_geometry(pinnacle_analog_t *a)
{
    int is60 = pinnacle_std_is_60hz(a->cfg.standard);
    a->width = 720;
    a->height = is60 ? 480 : 576;
    /* One packet per frame at 25 fps. For 60 Hz the vendor's size is still
     * unknown; 1600 samples is 1/30 s. Audio is counted by samples, not
     * packets, downstream, so the choice only affects latency. */
    a->audio_samples_per_packet = is60 ? 1600 : AUDIO_RATE / 25;
}

/* --- bring-up ------------------------------------------------------------ */

/* What the vendor driver does on the config channel between plug-in and the
 * first bitstream (the start of the analog trace), minus the repeats. */
static pinnacle_status_t power_up(pinnacle_analog_t *a)
{
    pinnacle_device_t *dev = a->dev;
    uint8_t r = 0;
    pinnacle_status_t st;

    if (libusb_set_interface_alt_setting(dev->handle, PINNACLE_INTERFACE_NUM, 0) != 0)
        return PINNACLE_ERR_USB_TRANSFER;
    if ((st = pinnacle_cfg_op(dev, 0x07, 0x00, &r)) != PINNACLE_OK)
        return st;
    if ((st = pinnacle_cfg_op(dev, 0x0c, 0x01, &r)) != PINNACLE_OK)
        return st;
    if (r)
        sleep_ms(1000);   /* FUN_0002cabc: the device asks for a second */
    if ((st = pinnacle_cfg_chip_reset(dev, SAA7113_ADDR)) != PINNACLE_OK)
        return st;
    uint8_t ver = 0;
    if (pinnacle_i2c_read(dev, SAA7113_ADDR, 0x00, &ver) != PINNACLE_OK)
        pin_logf(PIN_LOG_WARN, "pinnacle: SAA7113 did not answer\n");
    if ((st = saa_write_table(a, saa7113_init, sizeof(saa7113_init) / sizeof(saa7113_init[0]))) !=
        PINNACLE_OK)
        return st;
    if ((st = pinnacle_cfg_chip_reset(dev, 0x00)) != PINNACLE_OK)
        return st;
    return pinnacle_cfg_chip_reset(dev, CAPTURE_ADDR);
}

pinnacle_status_t pinnacle_analog_open(pinnacle_analog_t *a, pinnacle_device_t *dev,
                                       const char *capture_bitstream_path,
                                       const pinnacle_analog_config_t *cfg)
{
    memset(a, 0, sizeof(*a));
    a->dev = dev;
    if (cfg)
        a->cfg = *cfg;
    else
        pinnacle_analog_config_defaults(&a->cfg);
    apply_geometry(a);

    /* "06 00" answers 01 while some bitstream is running. A cold device
     * first needs the power-up sequence before its loader answers "05". */
    uint8_t up = 0;
    pinnacle_progress(dev, "Checking the device", -1);
    pinnacle_status_t st = pinnacle_cfg_op(dev, 0x06, 0x00, &up);
    if (st != PINNACLE_OK)
        return st;
    if (up != 0x01) {
        pinnacle_progress(dev, "Powering up the device", -1);
        if ((st = power_up(a)) != PINNACLE_OK)
            return st;
    }

    if ((st = pinnacle_fpga_load(dev, capture_bitstream_path)) != PINNACLE_OK)
        return st;
    if (libusb_set_interface_alt_setting(dev->handle, PINNACLE_INTERFACE_NUM,
                                         PINNACLE_ALT_SETTING_CAPTURE) != 0)
        return PINNACLE_ERR_USB_TRANSFER;

    /* Decoder: full init (a warm switch from DV skipped power_up), then
     * the settings. Reg 0x11 bit 3 (OEYC) enables the decoder's pixel
     * output bus towards the FPGA; the vendor turns it on here. */
    a->saa09 = 0x01;
    pinnacle_progress(dev, "Initialising the video decoder", -1);
    if ((st = saa_write_table(a, saa7113_init, sizeof(saa7113_init) / sizeof(saa7113_init[0]))) !=
        PINNACLE_OK)
        return st;
    if ((st = saa_apply_standard(a)) != PINNACLE_OK ||
        (st = saa_apply_input(a)) != PINNACLE_OK ||
        (st = saa_apply_picture(a)) != PINNACLE_OK ||
        (st = saa_write(a, 0x11, 0x0c)) != PINNACLE_OK)
        return st;

    /* Capture block and codec, in the vendor's order. */
    pinnacle_progress(dev, "Initialising the audio codec", -1);
    if ((st = cap_reset(a)) != PINNACLE_OK ||
        (st = cap_write(a, CAP_FORMAT, 0x00)) != PINNACLE_OK ||
        (st = cap_write(a, CAP_CONTROL, 0x00)) != PINNACLE_OK ||
        (st = pinnacle_cfg_op(dev, 0x08, 0x00, NULL)) != PINNACLE_OK ||
        (st = cap_write(a, CAP_CONTROL, CTL_CODEC_INIT)) != PINNACLE_OK ||
        (st = ac97_init(a)) != PINNACLE_OK)
        return st;
    return PINNACLE_OK;
}

pinnacle_status_t pinnacle_analog_set_input(pinnacle_analog_t *a, pinnacle_input_t input)
{
    a->cfg.input = input;
    return saa_apply_input(a);
}

pinnacle_status_t pinnacle_analog_set_standard(pinnacle_analog_t *a, pinnacle_std_t std)
{
    a->cfg.standard = std;
    apply_geometry(a);
    pinnacle_status_t st = saa_apply_standard(a);
    if (st == PINNACLE_OK)
        st = saa_apply_input(a);
    if (st == PINNACLE_OK)
        st = saa_apply_picture(a);
    return st;
}

pinnacle_status_t pinnacle_analog_set_picture(pinnacle_analog_t *a, const pinnacle_picture_t *p)
{
    a->cfg.picture = *p;
    return saa_apply_picture(a);
}

/* AC'97 register 0x10 (Line In Volume): bits 12:8 = left gain, bits 4:0 =
 * right gain, one 5-bit field 0..31 applied to both channels here since
 * this device exposes a single "line-in gain" control rather than
 * independent L/R sliders; bit 15 (mute) is left clear -- unmuting a muted
 * line is `pinnacle_analog_set_audio_gain()` with any in-range value, and
 * there is no separate mute API here. The vendor default 0x0808 (idx 8 on
 * both channels) is the anchor: per the AC'97 spec this control ranges
 * 0 = +12 dB down to 31 = -34.5 dB in -1.5 dB steps, so idx 8 = +12 - 8*1.5
 * = 0 dB, exactly what the vendor driver has always sent (see
 * pinnacle_analog_start()/ac97_init()'s `{ 0x10, 0x0808 }`). Inverting that
 * gives idx = (120 - db_tenths) / 15, clamped to 0..31. */
pinnacle_status_t pinnacle_analog_set_audio_gain(pinnacle_analog_t *a, int32_t db_tenths)
{
    if (db_tenths > 120)
        db_tenths = 120;
    if (db_tenths < -345)
        db_tenths = -345;
    int idx = (120 - db_tenths) / 15;
    if (idx < 0) idx = 0;
    if (idx > 31) idx = 31;
    uint16_t val = (uint16_t)(((unsigned)idx << 8) | (unsigned)idx);
    return ac97_write(a, 0x10, val);
}

/* Status byte 0x1f: bit7 INTL, bit6 HLVLN (no horizontal lock), bit5 FIDT
 * (60 Hz). The vendor's standard detection (FUN_0003a53c) reads exactly
 * these three. */
pinnacle_status_t pinnacle_analog_get_status(pinnacle_analog_t *a, pinnacle_analog_status_t *s)
{
    uint8_t v = 0;
    pinnacle_status_t st = pinnacle_i2c_read(a->dev, SAA7113_ADDR, 0x1f, &v);
    if (st != PINNACLE_OK)
        return st;
    s->raw = v;
    s->locked = !(v & 0x40);
    s->is_60hz = !!(v & 0x20);
    s->interlaced = !!(v & 0x80);
    return PINNACLE_OK;
}

/* FUN_0001aec4 (format), FUN_0001b218 (start video/audio), FUN_0001bf7c
 * (run). The capture block is reset and the codec rate re-sent first, as
 * the vendor does right before starting. */
pinnacle_status_t pinnacle_analog_start(pinnacle_analog_t *a)
{
    unsigned audio_bytes = a->audio_samples_per_packet * 4 + PACKET_HEADER;
    pinnacle_status_t st;

    if ((st = cap_reset(a)) != PINNACLE_OK ||
        (st = pinnacle_cfg_op(a->dev, 0x08, 0x00, NULL)) != PINNACLE_OK ||
        (st = ac97_write(a, 0x10, 0x0808)) != PINNACLE_OK ||
        (st = ac97_write(a, 0x20, 0x0000)) != PINNACLE_OK ||
        (st = cap_write(a, CAP_FORMAT, capture_format(a))) != PINNACLE_OK ||
        (st = cap_write(a, CAP_CONTROL, 0x00)) != PINNACLE_OK ||
        (st = cap_write(a, CAP_AUDIO_LO, (uint8_t)audio_bytes)) != PINNACLE_OK ||
        (st = cap_write(a, CAP_AUDIO_HI, (uint8_t)(audio_bytes >> 8))) != PINNACLE_OK ||
        (st = cap_update(a, CAP_AC97_CMD, 0, 0x80)) != PINNACLE_OK ||
        (st = ac97_write(a, 0x10, 0x0808)) != PINNACLE_OK ||
        (st = ac97_write(a, 0x20, 0x0000)) != PINNACLE_OK ||
        (st = cap_update(a, CAP_CONTROL, CTL_AUDIO, CTL_AUDIO)) != PINNACLE_OK ||
        (st = cap_update(a, CAP_CONTROL, CTL_VIDEO, CTL_VIDEO)) != PINNACLE_OK ||
        (st = pinnacle_cfg_op(a->dev, 0x08, 0x00, NULL)) != PINNACLE_OK ||
        (st = cap_update(a, CAP_CONTROL, CTL_RUN, CTL_RUN)) != PINNACLE_OK)
        return st;
    return PINNACLE_OK;
}

/* FUN_0001b5a0 + FUN_0001bfbc: drop the stream enables, clear RUN and wait
 * for the block to report idle (bit 7), 1 ms per poll, at most 100. */
pinnacle_status_t pinnacle_analog_stop(pinnacle_analog_t *a)
{
    pinnacle_status_t st = cap_update(a, CAP_CONTROL, CTL_VIDEO | CTL_AUDIO, 0);
    if (st == PINNACLE_OK)
        st = cap_update(a, CAP_CONTROL, CTL_RUN, 0);
    for (int i = 0; st == PINNACLE_OK && i < 100; i++) {
        uint8_t v = 0;
        st = pinnacle_i2c_read(a->dev, CAPTURE_ADDR, CAP_CONTROL, &v);
        if (st != PINNACLE_OK || !(v & CTL_BUSY))
            break;
        sleep_ms(1);
    }
    return st;
}

/* --- streaming ----------------------------------------------------------- */

/* Video transfers must be small. The device has little buffering at
 * 20 MB/s, and when the host is late the FPGA cuts the frame short: data
 * stops for 15-35 ms, then the frame ends with its short packet on time and
 * the frame counter stays continuous. Measured on an Intel xHCI with the
 * IOMMU on, 30-40 s runs, with 255 of 256 transfers queued in the kernel
 * throughout (so the host was never out of buffers):
 *
 *     64 KiB and up   ~20% of the data lost, a third of the frames short
 *     32 KiB          a few percent
 *     20 KiB          3-9 short frames per 1000 (the vendor's URB size)
 *     16 KiB          3-8 per 1000
 *      8 KiB, 4 KiB   none
 *
 * So it is per-transfer cost on the host side, not queue depth; usbfs
 * turns anything over 16 KiB into a scatter-gather list, and big buffers
 * need several TRBs. 512 x 8 KiB keeps 200 ms queued.
 * PINNACLE_VIDEO_XFER / PINNACLE_VIDEO_QUEUE override both for testing. */
#define VIDEO_QUEUE_MAX 1024
#define VIDEO_QUEUE 512
#define VIDEO_XFER (8u * 1024)
#define AUDIO_QUEUE 8

struct slot {
    struct libusb_transfer *xfer;
    int done, failed, status;
};

static void LIBUSB_CALL slot_cb(struct libusb_transfer *xfer)
{
    struct slot *s = xfer->user_data;
    s->done = 1;
    if (xfer->status != LIBUSB_TRANSFER_COMPLETED) {
        s->failed = 1;
        s->status = xfer->status;
    }
}

struct queue {
    uint8_t ep;
    unsigned depth, head, active;
    struct slot slots[VIDEO_QUEUE_MAX];
};

static int queue_init(struct queue *q, pinnacle_device_t *dev, uint8_t ep, unsigned depth,
                      unsigned bytes)
{
    memset(q, 0, sizeof(*q));
    q->ep = ep;
    q->depth = depth;
    for (unsigned i = 0; i < depth; i++) {
        struct libusb_transfer *x = libusb_alloc_transfer(0);
        uint8_t *buf = x ? malloc(bytes) : NULL;
        if (!buf) {
            libusb_free_transfer(x);
            return -1;
        }
        libusb_fill_bulk_transfer(x, dev->handle, ep, buf, (int)bytes, slot_cb, &q->slots[i], 0);
        q->slots[i].xfer = x;
        if (libusb_submit_transfer(x) != 0)
            return -1;
        q->active++;
    }
    return 0;
}

/* Delivers completed transfers in submission order and resubmits them.
 * Returns -1 on a transfer error, 1 if the callback asked to stop. */
static int queue_poll(struct queue *q, pinnacle_analog_raw_cb cb, void *user, int stopping)
{
    while (q->slots[q->head].done) {
        struct slot *s = &q->slots[q->head];
        if (s->failed) {
            if (s->status != LIBUSB_TRANSFER_CANCELLED && !stopping)
                pin_logf(PIN_LOG_ERROR, "pinnacle: EP 0x%02x transfer failed (status %d)\n", q->ep,
                        s->status);
            return -1;
        }
        int r = cb(q->ep, s->xfer->buffer, (size_t)s->xfer->actual_length, user);
        s->done = 0;
        if (!stopping && libusb_submit_transfer(s->xfer) != 0) {
            s->done = 1;
            s->failed = 1;
            s->status = LIBUSB_TRANSFER_ERROR;
            return -1;
        }
        q->head = (q->head + 1) % q->depth;
        if (r)
            return 1;
    }
    return 0;
}

static void queue_free(struct queue *q, pinnacle_device_t *dev)
{
    for (unsigned i = 0; i < q->depth; i++)
        if (q->slots[i].xfer && !q->slots[i].done)
            libusb_cancel_transfer(q->slots[i].xfer);
    for (int spin = 0; spin < 100; spin++) {
        int pending = 0;
        for (unsigned i = 0; i < q->depth; i++)
            if (q->slots[i].xfer && !q->slots[i].done)
                pending = 1;
        if (!pending)
            break;
        struct timeval tv = { .tv_sec = 0, .tv_usec = 20000 };
        libusb_handle_events_timeout_completed(dev->usb_ctx, &tv, NULL);
    }
    for (unsigned i = 0; i < q->depth; i++) {
        if (!q->slots[i].xfer)
            continue;
        free(q->slots[i].xfer->buffer);
        libusb_free_transfer(q->slots[i].xfer);
    }
    memset(q, 0, sizeof(*q));
}

pinnacle_status_t pinnacle_analog_read_loop(pinnacle_analog_t *a, pinnacle_analog_raw_cb cb,
                                            void *user, volatile int *stop_flag)
{
    pinnacle_device_t *dev = a->dev;
    struct queue *vq = calloc(1, sizeof(*vq)), *aq = calloc(1, sizeof(*aq));
    pinnacle_status_t status = PINNACLE_OK;
    /* One audio packet per transfer: round up to whole USB packets so the
     * device's short packet ends each transfer. */
    unsigned audio_bytes = (a->audio_samples_per_packet * 4 + PACKET_HEADER + 511) & ~511u;

    unsigned vdepth = dev->tuning.video_queue ? dev->tuning.video_queue : VIDEO_QUEUE;
    unsigned vbytes = dev->tuning.video_xfer ? dev->tuning.video_xfer : VIDEO_XFER;
    if (vdepth < 1 || vdepth > VIDEO_QUEUE_MAX)
        vdepth = VIDEO_QUEUE;
    if (vbytes < 512 || vbytes % 512)
        vbytes = VIDEO_XFER;

    if (!vq || !aq || queue_init(vq, dev, PINNACLE_EP_VIDEO_IN, vdepth, vbytes) != 0 ||
        queue_init(aq, dev, PINNACLE_EP_AUDIO_IN, AUDIO_QUEUE, audio_bytes) != 0) {
        status = PINNACLE_ERR_USB_TRANSFER;
        goto out;
    }

    /* PINNACLE_DEBUG_ANALOG=1: once a second, the fewest video transfers
     * that were still queued in the kernel and the longest time between two
     * passes of this loop. */
    int debug = dev->tuning.debug_analog;
    struct timespec tprev, tnow, trep;
    clock_gettime(CLOCK_MONOTONIC, &tprev);
    trep = tprev;
    unsigned min_inflight = vdepth;
    double max_gap = 0;

    while (!*stop_flag) {
        struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 };
        if (libusb_handle_events_timeout_completed(dev->usb_ctx, &tv, NULL) != 0) {
            status = PINNACLE_ERR_USB_TRANSFER;
            break;
        }
        if (debug) {
            unsigned inflight = 0;
            for (unsigned i = 0; i < vq->depth; i++)
                inflight += !vq->slots[i].done;
            if (inflight < min_inflight)
                min_inflight = inflight;
            clock_gettime(CLOCK_MONOTONIC, &tnow);
            double gap = (tnow.tv_sec - tprev.tv_sec) * 1e3 + (tnow.tv_nsec - tprev.tv_nsec) / 1e6;
            if (gap > max_gap)
                max_gap = gap;
            tprev = tnow;
            if (tnow.tv_sec - trep.tv_sec >= 1) {
                pin_logf(PIN_LOG_DEBUG, "\n[analog] min in flight %u/%u, max loop gap %.1f ms\n",
                        min_inflight, vq->depth, max_gap);
                min_inflight = vq->depth;
                max_gap = 0;
                trep = tnow;
            }
        }
        int rv = queue_poll(vq, cb, user, 0);
        int ra = rv == 0 ? queue_poll(aq, cb, user, 0) : 0;
        if (rv < 0 || ra < 0) {
            status = PINNACLE_ERR_USB_TRANSFER;
            break;
        }
        if (rv > 0 || ra > 0 || cb(0, NULL, 0, user))
            break;
    }
out:
    if (vq) {
        queue_free(vq, dev);
        free(vq);
    }
    if (aq) {
        queue_free(aq, dev);
        free(aq);
    }
    return status;
}

/* --- assembler ----------------------------------------------------------- */

typedef struct {
    pinnacle_analog_t *a;
    const pinnacle_capture_sink_t *sink;
    pinnacle_capture_stats_t *st;
    uint8_t *frame;              /* persistent: a short frame keeps the old tail */
    size_t frame_bytes, received;
    int in_frame, have_frame;
    uint16_t seq, last_vseq;
    uint64_t vtime;
    uint32_t index;
    int have_aseq;
    uint16_t next_aseq;
    uint8_t *silence;
    unsigned spp;
    /* Stopping: no more video, but wait (briefly) for the audio of the
     * frames already delivered, so both streams end together. */
    volatile int *ext_stop;
    int draining;
    struct timespec drain_start;
} assembler_t;

/* The device time in a packet header: a 64-bit little-endian count of ~10 MHz
 * ticks in bytes 4..11. Bytes 8..11 stay zero for the first 2^32 ticks
 * (429.5 s after the capture block was reset) and then count on. */
static uint64_t header_time(const uint8_t *d)
{
    uint64_t t = 0;
    for (int i = 7; i >= 0; i--)
        t = t << 8 | d[4 + i];
    return t;
}

static void start_draining(assembler_t *s)
{
    if (!s->draining) {
        s->draining = 1;
        clock_gettime(CLOCK_MONOTONIC, &s->drain_start);
    }
}

/* Audio for every delivered frame is out (or nothing was delivered). */
static int audio_caught_up(const assembler_t *s)
{
    return !s->have_frame || (uint16_t)(s->next_aseq - s->last_vseq - 1) < 0x8000;
}

static int emit_frame(assembler_t *s, uint16_t seq, uint64_t t, int repeated, size_t received)
{
    pinnacle_video_frame_t f = {
        .yuyv = s->frame, .width = s->a->width, .height = s->a->height,
        .index = s->index++, .seq = seq, .device_time = t,
        .repeated = repeated, .received = received,
    };
    s->st->frames++;
    return s->sink->video ? s->sink->video(&f, s->sink->user) : 0;
}

static int emit_audio(assembler_t *s, const uint8_t *pcm, unsigned samples, uint16_t seq,
                      uint64_t t, int silence)
{
    pinnacle_audio_block_t b = {
        .pcm = pcm, .samples = samples, .seq = seq, .device_time = t, .silence = silence,
    };
    s->st->audio_blocks++;
    return s->sink->audio ? s->sink->audio(&b, s->sink->user) : 0;
}

static int finish_frame(assembler_t *s)
{
    int r = 0;
    s->in_frame = 0;
    if (s->received < s->frame_bytes)
        s->st->frames_truncated++;
    if (s->have_frame) {
        uint16_t gap = (uint16_t)(s->seq - s->last_vseq - 1);
        /* Anything but a small forward gap means the counter restarted. */
        if (gap > 0 && gap < 250) {
            s->st->frames_missing += gap;
            for (uint16_t i = 1; i <= gap && !r; i++)
                r = emit_frame(s, (uint16_t)(s->last_vseq + i), 0, 1, 0);
        }
    }
    if (!s->have_aseq) {
        /* Audio packet N belongs to video frame N: start both at this frame. */
        s->next_aseq = s->seq;
        s->have_aseq = 1;
    }
    s->have_frame = 1;
    s->last_vseq = s->seq;
    return r ? r : emit_frame(s, s->seq, s->vtime, 0, s->received);
}

/* The device sends a frame as two fields, one after the other: all lines
 * of the first field, then all of the second. We weave them as the bytes
 * arrive. For PAL the first field is the top one (even lines, counting
 * from 0), which is also the first in time: comparing both weaves of a
 * still picture, this one has 40% less line-to-line comb energy. The
 * vendor driver's render path does the opposite conversion (woven frame
 * to field-sequential) with the same geometry. */
static void weave(assembler_t *s, const uint8_t *d, size_t n)
{
    size_t stride = (size_t)s->a->width * 2, half = s->a->height / 2;
    size_t off = s->received;
    while (n) {
        size_t line = off / stride, col = off % stride;
        size_t out = line < half ? 2 * line : 2 * (line - half) + 1;
        size_t k = stride - col < n ? stride - col : n;
        memcpy(s->frame + out * stride + col, d, k);
        d += k;
        n -= k;
        off += k;
    }
}

static int on_video(assembler_t *s, const uint8_t *d, size_t n)
{
    size_t wire = n;
    /* Pixel data cannot look like a header: 0xff never occurs in ITU-R BT.656
     * pixel values. Nothing else in the header is checked: the time field
     * is a 64-bit counter and its upper half is not always zero. */
    int header = n >= PACKET_HEADER && d[0] == 0xff && d[1] == 0x00;
    int r = 0;

    /* A header while a frame is open: that frame ended without its short
     * packet. Deliver what we have. (Pixel data cannot look like a header:
     * 0xff never occurs in ITU-R BT.656 pixel values.) */
    if (header && s->in_frame && (r = finish_frame(s)) != 0)
        return r;
    if (header) {
        s->seq = (uint16_t)(d[2] | d[3] << 8);
        s->vtime = header_time(d);
        s->in_frame = 1;
        s->received = 0;
        d += PACKET_HEADER;
        n -= PACKET_HEADER;
    } else if (!s->in_frame) {
        if (n)
            s->st->resyncs++;
        return 0;
    }

    size_t room = s->frame_bytes - s->received, take = n < room ? n : room;
    weave(s, d, take);
    s->received += take;
    if (wire % 512 != 0 || s->received >= s->frame_bytes)
        r = finish_frame(s);
    return r;
}

static int on_audio(assembler_t *s, const uint8_t *d, size_t n)
{
    if (n < PACKET_HEADER || d[0] != 0xff || d[1] != 0x00 || !s->have_aseq)
        return 0;   /* audio before the first video frame has nothing to pair with */
    uint16_t seq = (uint16_t)(d[2] | d[3] << 8);
    uint64_t t = header_time(d);
    uint16_t gap = (uint16_t)(seq - s->next_aseq);
    int r = 0;

    if (gap >= 0x8000)
        return 0;   /* older than where output started */
    if (s->draining && (uint16_t)(seq - s->last_vseq - 1) < 0x8000)
        return 0;   /* its frame was not delivered */
    if (gap < 250) {
        s->st->audio_missing += gap;
        for (uint16_t i = 0; i < gap && !r; i++)
            r = emit_audio(s, s->silence, s->spp, (uint16_t)(s->next_aseq + i), 0, 1);
    }
    s->next_aseq = (uint16_t)(seq + 1);
    return r ? r : emit_audio(s, d + PACKET_HEADER, (unsigned)((n - PACKET_HEADER) / 4), seq, t, 0);
}

static int assembler_cb(uint8_t ep, const uint8_t *data, size_t len, void *user)
{
    assembler_t *s = user;
    int r = 0;


    if (s->ext_stop && *s->ext_stop)
        start_draining(s);
    if (ep == PINNACLE_EP_VIDEO_IN && !s->draining)
        r = on_video(s, data, len);
    else if (ep == PINNACLE_EP_AUDIO_IN)
        r = on_audio(s, data, len);
    else if (ep == 0 && !s->draining && s->sink->tick)
        r = s->sink->tick(s->sink->user);
    if (r)
        start_draining(s);
    if (!s->draining)
        return 0;

    if (audio_caught_up(s))
        return 1;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long ms = (now.tv_sec - s->drain_start.tv_sec) * 1000 +
              (now.tv_nsec - s->drain_start.tv_nsec) / 1000000;
    if (ms < 300)
        return 0;
    /* The audio never came: pad, so the streams still end together. */
    while (!audio_caught_up(s)) {
        s->st->audio_missing++;
        emit_audio(s, s->silence, s->spp, s->next_aseq, 0, 1);
        s->next_aseq++;
    }
    return 1;
}

pinnacle_status_t pinnacle_analog_capture_loop(pinnacle_analog_t *a,
                                               const pinnacle_capture_sink_t *sink,
                                               pinnacle_capture_stats_t *stats,
                                               volatile int *stop_flag)
{
    assembler_t s;
    memset(&s, 0, sizeof(s));
    memset(stats, 0, sizeof(*stats));
    s.a = a;
    s.sink = sink;
    s.st = stats;
    s.spp = a->audio_samples_per_packet;
    s.frame_bytes = (size_t)a->width * a->height * 2;
    s.frame = malloc(s.frame_bytes);
    s.silence = calloc(s.spp, 4);
    if (!s.frame || !s.silence) {
        free(s.frame);
        free(s.silence);
        return PINNACLE_ERR_USB_TRANSFER;
    }
    /* Black, in case the very first frame arrives short. */
    for (size_t i = 0; i < s.frame_bytes; i += 2) {
        s.frame[i] = 0x10;
        s.frame[i + 1] = 0x80;
    }
    /* The assembler watches stop_flag itself, so it can drain first. */
    s.ext_stop = stop_flag;
    volatile int never = 0;
    pinnacle_status_t st = pinnacle_analog_read_loop(a, assembler_cb, &s, &never);
    free(s.frame);
    free(s.silence);
    return st;
}
