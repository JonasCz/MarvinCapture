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
#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

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

#if !defined(_WIN32)
/* numbered output: "lazy_n-NNNN.dv" from 5 on (the engine builds these with pin_naming_build) */
static int name_after(void *ctx, unsigned k, char *out, size_t cap)
{
    unsigned first = *(const unsigned *)ctx;
    snprintf(out, cap, "lazy_n-%04u.dv", first + k);
    return 0;
}

static int g_ctx_freed;
static void ctx_free(void *ctx)
{
    (void)ctx;
    g_ctx_freed++;
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    CHECK(f);
    fputs(text, f);
    fclose(f);
}
#endif

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

/* A sink whose open() creates the file and then fails, as ffv1_open() / hwenc_open() do when the
 * header or the encoder fails after avio_open(). */
static pin_status_t stub_open(pin_sink_t *s, const char *path, const pin_sink_params_t *params)
{
    (void)params;
    if (!s->priv)
        return PIN_ERR_IO;          /* refuses without touching the file */
    FILE *f = fopen(path, "wb");
    CHECK(f);
    fputs("partial", f);
    fclose(f);
    return PIN_ERR_CODEC;
}
static pin_status_t stub_write(pin_sink_t *s, const uint8_t *d, size_t n)
{
    (void)s; (void)d; (void)n;
    return PIN_OK;
}
static pin_status_t stub_close(pin_sink_t *s)
{
    free(s);
    return PIN_OK;
}
static pin_sink_t *stub_sink(int creates)
{
    pin_sink_t *s = calloc(1, sizeof(*s));
    CHECK(s);
    s->priv = creates ? (void *)s : NULL;
    s->open = stub_open;
    s->write_unit = stub_write;
    s->write_video = (pin_status_t (*)(pin_sink_t *, const uint8_t *, size_t))stub_write;
    s->close = stub_close;
    return s;
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

    /* a unit the sink drops as damaged is not video: no count, and a file with only those is removed */
    {
        pin_sink_params_t p = params(PIN_KIND_DV, 720, 576, 25, 1, PIN_ASPECT_4_3);
        pin_sink_t *s = pin_sink_lazy(pin_sink_create(PIN_FMT_DV_RAW));
        CHECK(s && s->open(s, "lazy_dmg.dv", &p) == PIN_OK);
        CHECK_EQ_I(s->write_unit(s, fr, 1000), PIN_OK);
        pin_sink_status_t st;
        s->get_status(s, &st);
        CHECK_EQ_I(st.units_written, 0);
        CHECK_EQ_I(s->close(s), PIN_OK);
        CHECK(!exists("lazy_dmg.dv"));

        s = pin_sink_lazy(pin_sink_create(PIN_FMT_DV_RAW));
        CHECK(s && s->open(s, "lazy_dmg.dv", &p) == PIN_OK);
        CHECK_EQ_I(s->write_unit(s, fr, 1000), PIN_OK);
        CHECK_EQ_I(s->write_unit(s, fr, frame), PIN_OK);
        s->get_status(s, &st);
        CHECK_EQ_I(st.units_written, 1);
        CHECK_EQ_I(s->close(s), PIN_OK);
        CHECK_EQ_I(size_of("lazy_dmg.dv"), (long)frame);
        remove("lazy_dmg.dv");
    }
    printf("OK: damaged units are not counted as written\n");

    /* an open that fails after creating the file leaves nothing; one that fails before keeps the old file */
    {
        pin_sink_params_t p = params(PIN_KIND_DV, 720, 576, 25, 1, PIN_ASPECT_4_3);
        pin_sink_t *s = pin_sink_lazy(stub_sink(1));
        CHECK(s && s->open(s, "lazy_fail.dv", &p) == PIN_OK);
        CHECK_EQ_I(s->write_unit(s, fr, frame), PIN_ERR_CODEC);
        CHECK(exists("lazy_fail.dv"));
        CHECK_EQ_I(s->close(s), PIN_ERR_CODEC);
        CHECK(!exists("lazy_fail.dv"));

        FILE *of = fopen("lazy_old.dv", "wb");
        CHECK(of);
        fputs("old take", of);
        fclose(of);
        s = pin_sink_lazy(stub_sink(0));
        CHECK(s && s->open(s, "lazy_old.dv", &p) == PIN_OK);
        CHECK_EQ_I(s->write_unit(s, fr, frame), PIN_ERR_IO);
        CHECK_EQ_I(s->close(s), PIN_ERR_IO);
        CHECK_EQ_I(size_of("lazy_old.dv"), 8);
        remove("lazy_old.dv");
    }
    printf("OK: a failed open leaves no file of its own\n");

#if !defined(_WIN32)
    /* the file cannot be created (the name is a directory): close() must not write a header into a
     * muxer without a file */
    {
        pin_sink_params_t p = params(PIN_KIND_DV, 720, 576, 25, 1, PIN_ASPECT_4_3);
        pin_format_t fmts[] = { PIN_FMT_DV_AVI, PIN_FMT_DV_MOV };
        for (int i = 0; i < 2; i++) {
            const char *dir = i ? "lazy_isdir.mov" : "lazy_isdir.avi";
            CHECK(mkdir(dir, 0777) == 0);
            pin_sink_t *s = pin_sink_lazy(pin_sink_create(fmts[i]));
            CHECK(s && s->open(s, dir, &p) == PIN_OK);
            CHECK(s->write_unit(s, fr, frame) != PIN_OK);
            CHECK(s->close(s) != PIN_OK);
            rmdir(dir);
        }
    }
    printf("OK: a file that cannot be created does not crash close\n");
#endif

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

#if !defined(_WIN32)
    /* numbered output: a name that cannot be created (here: taken by an existing file; on
     * Android a file of an earlier installation the app cannot see) moves the file on to the
     * next free number, without touching the taken ones */
    {
        static unsigned first = 5;
        write_file("lazy_n-0005.dv", "taken");
        write_file("lazy_n-0006.dv", "taken too");
        remove("lazy_n-0007.dv");
        pin_sink_params_t p = params(PIN_KIND_DV, 720, 576, 25, 1, PIN_ASPECT_4_3);
        pin_sink_t *s = pin_sink_lazy(pin_sink_create(PIN_FMT_DV_RAW));
        CHECK(s);
        g_ctx_freed = 0;
        pin_sink_lazy_set_renumber(s, name_after, &first, ctx_free);
        CHECK_EQ_I(s->open(s, "lazy_n-0005.dv", &p), PIN_OK);
        char used[256] = "";
        CHECK_EQ_I(pin_sink_lazy_renumbered(s, used, sizeof(used)), 0);   /* not yet created */
        CHECK_EQ_I(s->write_unit(s, fr, frame), PIN_OK);
        CHECK_EQ_I(pin_sink_lazy_renumbered(s, used, sizeof(used)), 2);
        CHECK(strcmp(used, "lazy_n-0007.dv") == 0);
        CHECK_EQ_I(s->write_unit(s, fr, frame), PIN_OK);
        CHECK_EQ_I(s->close(s), PIN_OK);
        CHECK_EQ_I(g_ctx_freed, 1);
        CHECK_EQ_I(size_of("lazy_n-0005.dv"), 5);
        CHECK_EQ_I(size_of("lazy_n-0006.dv"), 9);
        CHECK_EQ_I(size_of("lazy_n-0007.dv"), 2 * (long)frame);
        remove("lazy_n-0005.dv");
        remove("lazy_n-0006.dv");
        remove("lazy_n-0007.dv");
    }
    /* a free name: kept, and the probe leaves nothing behind before the first unit */
    {
        static unsigned first = 10;
        remove("lazy_n-0010.dv");
        remove("lazy_n-0011.dv");
        pin_sink_params_t p = params(PIN_KIND_DV, 720, 576, 25, 1, PIN_ASPECT_4_3);
        pin_sink_t *s = pin_sink_lazy(pin_sink_create(PIN_FMT_DV_RAW));
        CHECK(s);
        pin_sink_lazy_set_renumber(s, name_after, &first, NULL);
        CHECK_EQ_I(s->open(s, "lazy_n-0010.dv", &p), PIN_OK);
        CHECK(!exists("lazy_n-0010.dv"));
        CHECK_EQ_I(s->write_unit(s, fr, frame), PIN_OK);
        CHECK_EQ_I(pin_sink_lazy_renumbered(s, NULL, 0), 0);
        CHECK_EQ_I(s->close(s), PIN_OK);
        CHECK_EQ_I(size_of("lazy_n-0010.dv"), (long)frame);
        CHECK(!exists("lazy_n-0011.dv"));
        remove("lazy_n-0010.dv");
    }
    /* not a lazy sink: the context is freed at once, nothing else happens */
    {
        pin_sink_t *s = pin_sink_create(PIN_FMT_DV_RAW);
        CHECK(s);
        g_ctx_freed = 0;
        pin_sink_lazy_set_renumber(s, name_after, NULL, ctx_free);
        CHECK_EQ_I(g_ctx_freed, 1);
        CHECK_EQ_I(pin_sink_lazy_renumbered(s, NULL, 0), 0);
        s->close(s);
    }
    printf("OK: numbered output skips taken names\n");
#endif
    free(fr);
    return 0;
}
