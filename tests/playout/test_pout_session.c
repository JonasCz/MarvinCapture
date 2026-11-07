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
 * The playout loop on the simulated clock with the file output (what --dry-run does), against the
 * generated fixtures:
 *  - no actions: the whole tape is played once, the output file is the tape byte for byte (DV);
 *  - `--rew --wait --play --wait timecode=T --stop`: exactly the frames before T come out;
 *  - a pause repeats the frame it froze on (DV);
 *  - a timecode that is not on the tape fails with POUT_EXIT_DECK; an interrupt ends with 130;
 *  - tx_wanted off: the deck runs on but nothing is sent;
 *  - HDV: what comes out is a contiguous piece of the input (after PAT/PMT) and the run ends.
 */

#include "pout_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: FAIL %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

#define DV_FRAME 144000u

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    size_t cap = 1 << 20, n = 0, k;
    uint8_t *b = (uint8_t *)malloc(cap);
    while ((k = fread(b + n, 1, cap - n, f)) > 0) {
        n += k;
        if (n == cap)
            b = (uint8_t *)realloc(b, cap *= 2);
    }
    fclose(f);
    *len = n;
    return b;
}

static char tmp_path[1024];
static const char *tmp_name(const char *suffix)
{
    const char *d = getenv("TMPDIR");
    snprintf(tmp_path, sizeof tmp_path, "%s/test_pout_session_%d%s", d && *d ? d : ".", (int)(size_t)&failures & 0xFFF, suffix);
    return tmp_path;
}

typedef struct {
    int interrupt_after;    /* periods; 0 = never */
    int periods;
    int tx_off_from, tx_off_to;
    int tx_starts, tx_stops;
} ctx_t;

static int hk_interrupted(void *u)
{
    ctx_t *c = (ctx_t *)u;
    return c->interrupt_after && c->periods >= c->interrupt_after;
}
static int hk_tx_wanted(void *u)
{
    ctx_t *c = (ctx_t *)u;
    c->periods++;       /* called once per frame period */
    return !(c->tx_off_to && c->periods >= c->tx_off_from && c->periods < c->tx_off_to);
}
static void hk_tx_changed(void *u, int on, pout_cip_t *cip)
{
    ctx_t *c = (ctx_t *)u;
    (void)cip;
    on ? c->tx_starts++ : c->tx_stops++;
}

/* Parses argv, runs a session against a file output; returns the exit code, *out_len the file size */
static int run(const char *const *argv, int argc, int serve, ctx_t *ctx, uint8_t **out, size_t *out_len,
               unsigned long *frames_sent, char *err, size_t err_cap)
{
    pout_script_t *sc = NULL;
    char perr[256] = "";
    if (pout_script_parse(argc, argv, &sc, perr, sizeof perr) != PIN_OK) {
        fprintf(stderr, "parse: %s\n", perr);
        failures++;
        return -1;
    }
    pout_tape_t *tape = NULL;
    if (pout_tape_open(&tape, (const char *const *)sc->files, sc->nfiles, perr, sizeof perr) != 0) {
        fprintf(stderr, "tape: %s\n", perr);
        failures++;
        pout_script_destroy(sc);
        return -1;
    }
    const char *path = tmp_name(".out");
    pout_out_t *o = pout_out_file_create(path, perr, sizeof perr);
    CHECK(o != NULL);
    pout_session_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.tape = tape;
    cfg.sc = sc;
    cfg.out = o;
    cfg.serve = serve;
    cfg.user = ctx;
    cfg.hooks.interrupted = hk_interrupted;
    cfg.hooks.tx_wanted = hk_tx_wanted;
    cfg.hooks.tx_changed = hk_tx_changed;
    pout_session_t *s = pout_session_create(&cfg, perr, sizeof perr);
    CHECK(s != NULL);
    int code = pout_session_run(s, err, err_cap);
    if (frames_sent)
        *frames_sent = pout_session_frames_sent(s);
    pout_session_destroy(s);
    o->destroy(o);
    *out = slurp(path, out_len);
    remove(path);
    pout_tape_close(tape);
    pout_script_destroy(sc);
    return code;
}

