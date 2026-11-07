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

#include "pout_hw_session.h"

#include "pin_exe_dir.h"
#include "pin_log.h"
#include "pin_settings.h"
#include "pout_avc.h"
#include "pout_hw_link.h"
#include "pout_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define DIR_SEP "\\"
#else
#define DIR_SEP "/"
#endif

#define BITSTREAM_NAME "fpga-ohci.bin"
#define BITSTREAM_BYTES 78422   /* every bitstream of the device, as pin_session.c checks */

/* GUESS: the first packet's presentation time is this many cycles after the cycle timer we read
 * (the read takes a USB round trip and the first packets wait in the queue for the ring to fill) */
#define START_LEAD_CYCLES 40u
#define BUS_POLL_SECONDS 0.25

/* ---- firmware ------------------------------------------------------------------------------ */

static long long file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    long long n = -1;
    if (fseek(f, 0, SEEK_END) == 0)
        n = ftell(f);
    fclose(f);
    return n;
}

int pout_hw_find_bitstream(char *path, size_t cap, char *why, size_t why_cap)
{
    char dirs[3][PIN_PATH_MAX];
    int nd = 0;
    char settings_path[PIN_PATH_MAX], base[PIN_PATH_MAX - 16];

    pin_settings_t st;
    pin_settings_init(&st);
    if (pin_settings_default_path(settings_path, sizeof(settings_path)) == 0 &&
        pin_settings_load(&st, settings_path) == 0) {
        const char *o = pin_settings_get_string(&st, "Paths", "firmware_dir", "");
        if (o[0])
            snprintf(dirs[nd++], PIN_PATH_MAX, "%s", o);
    }
    pin_settings_free(&st);
    if (pin_exe_dir(base, sizeof(base)) == 0)
        snprintf(dirs[nd++], PIN_PATH_MAX, "%s" DIR_SEP "firmware", base);
    snprintf(dirs[nd++], PIN_PATH_MAX, "firmware");

    int bad = -1;
    long long bad_size = 0;
    for (int i = 0; i < nd; i++) {
        char p[PIN_PATH_MAX];
        if (snprintf(p, sizeof(p), "%s" DIR_SEP BITSTREAM_NAME, dirs[i]) >= (int)sizeof(p))
            continue;
        long long n = file_size(p);
        if (n == BITSTREAM_BYTES) {
            snprintf(path, cap, "%s", p);
            return 0;
        }
        if (n >= 0 && bad < 0) {
            bad = i;
            bad_size = n;
            snprintf(path, cap, "%s", p);
        }
    }
    if (why && why_cap) {
        if (bad >= 0) {
            snprintf(why, why_cap, "FPGA bitstream %s is %lld bytes, expected %d (wrong or truncated file)", path,
                     bad_size, BITSTREAM_BYTES);
        } else {
            int len = snprintf(why, why_cap, "FPGA bitstream " BITSTREAM_NAME " not found. Looked in:");
            for (int i = 0; i < nd && len > 0 && (size_t)len < why_cap; i++)
                len += snprintf(why + len, why_cap - (size_t)len, "%s %s", i ? ";" : "", dirs[i]);
        }
    }
    return -1;
}

/* ---- the session around the link ------------------------------------------------------------ */

typedef struct {
    const pout_script_t *sc;
    pout_tape_t *tape;
    int (*interrupted)(void);
    pout_link_t *link;
    pout_session_t *session;
    pout_avc_t avc;
    pout_out_t out;             /* the IT stream as a pout_out_t; priv = this */
    int local;                  /* a local broadcast connection is wanted (the script has actions) */
    int gen;                    /* bus generation we last saw */
    double gen_checked;
    int usb_error;
    int controller;             /* node that sent the last AV/C command, -1 none */
    int tx_seen;
    char failure[160];
} hw_t;

static void fail_note(hw_t *h, const char *msg)
{
    if (!h->failure[0])
        snprintf(h->failure, sizeof(h->failure), "%s", msg);
}

/* -- the IT output -- */

static int it_start(pout_out_t *o)
{
    hw_t *h = (hw_t *)o->priv;
    unsigned ch = 63, sp = 0;
    pout_link_tx(h->link, &ch, &sp);
    return pout_link_it_start(h->link, ch, sp);
}

