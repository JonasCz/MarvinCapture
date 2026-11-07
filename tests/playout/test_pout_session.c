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
 *  - HDV: what comes out is a contiguous piece of the input (after PAT/PMT) and the run ends;
 *  - mixed tapes (DV PAL + HDV + DV NTSC, ...): the format switch policy of pout_switch.h: no packets
 *    in the gap, empty packets of the new format in the blank time, the tape frames of both lost,
 *    a new output file at each DV <-> HDV change, PAL <-> NTSC in one .dv, overrides, a switch cut
 *    short by a stream stop, pause on both sides.
 */

#include "pout_session.h"
#include "pout_switch.h"

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

/* ---- mixed tapes ---------------------------------------------------------------------------- */

#define MAXF 4

typedef struct {
    pout_out_t base;
    pout_out_t *in;                 /* the file output behind it */
    unsigned long pkts, empty;      /* packets that reached the output, those of them without payload */
    unsigned long empty_after_sw;   /* empty packets since the last phase 1 ... */
    unsigned long first_data_empties; /* ... up to the first data packet (kept) */
    int counting_after_sw;
} cnt_out_t;

static int c_start(pout_out_t *o) { return ((cnt_out_t *)o)->in->start(((cnt_out_t *)o)->in); }
static void c_stop(pout_out_t *o) { ((cnt_out_t *)o)->in->stop(((cnt_out_t *)o)->in); }
static void c_destroy(pout_out_t *o) { ((cnt_out_t *)o)->in->destroy(((cnt_out_t *)o)->in); }
static int c_format(pout_out_t *o, const pout_frame_fmt_t *f)
{
    cnt_out_t *c = (cnt_out_t *)o;
    return c->in->format ? c->in->format(c->in, f) : 0;
}
static int c_packet(pout_out_t *o, const uint8_t *pkt, unsigned len)
{
    cnt_out_t *c = (cnt_out_t *)o;
    c->pkts++;
    if (len == 8) {
        c->empty++;
        if (c->counting_after_sw)
            c->empty_after_sw++;
    } else if (c->counting_after_sw) {
        c->first_data_empties = c->empty_after_sw;
        c->counting_after_sw = 0;
    }
    return c->in->packet(c->in, pkt, len);
}

typedef struct {
    cnt_out_t *co;
    int periods, tx_off_from, tx_off_to;
    int begins, applies, apply_streaming;
    unsigned long pkts_at_begin, pkts_at_apply;
    uint32_t cycle_at_begin, cycle_at_apply;
    pout_cip_kind_t kind_at_apply;
} mctx_t;

static int m_tx_wanted(void *u)
{
    mctx_t *c = (mctx_t *)u;
    c->periods++;
    return !(c->tx_off_to && c->periods >= c->tx_off_from && c->periods < c->tx_off_to);
}
static void m_switch(void *u, int phase, const pout_frame_fmt_t *to, const pout_switch_policy_t *pol, int streaming,
                     pout_cip_t *cip)
{
    mctx_t *c = (mctx_t *)u;
    (void)pol;
    if (phase == 0) {
        c->begins++;
        c->pkts_at_begin = c->co->pkts;
        c->cycle_at_begin = pout_cip_cycle(cip);
    } else {
        c->applies++;
        c->apply_streaming = streaming;
        c->pkts_at_apply = c->co->pkts;
        c->cycle_at_apply = pout_cip_cycle(cip);
        c->kind_at_apply = cip->kind;
        c->co->empty_after_sw = 0;
        c->co->counting_after_sw = 1;
        CHECK((to->media == POUT_MEDIA_HDV) == (cip->kind == POUT_CIP_HDV));
    }
}

typedef struct {
    int code;
    char err[256];
    unsigned long sent;
    int nfiles;
    uint8_t *data[MAXF];
    size_t len[MAXF];
    mctx_t m;
} mres_t;

