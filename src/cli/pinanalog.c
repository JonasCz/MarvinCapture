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
 * pinanalog -- analog capture from the Pinnacle 500-USB into an AVI
 * (uncompressed YUY2 + 48 kHz stereo PCM).
 *
 * Brings the device into analog mode (from cold or from DV, no replug),
 * captures until Ctrl+C or -t, and reports every repair the assembler had
 * to make (see pinnacle_analog.h). A clean capture prints zeros.
 */

#include "pinnacle_device.h"
#include "pinnacle_analog.h"
#include "pinnacle_enum.h"
#include "pinnacle_lock.h"
#include "avi_writer.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile int g_stop = 0;

static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: pinanalog [options]\n"
            "  -b FILE                 Capture FPGA bitstream (default firmware/fpga-capture.bin)\n"
            "  -o FILE.avi             write YUY2 + PCM AVI (otherwise just count)\n"
            "  -i composite|svideo     input (default composite)\n"
            "  -s pal|ntsc|pal-m|pal-n|pal-60|ntsc-443|ntsc-j|secam|auto  (default auto)\n"
            "  -t seconds              stop after this long\n"
            "  --brightness 0..255  --contrast 0..127  --saturation 0..127\n"
            "  --hue -128..127  --sharpness 0..3  --tv-mode (no VCR timing)\n"
            "  --status                print the decoder status and exit\n"
            "  --raw FILE              dump raw EP 0x82/0x86 completions instead\n");
}

static int parse_std(const char *s, pinnacle_std_t *out)
{
    static const char *const names[] = {
        "pal", "ntsc", "pal-m", "pal-n", "pal-60", "ntsc-443", "ntsc-j", "secam",
    };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strcmp(s, names[i]) == 0) {
            *out = (pinnacle_std_t)i;
            return 0;
        }
    return -1;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* --- raw dump ------------------------------------------------------------ */

typedef struct {
    FILE *raw;
    double t0, deadline;
} raw_ctx_t;

/* Record: ep (1), length (4, LE), arrival time in us since start (8, LE), data. */
static int raw_cb(uint8_t ep, const uint8_t *data, size_t len, void *user)
{
    raw_ctx_t *c = user;
    if (!ep)
        return c->deadline > 0 && now_s() > c->deadline;
    uint64_t us = (uint64_t)((now_s() - c->t0) * 1e6);
    uint8_t hdr[13];
    hdr[0] = ep;
    for (int i = 0; i < 4; i++)
        hdr[1 + i] = (uint8_t)(len >> (8 * i));
    for (int i = 0; i < 8; i++)
        hdr[5 + i] = (uint8_t)(us >> (8 * i));
    fwrite(hdr, 1, sizeof(hdr), c->raw);
    fwrite(data, 1, len, c->raw);
    return c->deadline > 0 && now_s() > c->deadline;
}

/* --- writer thread --------------------------------------------------------
 *
 * The USB thread only copies into this queue; the disk is written from a
 * second thread, so a filesystem stall cannot starve the transfer queue
 * (the lesson from DV capture, docs/usage.md). The queue holds
 * about 5 s of video. */

#define QUEUE_ITEMS 256

typedef struct {
    int video;
    size_t len;
    uint8_t *data;
} item_t;

typedef struct {
    avi_writer_t *avi;
    item_t items[QUEUE_ITEMS];
    unsigned head, tail, max_fill;   /* head: next to write, tail: next free */
    int closed, failed;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    pthread_t thread;
    /* capture side */
    double t0, deadline, last_report;
    pinnacle_capture_stats_t *stats;
    unsigned long blocked;
} writer_t;

static void *writer_main(void *arg)
{
    writer_t *w = arg;
    pthread_mutex_lock(&w->lock);
    for (;;) {
        while (w->head == w->tail && !w->closed)
            pthread_cond_wait(&w->wake, &w->lock);
        if (w->head == w->tail)
            break;
        item_t it = w->items[w->head % QUEUE_ITEMS];
        pthread_mutex_unlock(&w->lock);
        int r = it.video ? avi_write_video(w->avi, it.data, it.len)
                         : avi_write_audio(w->avi, it.data, it.len);
        free(it.data);
        pthread_mutex_lock(&w->lock);
        if (r != 0)
            w->failed = 1;
        w->head++;
        pthread_cond_broadcast(&w->wake);
    }
    pthread_mutex_unlock(&w->lock);
    return NULL;
}

static int writer_push(writer_t *w, int video, const uint8_t *data, size_t len)
{
    uint8_t *copy = malloc(len);
    if (!copy)
        return -1;
    memcpy(copy, data, len);
    pthread_mutex_lock(&w->lock);
    if (w->tail - w->head >= QUEUE_ITEMS) {
        w->blocked++;
        while (w->tail - w->head >= QUEUE_ITEMS && !w->failed)
            pthread_cond_wait(&w->wake, &w->lock);
    }
    w->items[w->tail % QUEUE_ITEMS] = (item_t){ video, len, copy };
    w->tail++;
    if (w->tail - w->head > w->max_fill)
        w->max_fill = w->tail - w->head;
    int failed = w->failed;
    pthread_cond_broadcast(&w->wake);
    pthread_mutex_unlock(&w->lock);
    return failed ? -1 : 0;
}