static int it_packet(pout_out_t *o, const uint8_t *pkt, unsigned len)
{
    hw_t *h = (hw_t *)o->priv;
    int rc = pout_link_it_packet(h->link, pkt, len);
    if (rc != 0)
        fail_note(h, "the isochronous transmit stream failed (EP 0x06 write error or timeout)");
    return rc;
}

static void it_stop(pout_out_t *o)
{
    hw_t *h = (hw_t *)o->priv;
    pout_link_it_stop(h->link);
}

static void it_destroy(pout_out_t *o) { (void)o; }

/* -- callbacks from the link -- */

static int on_avc(void *user, uint16_t src, const uint8_t *cmd, unsigned len, uint8_t *resp, unsigned cap)
{
    hw_t *h = (hw_t *)user;
    h->controller = src & 0x3f;
    if (!h->session)
        return 0;       /* a command before the deck exists: no answer, the controller retries */
    return pout_avc_handle(&h->avc, pout_session_deck(h->session), pout_session_time(h->session), cmd, len, resp,
                           cap);
}

static void on_plug(void *user, int tx_on, unsigned channel, unsigned speed)
{
    (void)user;
    static const char *const names[] = { "S100", "S200", "S400" };
    if (tx_on)
        pin_logf(PIN_LOG_INFO, "playout: a controller connected our output plug: channel %u, %s\n", channel,
                 names[speed > 2 ? 2 : speed]);
    else
        pin_logf(PIN_LOG_INFO, "playout: the output plug was disconnected\n");
}

static void on_progress(void *user, const char *step, int percent)
{
    (void)user;
    (void)percent;
    fprintf(stderr, "%s\n", step);
}

/* -- hooks -- */

static int hk_interrupted(void *u)
{
    hw_t *h = (hw_t *)u;
    return h->interrupted && h->interrupted();
}

static int hk_failed(void *u, char *msg, size_t cap)
{
    hw_t *h = (hw_t *)u;
    if (h->usb_error)
        fail_note(h, "USB error while talking to the device (unplugged?)");
    if (h->failure[0]) {
        snprintf(msg, cap, "%s", h->failure);
        return 1;
    }
    return 0;
}

/* Bus reset: the IT context is dead and the plugs are cleared. Stop the stream, re-read the topology,
 * redo the local connection; the session then sees tx off for one period and starts again. */
static int check_bus_reset(hw_t *h)
{
    double now = pout_wall_seconds();
    if (now - h->gen_checked < BUS_POLL_SECONDS)
        return 0;
    h->gen_checked = now;
    int g = pout_link_bus_generation(h->link);
    if (g < 0 || g == h->gen)
        return 0;
    pin_logf(PIN_LOG_INFO, "playout: bus reset (generation %d -> %d), restarting the stream\n", h->gen, g);
    h->gen = g;
    pout_link_it_stop(h->link);
    if (pout_link_rescan(h->link) != 0)
        h->usb_error = 1;
    else if (h->local && pout_link_local_connect(h->link, 1) < 0)
        fail_note(h, "cannot make the broadcast connection again after the bus reset");
    return 1;
}

static int hk_tx_wanted(void *u)
{
    hw_t *h = (hw_t *)u;
    if (check_bus_reset(h))
        return 0;
    return pout_link_tx(h->link, NULL, NULL);
}

static void hk_tx_changed(void *u, int on, pout_cip_t *cip)
{
    hw_t *h = (hw_t *)u;
    if (!on) {
        pin_logf(PIN_LOG_INFO, "playout: transmission stopped\n");
        return;
    }
    uint32_t cyc = 0;
    unsigned off = 0;
    pout_cip_set_sid(cip, pout_link_node(h->link) & 0x3fu);
    if (pout_link_cycle_timer(h->link, &cyc, &off) != 0)
        h->usb_error = 1;
    else
        pout_cip_set_cycle(cip, (cyc + START_LEAD_CYCLES) % POUT_CIP_CYCLES_WRAP);
    h->tx_seen = 1;
    unsigned ch = 63, sp = 0;
    pout_link_tx(h->link, &ch, &sp);
    pin_logf(PIN_LOG_INFO, "playout: transmitting on channel %u\n", ch);
}

