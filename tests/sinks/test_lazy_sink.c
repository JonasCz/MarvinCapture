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
 * ctest: pin_sink_lazy() -- a file sink that creates its file with the first
 * video unit: nothing is created (or touched) when no video arrives, whatever
 * the container, and the first unit makes a normal file.
 */

#include "test_util.h"
#include "../../src/sinks/pin_sink.h"
#include "../../src/engine/dv_subcode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

static long size_of(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

static pin_sink_params_t params(pin_kind_t kind, int w, int h, int fn, int fd, pin_aspect_t aspect)
{
    pin_sink_params_t p;
    memset(&p, 0, sizeof(p));
    p.kind = kind;
    p.width = w;
    p.height = h;
    p.fps_num = fn;
    p.fps_den = fd;
    p.interlaced = 1;
    p.top_field_first = 1;
    p.aspect = aspect;
    p.audio_rate = 48000;
    p.audio_channels = 2;
    p.colour_matrix = PIN_MATRIX_BT601;
    return p;
}

/* opens a lazy sink of `format`, closes it without a unit: no file at all */
static void empty_leaves_nothing(pin_format_t format, pin_kind_t kind, const char *path, const char *tmp_extra)
{
    pin_sink_params_t p = kind == PIN_KIND_HDV ? params(kind, 1440, 1080, 25, 1, PIN_ASPECT_16_9)
                                               : params(kind, 720, 576, 25, 1, PIN_ASPECT_4_3);
    remove(path);
    pin_sink_t *s = pin_sink_lazy(pin_sink_create(format));
    CHECK(s);
    CHECK_EQ_I(s->open(s, path, &p), PIN_OK);
    CHECK(!exists(path));                       /* open() creates nothing */
    if (tmp_extra)
        CHECK(!exists(tmp_extra));
    pin_sink_status_t st;
    s->get_status(s, &st);
    CHECK_EQ_I(st.units_written, 0);
    CHECK_EQ_I(s->close(s), PIN_OK);
    CHECK(!exists(path));
    if (tmp_extra)
        CHECK(!exists(tmp_extra));
}

int main(void)
{
    const size_t frame = (size_t)DV_SEQ_COUNT_PAL * DV_SEQ_SIZE;
    uint8_t *fr = calloc(1, frame);
    CHECK(fr);

    /* nothing arrives: no file, in every container */
    empty_leaves_nothing(PIN_FMT_DV_RAW, PIN_KIND_DV, "lazy_a.dv", NULL);
    empty_leaves_nothing(PIN_FMT_HDV_TS, PIN_KIND_HDV, "lazy_a.ts", NULL);
    empty_leaves_nothing(PIN_FMT_DV_AVI, PIN_KIND_DV, "lazy_a.avi", NULL);
    empty_leaves_nothing(PIN_FMT_DV_MOV, PIN_KIND_DV, "lazy_a.mov", NULL);
    empty_leaves_nothing(PIN_FMT_HDV_MKV, PIN_KIND_HDV, "lazy_a.mkv", "lazy_a.mkv.rawts.tmp");
    empty_leaves_nothing(PIN_FMT_HDV_MOV, PIN_KIND_HDV, "lazy_a_h.mov", "lazy_a_h.mov.rawts.tmp");
    empty_leaves_nothing(PIN_FMT_ANALOG_AVI, PIN_KIND_ANALOG, "lazy_a2.avi", NULL);
    empty_leaves_nothing(PIN_FMT_ANALOG_FFV1_MKV, PIN_KIND_ANALOG, "lazy_a2.mkv", NULL);
    printf("OK: no unit, no file (raw, rewrap, analog)\n");

    /* an existing file is not truncated before there is something to replace it with */
    {
        FILE *f = fopen("lazy_keep.dv", "wb");
        CHECK(f);
        fwrite("old take", 1, 8, f);
        fclose(f);
        pin_sink_params_t p = params(PIN_KIND_DV, 720, 576, 25, 1, PIN_ASPECT_4_3);
        pin_sink_t *s = pin_sink_lazy(pin_sink_create(PIN_FMT_DV_RAW));
        CHECK(s && s->open(s, "lazy_keep.dv", &p) == PIN_OK);
        CHECK_EQ_I(size_of("lazy_keep.dv"), 8);
        CHECK_EQ_I(s->close(s), PIN_OK);
        CHECK_EQ_I(size_of("lazy_keep.dv"), 8);
        remove("lazy_keep.dv");
    }
    printf("OK: an existing file is untouched without video\n");

    /* the first unit creates the file; units are counted */
    {
        pin_sink_params_t p = params(PIN_KIND_DV, 720, 576, 25, 1, PIN_ASPECT_4_3);
        pin_sink_t *s = pin_sink_lazy(pin_sink_create(PIN_FMT_DV_RAW));
        CHECK(s && s->open(s, "lazy_b.dv", &p) == PIN_OK);
        CHECK(!exists("lazy_b.dv"));
        CHECK_EQ_I(s->write_unit(s, fr, frame), PIN_OK);
        CHECK(exists("lazy_b.dv"));
        CHECK_EQ_I(s->write_unit(s, fr, frame), PIN_OK);
        pin_sink_status_t st;
        s->get_status(s, &st);
        CHECK_EQ_I(st.units_written, 2);
        CHECK_EQ_I(s->close(s), PIN_OK);
        CHECK_EQ_I(size_of("lazy_b.dv"), 2 * (long)frame);
        remove("lazy_b.dv");
    }

    /* analog: audio before the first frame is dropped and does not create the file */
    {
        pin_sink_params_t p = params(PIN_KIND_ANALOG, 720, 576, 25, 1, PIN_ASPECT_4_3);
        pin_sink_t *s = pin_sink_lazy(pin_sink_create(PIN_FMT_ANALOG_AVI));
        int16_t pcm[2 * 100] = { 0 };
        CHECK(s && s->open(s, "lazy_c.avi", &p) == PIN_OK);
        CHECK_EQ_I(s->write_audio(s, pcm, 100), PIN_OK);
        CHECK(!exists("lazy_c.avi"));
        uint8_t *yuyv = calloc(1, 720 * 576 * 2);
        CHECK(yuyv);
        CHECK_EQ_I(s->write_video(s, yuyv, 720 * 576 * 2), PIN_OK);
        CHECK(exists("lazy_c.avi"));
        CHECK_EQ_I(s->write_audio(s, pcm, 100), PIN_OK);
        pin_sink_status_t st;
        s->get_status(s, &st);
        CHECK_EQ_I(st.units_written, 1);
        CHECK_EQ_I(s->close(s), PIN_OK);
        CHECK(size_of("lazy_c.avi") > 720 * 576 * 2);
        remove("lazy_c.avi");
        free(yuyv);
    }

    /* a directory that is not there is reported by open(), as before */
    {
        pin_sink_params_t p = params(PIN_KIND_DV, 720, 576, 25, 1, PIN_ASPECT_4_3);
        pin_sink_t *s = pin_sink_lazy(pin_sink_create(PIN_FMT_DV_RAW));
        CHECK(s);
        CHECK_EQ_I(s->open(s, "no_such_dir_lazy/x.dv", &p), PIN_ERR_IO);
        s->close(s);
    }
    printf("OK: first unit creates the file; audio first is dropped; missing directory\n");
    free(fr);
    return 0;
}