/* switches: NULL or one --switch spec */
static void mrun(const char *const *argv, int argc, const char *sw, int tx_off_from, int tx_off_to, mres_t *r)
{
    memset(r, 0, sizeof *r);
    pout_script_t *sc = NULL;
    char perr[256] = "";
    pout_switch_reset();
    if (pout_script_parse(argc, argv, &sc, perr, sizeof perr) != PIN_OK) {
        fprintf(stderr, "parse: %s\n", perr);
        failures++;
        return;
    }
    if (sw && pout_switch_set(sw, perr, sizeof perr) != 0) {
        fprintf(stderr, "switch: %s\n", perr);
        failures++;
    }
    pout_tape_t *tape = NULL;
    if (pout_tape_open(&tape, (const char *const *)sc->files, sc->nfiles, perr, sizeof perr) != 0) {
        fprintf(stderr, "tape: %s\n", perr);
        failures++;
        pout_script_destroy(sc);
        return;
    }
    cnt_out_t *co = (cnt_out_t *)calloc(1, sizeof *co);
    co->in = pout_out_file_create(tmp_name(".mix"), perr, sizeof perr);
    CHECK(co->in != NULL);
    co->base.start = c_start;
    co->base.packet = c_packet;
    co->base.stop = c_stop;
    co->base.destroy = c_destroy;
    co->base.format = c_format;
    r->m.co = co;
    r->m.tx_off_from = tx_off_from;
    r->m.tx_off_to = tx_off_to;
    pout_session_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.tape = tape;
    cfg.sc = sc;
    cfg.out = &co->base;
    cfg.user = &r->m;
    cfg.hooks.tx_wanted = m_tx_wanted;
    cfg.hooks.format_switch = m_switch;
    pout_session_t *se = pout_session_create(&cfg, perr, sizeof perr);
    CHECK(se != NULL);
    r->code = pout_session_run(se, r->err, sizeof r->err);
    r->sent = pout_session_frames_sent(se);
    pout_session_destroy(se);
    r->nfiles = pout_out_file_count(co->in);
    char names[MAXF][1100];
    for (int i = 0; i < r->nfiles && i < MAXF; i++)
        snprintf(names[i], sizeof names[i], "%s", pout_out_file_name(co->in, i));
    co->base.destroy(&co->base);        /* closes the files */
    for (int i = 0; i < r->nfiles && i < MAXF; i++) {
        r->data[i] = slurp(names[i], &r->len[i]);
        remove(names[i]);
    }
    pout_tape_close(tape);
    pout_script_destroy(sc);
    pout_switch_reset();
}

static void mfree(mres_t *r)
{
    for (int i = 0; i < MAXF; i++)
        free(r->data[i]);
    free(r->m.co);
}

/* out (after the reassembler's PAT and PMT) is a contiguous run of whole packets of h */
static int is_piece_of(const uint8_t *out, size_t out_len, const uint8_t *h, size_t hlen, size_t *at)
{
    if (out_len < 3 * 188 || out_len % 188 || out[0] != 0x47)
        return 0;
    size_t body = out_len - 2 * 188;
    for (size_t off = 0; off + body <= hlen; off += 188)
        if (memcmp(h + off, out + 2 * 188, body) == 0) {
            if (at)
                *at = off;
            return 1;
        }
    return 0;
}