static void hk_period_end(void *u)
{
    hw_t *h = (hw_t *)u;
    if (pout_link_it_flush(h->link) != 0)
        fail_note(h, "the isochronous transmit stream failed");
}

/* The IT queue paces a sending loop; with nothing to send, wait here for requests instead. */
static void hk_between(void *u, double deadline, int tx)
{
    hw_t *h = (hw_t *)u;
    (void)deadline;
    if (pout_link_pump(h->link, tx ? 1 : 20) < 0)
        h->usb_error = 1;
}

static void hk_status(void *u, const char *line)
{
    (void)u;
    fprintf(stderr, "%s\n", line);
    fflush(stderr);
}

static void hk_status_extra(void *u, char *buf, size_t cap)
{
    hw_t *h = (hw_t *)u;
    if (h->controller >= 0)
        snprintf(buf, cap, "controller node %d", h->controller);
    else
        snprintf(buf, cap, "%s", h->local ? "local connection" : "no controller yet");
}

int pout_hw_run(const pout_script_t *sc, pout_tape_t *tape, int (*interrupted)(void), char *err, size_t err_cap)
{
    hw_t h;
    memset(&h, 0, sizeof(h));
    h.sc = sc;
    h.tape = tape;
    h.interrupted = interrupted;
    h.controller = -1;
    h.local = sc->nactions > 0;
    h.out.start = it_start;
    h.out.packet = it_packet;
    h.out.stop = it_stop;
    h.out.destroy = it_destroy;
    h.out.priv = &h;

    int hdv = pout_tape_media(tape) == POUT_MEDIA_HDV;
    char path[PIN_PATH_MAX], why[400];
    if (pout_hw_find_bitstream(path, sizeof(path), why, sizeof(why)) != 0) {
        snprintf(err, err_cap, "%s", why);
        return POUT_EXIT_DEVICE;
    }

    pout_link_cfg_t lc;
    memset(&lc, 0, sizeof(lc));
    lc.device_id = sc->has_device ? sc->device : NULL;
    lc.bitstream_path = path;
    lc.max_packet = hdv ? 584 : 488;
    lc.speed = sc->speed >= 400 ? 2 : sc->speed >= 200 ? 1 : 0;
    lc.channel = (unsigned)sc->channel;
    lc.progress = on_progress;
    lc.plug = on_plug;
    if (sc->virtual_deck) {
        lc.avc = on_avc;
        lc.avc_user = &h;
    }
    pout_avc_init(&h.avc, hdv ? POUT_AVC_HDV : pout_tape_is_625(tape) ? POUT_AVC_DV_625 : POUT_AVC_DV_525);

    if (pout_link_open(&h.link, &lc, err, err_cap) != 0)
        return POUT_EXIT_DEVICE;
    h.gen = pout_link_bus_generation(h.link);
    h.gen_checked = pout_wall_seconds();

    int code;
    pout_session_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.tape = tape;
    cfg.sc = sc;
    cfg.out = &h.out;
    cfg.realtime = 1;
    cfg.serve = 1;
    cfg.user = &h;
    cfg.hooks.interrupted = hk_interrupted;
    cfg.hooks.failed = hk_failed;
    cfg.hooks.tx_wanted = hk_tx_wanted;
    cfg.hooks.tx_changed = hk_tx_changed;
    cfg.hooks.period_end = hk_period_end;
    cfg.hooks.between = hk_between;
    cfg.hooks.status = hk_status;
    cfg.hooks.status_extra = hk_status_extra;
    h.session = pout_session_create(&cfg, err, err_cap);
    if (!h.session) {
        pout_link_close(h.link);
        return POUT_EXIT_DEVICE;
    }

    if (h.local && pout_link_local_connect(h.link, 1) < 0) {
        snprintf(err, err_cap, "cannot make the broadcast connection on channel %d (is another node using it, or "
                 "no bus manager?)", sc->channel);
        code = POUT_EXIT_DEVICE;
    } else {
        code = pout_session_run(h.session, err, err_cap);
    }
    pin_logf(PIN_LOG_INFO, "playout: %lu frames sent\n", pout_session_frames_sent(h.session));
    pout_session_destroy(h.session);
    h.session = NULL;
    pout_link_close(h.link);
    return code;
}