static void report(writer_t *w, int final)
{
    double t = now_s();
    if (!final && t - w->last_report < 1.0)
        return;
    w->last_report = t;
    const pinnacle_capture_stats_t *s = w->stats;
    fprintf(stderr,
            "%s%6.1f s  frames %lu  missing %lu  truncated %lu  audio %lu (missing %lu)  "
            "queue max %u%s",
            final ? "" : "\r", t - w->t0, s->frames, s->frames_missing, s->frames_truncated,
            s->audio_blocks, s->audio_missing, w->max_fill, final ? "\n" : "  ");
}

static int on_video(const pinnacle_video_frame_t *f, void *user)
{
    writer_t *w = user;
    size_t full = (size_t)f->width * f->height * 2;
    if (f->repeated && f->received)
        fprintf(stderr, "\npinanalog: frame %u at %.2f s arrived short (%zu of %zu bytes), "
                "repeating the previous one\n", f->index, now_s() - w->t0, f->received, full);
    else if (f->repeated)
        fprintf(stderr, "\npinanalog: frame %u at %.2f s never arrived, repeating the previous one\n",
                f->index, now_s() - w->t0);

    if (w->avi && writer_push(w, 1, f->yuyv, full) != 0)
        return 1;
    report(w, 0);
    return w->deadline > 0 && now_s() > w->deadline;
}

static int on_audio(const pinnacle_audio_block_t *b, void *user)
{
    writer_t *w = user;
    return w->avi && writer_push(w, 0, b->pcm, (size_t)b->samples * 4) != 0;
}