int main(void)
{
    const char *dir = getenv("PIN_TEST_DATA");
    char pal[1024], hdv[1024];
    if (!dir)
        return 77;
    snprintf(pal, sizeof pal, "%s/dv-pal.dv", dir);
    snprintf(hdv, sizeof hdv, "%s/hdv.ts", dir);
    size_t src_len = 0;
    uint8_t *src = slurp(pal, &src_len);
    if (!src)
        return 77;
    CHECK(src_len % DV_FRAME == 0);
    size_t nf = src_len / DV_FRAME;

    char err[256];
    uint8_t *out;
    size_t out_len;
    unsigned long sent;

    /* 1. no actions: the whole tape (two copies of the fixture), byte for byte */
    {
        const char *a[] = { pal, pal };
        ctx_t c = { 0 };
        err[0] = 0;
        int code = run(a, 2, 0, &c, &out, &out_len, &sent, err, sizeof err);
        CHECK(code == POUT_EXIT_OK);
        CHECK(out && out_len == 2 * src_len);
        CHECK(sent == 2 * nf);
        if (out && out_len == 2 * src_len)
            CHECK(memcmp(out, src, src_len) == 0 && memcmp(out + src_len, src, src_len) == 0);
        CHECK(c.tx_starts == 1 && c.tx_stops == 1);
        free(out);
    }

    /* 2. rewind, play up to a timecode, stop: the frames before it, exactly */
    {
        const char *a[] = { pal, pal, "--rew", "--wait", "--play", "--wait", "timecode=00:10:00:20", "--stop" };
        ctx_t c = { 0 };
        err[0] = 0;
        int code = run(a, 8, 0, &c, &out, &out_len, &sent, err, sizeof err);
        CHECK(code == POUT_EXIT_OK);
        CHECK(sent == 20);
        CHECK(out && out_len == 20 * (size_t)DV_FRAME);
        if (out && out_len == 20 * (size_t)DV_FRAME)
            CHECK(memcmp(out, src, out_len) == 0);
        free(out);
    }

    /* 3. goto into the middle, play a while, pause: the paused frame repeats, then the frames from the
     *    target on (the deck keeps playing for pause_delay after the pause command), then the last
     *    one repeated while the script waits 3 s */
    {
        const char *a[] = { pal, "--goto", "00:10:00:10", "--wait", "--play", "--wait", "timecode=00:10:00:15",
                            "--pause", "--wait", "wallclock=00:00:03", "--stop" };
        ctx_t c = { 0 };
        err[0] = 0;
        int code = run(a, 11, 0, &c, &out, &out_len, &sent, err, sizeof err);
        CHECK(code == POUT_EXIT_OK);
        CHECK(out && out_len > 0 && out_len % DV_FRAME == 0);
        if (out && out_len > 0 && out_len % DV_FRAME == 0) {
            size_t n = out_len / DV_FRAME;
            int *idx = (int *)malloc(n * sizeof(int));
            for (size_t k = 0; k < n; k++) {
                idx[k] = -1;
                for (size_t j = 0; j < nf; j++)
                    if (memcmp(out + k * DV_FRAME, src + j * DV_FRAME, DV_FRAME) == 0) {
                        idx[k] = (int)j;
                        break;
                    }
            }
            CHECK(idx[0] == 10);                 /* the search ended paused on its target */
            size_t k = 0;
            while (k < n && idx[k] == 10)
                k++;                              /* the repeats, then play: 11, 12, ... one by one */
            CHECK(k >= 2);
            int prev = 10;
            for (; k < n && idx[k] != prev; k++) {
                CHECK(idx[k] == prev + 1);
                prev = idx[k];
            }
            CHECK(prev >= 15 && prev <= 15 + 10);   /* played at least up to the wait, not far beyond */
            size_t rep = 0;
            for (; k < n; k++, rep++)
                CHECK(idx[k] == prev);            /* paused: the frozen frame, to the end */
            CHECK(rep > 50);
            free(idx);
        }
        free(out);
    }

    /* 4. a timecode that is not on the tape */
    {
        const char *a[] = { pal, "--goto", "00:20:00:00" };
        ctx_t c = { 0 };
        err[0] = 0;
        int code = run(a, 3, 0, &c, &out, &out_len, &sent, err, sizeof err);
        CHECK(code == POUT_EXIT_DECK);
        CHECK(strstr(err, "not on the tape") != NULL);
        free(out);
    }

    /* 5. serving (no actions, serve): runs until interrupted */
    {
        const char *a[] = { pal };
        ctx_t c = { 0 };
        c.interrupt_after = 500;
        err[0] = 0;
        int code = run(a, 1, 1, &c, &out, &out_len, &sent, err, sizeof err);
        CHECK(code == POUT_EXIT_CANCELLED);
        CHECK(sent == 0);       /* a stopped deck sends empty packets only */
        CHECK(out_len == 0);
        free(out);
    }

    /* 6. nobody receiving for a while: the tape runs on but nothing of it is sent */
    {
        const char *a[] = { pal };
        ctx_t c = { 0 };
        c.tx_off_from = 30;
        c.tx_off_to = 60;
        err[0] = 0;
        int code = run(a, 1, 0, &c, &out, &out_len, &sent, err, sizeof err);
        CHECK(code == POUT_EXIT_OK);
        CHECK(c.tx_starts == 2 && c.tx_stops == 2);
        /* the whole tape took its time, the sent part is short by the periods without a receiver */
        CHECK(sent > 0 && sent < nf);
        CHECK(out && out_len == (size_t)sent * DV_FRAME);
        free(out);
    }

    /* 7. HDV: no actions, plays the whole tape; the output is a contiguous piece of the input */
    {
        size_t hlen = 0;
        uint8_t *h = slurp(hdv, &hlen);
        if (h) {
            const char *a[] = { hdv };
            ctx_t c = { 0 };
            err[0] = 0;
            int code = run(a, 1, 0, &c, &out, &out_len, &sent, err, sizeof err);
            CHECK(code == POUT_EXIT_OK);
            CHECK(out && out_len > 1000 && out_len % 188 == 0 && out_len <= hlen + 2 * 188);
            if (out && out_len > 3 * 188 && out_len % 188 == 0) {
                /* PAT and PMT first (the reassembler's own), then input from a GOP start on */
                CHECK(out[0] == 0x47);
                size_t body = out_len - 2 * 188;
                int found = 0;
                for (size_t off = 0; off + body <= hlen && !found; off += 188)
                    found = memcmp(h + off, out + 2 * 188, body) == 0;
                CHECK(found);
            }
            free(out);
            free(h);
        }
    }

    free(src);
    if (failures)
        fprintf(stderr, "%d failure(s)\n", failures);
    else
        printf("test_pout_session: ok\n");
    return failures ? 1 : 0;
}