static void test_mixed(const char *pal, const char *hdv, const char *ntsc)
{
    size_t pal_len = 0, hdv_len = 0, ntsc_len = 0;
    uint8_t *pd = slurp(pal, &pal_len), *hd = slurp(hdv, &hdv_len), *nd = slurp(ntsc, &ntsc_len);
    if (!pd || !hd || !nd) {
        free(pd);
        free(hd);
        free(nd);
        return;
    }
    mres_t r;
    size_t at;

    /* 1. DV PAL + HDV + DV NTSC with a short policy: kind=gap:0.4,blank:0.2.
     *    PAL -> HDV: the gap is 3200 cycles = 10 PAL periods, the blank 1600 cycles = 5 HDV (25 fps)
     *    periods: tape frames 50..64 are lost, HDV plays from 65 (its GOP start at or before).
     *    HDV -> NTSC: the gap is 10 HDV periods, the blank 6 NTSC periods: 16 of the 30 NTSC frames
     *    are lost, the third file holds frames 16..29. */
    {
        const char *a[] = { pal, hdv, ntsc };
        char sw[] = "kind=gap:0.4,blank:0.2";
        mrun(a, 3, sw, 0, 0, &r);
        CHECK(r.code == POUT_EXIT_OK);
        CHECK(r.nfiles == 3);
        CHECK(r.m.begins == 2 && r.m.applies == 2 && r.m.apply_streaming == 1);
        if (r.nfiles == 3) {
            CHECK(r.len[0] == pal_len && r.data[0] && memcmp(r.data[0], pd, pal_len) == 0);   /* DV before the gap, whole */
            CHECK(is_piece_of(r.data[1], r.len[1], hd, hdv_len, &at));
            CHECK(r.len[1] > 188 * 100 && r.len[1] < hdv_len);          /* the first 15 frames' worth is lost */
            CHECK(r.len[2] == 14 * 120000u && memcmp(r.data[2], nd + 16 * 120000u, r.len[2]) == 0);
        }
        mfree(&r);
    }

    /* 2. the same without any loss: gap and blank 0 */
    {
        const char *a[] = { pal, hdv, ntsc };
        mrun(a, 3, "kind=gap:0,blank:0", 0, 0, &r);
        CHECK(r.code == POUT_EXIT_OK && r.nfiles == 3);
        if (r.nfiles == 3) {
            CHECK(r.len[0] == pal_len && memcmp(r.data[0], pd, pal_len) == 0);
            CHECK(is_piece_of(r.data[1], r.len[1], hd, hdv_len, &at) && r.len[1] >= hdv_len * 9 / 10);   /* from the first GOP to the last whole picture */
            CHECK(r.len[2] == ntsc_len && memcmp(r.data[2], nd, ntsc_len) == 0);
        }
        mfree(&r);
    }

    /* 3. the defaults, DV -> HDV: nothing in the gap (the cycle count runs on by 8000), then empty
     *    packets of the new format for the blank time (5 HDV periods = 1600 cycles) */
    {
        const char *a[] = { pal, hdv };
        mrun(a, 2, NULL, 0, 0, &r);
        CHECK(r.code == POUT_EXIT_OK && r.nfiles == 2);
        CHECK(r.m.begins == 1 && r.m.applies == 1);
        CHECK(r.m.pkts_at_apply == r.m.pkts_at_begin);               /* no packets at all in the gap */
        CHECK(r.m.cycle_at_apply - r.m.cycle_at_begin == 8000u);     /* 1.0 s of bus cycles went by */
        CHECK(r.m.kind_at_apply == POUT_CIP_HDV);
        /* from the new format on: at least the blank time of empty packets, then the frame's own */
        CHECK(r.m.co->first_data_empties >= 1600 && r.m.co->first_data_empties < 1600 + 320);
        if (r.nfiles == 2)
            CHECK(r.len[0] == pal_len && is_piece_of(r.data[1], r.len[1], hd, hdv_len, &at));
        mfree(&r);
    }

    /* 4. the empty packets between the gap and the first frame: exactly the blank time (HDV -> DV,
     *    0.2 s = 1600 cycles = 6 NTSC periods of 266 or 267 cycles = 1601) before the first data packet */
    {
        const char *a[] = { hdv, ntsc };
        mrun(a, 2, "kind=gap:0.2,blank:0.2", 0, 0, &r);
        CHECK(r.code == POUT_EXIT_OK && r.nfiles == 2);
        CHECK(r.m.pkts_at_apply == r.m.pkts_at_begin);
        CHECK(r.m.cycle_at_apply - r.m.cycle_at_begin == 1600u);
        /* a DV frame period starts with a data packet: all the empty packets before it are the blank time */
        CHECK(r.m.co->first_data_empties == 1601);
        mfree(&r);
    }

    /* 5. PAL + NTSC: one .dv, the STD default (no gap, 0.5 s blank = 4000 cycles = 15 NTSC periods) */
    {
        const char *a[] = { pal, ntsc };
        mrun(a, 2, NULL, 0, 0, &r);
        CHECK(r.code == POUT_EXIT_OK);
        CHECK(r.nfiles == 1);
        CHECK(r.m.begins == 1 && r.m.pkts_at_apply == r.m.pkts_at_begin);   /* no gap: nothing is skipped */
        CHECK(r.m.cycle_at_apply == r.m.cycle_at_begin);
        if (r.nfiles == 1) {
            CHECK(r.len[0] == pal_len + 15 * 120000u);
            CHECK(memcmp(r.data[0], pd, pal_len) == 0);
            CHECK(memcmp(r.data[0] + pal_len, nd + 15 * 120000u, 15 * 120000u) == 0);
        }
        mfree(&r);
    }

    /* 6. nobody receiving during a gap: the switch is cut short (streaming 0), the tape goes on in
     *    the new format when the receiver is back */
    {
        const char *a[] = { pal, hdv };
        mrun(a, 2, NULL, 85, 95, &r);
        CHECK(r.code == POUT_EXIT_OK && r.nfiles == 2);
        CHECK(r.m.begins == 1 && r.m.applies == 1 && r.m.apply_streaming == 0);
        if (r.nfiles == 2)
            CHECK(is_piece_of(r.data[1], r.len[1], hd, hdv_len, &at));
        mfree(&r);
    }

    /* 7. pause after the switch to DV (HDV + PAL): the DV frame repeats; and in the HDV part (PAL + HDV)
     *    a pause sends nothing and the run still ends */
    {
        const char *a[] = { hdv, pal, "--play", "--wait", "wallclock=00:00:04", "--pause", "--wait", "wallclock=00:00:02",
                            "--stop" };
        mrun(a, 9, "kind=gap:0.2,blank:0", 0, 0, &r);
        CHECK(r.code == POUT_EXIT_OK && r.nfiles == 2);
        if (r.nfiles == 2 && r.len[1] >= 3 * 144000u && r.len[1] % 144000u == 0) {
            size_t n = r.len[1] / 144000u;
            CHECK(memcmp(r.data[1] + (n - 1) * 144000u, r.data[1] + (n - 2) * 144000u, 144000u) == 0);   /* repeated */
        } else {
            CHECK(0 && "DV part after HDV");
        }
        mfree(&r);
        const char *b[] = { pal, hdv, "--play", "--wait", "wallclock=00:00:04", "--pause", "--wait", "wallclock=00:00:02",
                            "--stop" };
        mrun(b, 9, "kind=gap:0.2,blank:0", 0, 0, &r);
        CHECK(r.code == POUT_EXIT_OK && r.nfiles == 2);
        CHECK(r.nfiles == 2 && r.len[0] == pal_len && r.len[1] > 188 * 20);
        mfree(&r);
    }

    free(pd);
    free(hd);
    free(nd);
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

    {
        char ntsc[1024];
        snprintf(ntsc, sizeof ntsc, "%s/dv-ntsc.dv", dir);
        test_mixed(pal, hdv, ntsc);
    }

    free(src);
    if (failures)
        fprintf(stderr, "%d failure(s)\n", failures);
    else
        printf("test_pout_session: ok\n");
    return failures ? 1 : 0;
}