/* --- main ----------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *bitstream = "firmware/fpga-capture.bin", *raw_path = NULL, *out_path = NULL;
    double seconds = 0;
    int status_only = 0, auto_std = 1;
    pinnacle_analog_config_t cfg;
    pinnacle_analog_config_defaults(&cfg);

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *val = i + 1 < argc ? argv[i + 1] : NULL;
        int *pic = NULL;
        if (strcmp(arg, "--brightness") == 0)
            pic = &cfg.picture.brightness;
        else if (strcmp(arg, "--contrast") == 0)
            pic = &cfg.picture.contrast;
        else if (strcmp(arg, "--saturation") == 0)
            pic = &cfg.picture.saturation;
        else if (strcmp(arg, "--hue") == 0)
            pic = &cfg.picture.hue;
        else if (strcmp(arg, "--sharpness") == 0)
            pic = &cfg.picture.sharpness;

        if (pic && val) {
            *pic = atoi(val);
            i++;
        } else if (strcmp(arg, "-b") == 0 && val) {
            bitstream = val;
            i++;
        } else if (strcmp(arg, "-o") == 0 && val) {
            out_path = val;
            i++;
        } else if (strcmp(arg, "-i") == 0 && val) {
            if (strcmp(val, "composite") == 0)
                cfg.input = PINNACLE_INPUT_COMPOSITE;
            else if (strcmp(val, "svideo") == 0)
                cfg.input = PINNACLE_INPUT_SVIDEO;
            else {
                usage();
                return 2;
            }
            i++;
        } else if (strcmp(arg, "-s") == 0 && val) {
            auto_std = strcmp(val, "auto") == 0;
            if (!auto_std && parse_std(val, &cfg.standard) != 0) {
                usage();
                return 2;
            }
            i++;
        } else if (strcmp(arg, "-t") == 0 && val) {
            seconds = atof(val);
            i++;
        } else if (strcmp(arg, "--raw") == 0 && val) {
            raw_path = val;
            i++;
        } else if (strcmp(arg, "--tv-mode") == 0) {
            cfg.vcr_mode = 0;
        } else if (strcmp(arg, "--status") == 0) {
            status_only = 1;
        } else {
            usage();
            return 2;
        }
    }
    if (!bitstream) {
        usage();
        return 2;
    }

    pinnacle_device_t dev;
    pinnacle_status_t st = pinnacle_open(&dev);
    if (st != PINNACLE_OK) {
        fprintf(stderr, "pinanalog: %s\n", pinnacle_strerror(st));
        return 1;
    }
    pinnacle_tuning_from_env(&dev.tuning);

    /* Cross-process ownership (pinnacle_lock.h): refuse to fight another
     * pincli/pindeck/pinanalog -- or a future pinctl -- over this device. */
    char device_id[PINNACLE_ENUM_ID_MAX];
    pinnacle_enum_build_id(libusb_get_device(dev.handle), device_id, sizeof(device_id));
    pinnacle_lock_t *lock = NULL;
    st = pinnacle_lock_acquire(device_id, &lock);
    if (st != PINNACLE_OK) {
        fprintf(stderr, "pinanalog: %s\n", st == PINNACLE_ERR_BUSY
                ? "device is already in use by another process" : "failed to take the device lock");
        pinnacle_close(&dev);
        return 1;
    }

    pinnacle_analog_t a;
    double t_open = now_s();
    st = pinnacle_analog_open(&a, &dev, bitstream, &cfg);
    if (st != PINNACLE_OK) {
        fprintf(stderr, "pinanalog: bring-up failed: %s\n", pinnacle_strerror(st));
        pinnacle_lock_release(lock);
        pinnacle_close(&dev);
        return 1;
    }
    pinnacle_lock_update(lock, PINNACLE_LOCK_READY, dev.guid_hi, dev.guid_lo);

    /* The decoder needs a moment to lock after the input is selected. */
    pinnacle_analog_status_t s = { 0 };
    for (int i = 0; i < 20; i++) {
        if (pinnacle_analog_get_status(&a, &s) == PINNACLE_OK && s.locked)
            break;
        struct timespec ts = { 0, 50 * 1000000L };
        nanosleep(&ts, NULL);
    }
    fprintf(stderr, "pinanalog: capture mode up in %.1f s; %s input: %s, %s (status 0x%02x)\n",
            now_s() - t_open, cfg.input == PINNACLE_INPUT_SVIDEO ? "S-video" : "composite",
            s.locked ? "signal locked" : "NO SIGNAL", s.is_60hz ? "60 Hz" : "50 Hz", s.raw);
    if (auto_std && s.locked && s.is_60hz != pinnacle_std_is_60hz(cfg.standard))
        pinnacle_analog_set_standard(&a, s.is_60hz ? PINNACLE_STD_NTSC : PINNACLE_STD_PAL);
    fprintf(stderr, "pinanalog: standard %s, %ux%u\n", pinnacle_std_name(a.cfg.standard),
            a.width, a.height);
    if (status_only) {
        pinnacle_lock_release(lock);
        pinnacle_close(&dev);
        return s.locked ? 0 : 3;
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    if ((st = pinnacle_analog_start(&a)) != PINNACLE_OK) {
        fprintf(stderr, "pinanalog: start failed: %s\n", pinnacle_strerror(st));
        pinnacle_lock_release(lock);
        pinnacle_close(&dev);
        return 1;
    }
    pinnacle_lock_update(lock, PINNACLE_LOCK_CAPTURING, dev.guid_hi, dev.guid_lo);

    int rc = 0;
    if (raw_path) {
        raw_ctx_t ctx = { 0 };
        if (!(ctx.raw = fopen(raw_path, "wb"))) {
            perror(raw_path);
            rc = 1;
        } else {
            ctx.t0 = now_s();
            ctx.deadline = seconds > 0 ? ctx.t0 + seconds : 0;
            st = pinnacle_analog_read_loop(&a, raw_cb, &ctx, &g_stop);
            fclose(ctx.raw);
        }
    } else {
        pinnacle_capture_stats_t stats;
        writer_t *w = calloc(1, sizeof(*w));
        pthread_mutex_init(&w->lock, NULL);
        pthread_cond_init(&w->wake, NULL);
        w->stats = &stats;
        int is60 = pinnacle_std_is_60hz(a.cfg.standard);
        if (out_path) {
            w->avi = avi_open(out_path, a.width, a.height, is60 ? 30000 : 25, is60 ? 1001 : 1,
                              4, 3, 48000, 2);
            if (!w->avi) {
                perror(out_path);
                rc = 1;
            } else {
                pthread_create(&w->thread, NULL, writer_main, w);
            }
        }
        if (rc == 0) {
            pinnacle_capture_sink_t sink = { .video = on_video, .audio = on_audio, .user = w };
            w->t0 = w->last_report = now_s();
            w->deadline = seconds > 0 ? w->t0 + seconds : 0;
            st = pinnacle_analog_capture_loop(&a, &sink, &stats, &g_stop);
            report(w, 1);
            if (w->avi) {
                pthread_mutex_lock(&w->lock);
                w->closed = 1;
                pthread_cond_broadcast(&w->wake);
                pthread_mutex_unlock(&w->lock);
                pthread_join(w->thread, NULL);
                if (avi_close(w->avi) != 0 || w->failed) {
                    fprintf(stderr, "pinanalog: writing %s failed\n", out_path);
                    rc = 1;
                }
            }
            if (w->blocked)
                fprintf(stderr, "pinanalog: WARNING: the disk fell behind %lu times\n",
                        w->blocked);
            if (stats.frames_missing || stats.frames_truncated || stats.audio_missing)
                rc = rc ? rc : 4;
        }
        free(w);
    }

    pinnacle_analog_stop(&a);
    if (st != PINNACLE_OK) {
        fprintf(stderr, "pinanalog: %s\n", pinnacle_strerror(st));
        rc = 1;
    }
    pinnacle_lock_release(lock);
    pinnacle_close(&dev);
    return rc;
}
