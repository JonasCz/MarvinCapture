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

#include "pout_hw_link.h"

#include "pout_it.h"
#include "pin_log.h"
#include "pinnacle_1394.h"
#include "pinnacle_device.h"
#include "pinnacle_enum.h"
#include "pinnacle_lock.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
static unsigned long long now_ms(void) { return GetTickCount64(); }
static void sleep_ms(unsigned ms) { Sleep(ms); }
#else
#include <time.h>
static unsigned long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000u + (unsigned long long)ts.tv_nsec / 1000000u;
}
static void sleep_ms(unsigned ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
#endif

/* OHCI registers used here */
#define OHCI_CSR_DATA      0x00c
#define OHCI_CSR_COMPARE   0x010
#define OHCI_CSR_CONTROL   0x014
#define OHCI_CYCLE_TIMER   0x0f0

/* IRM registers (offsets within 0xFFFF_xxxx_xxxx) */
#define IRM_BANDWIDTH_AVAILABLE 0xf0000220u
#define IRM_CHANNELS_HI         0xf0000224u   /* channels 0..31, channel 0 is bit 31 */
#define IRM_CHANNELS_LO         0xf0000228u   /* channels 32..63 */

/* the EP 0x06 queue: the vendor sends about 30 KB per transfer (FUN_0003efd0 starts a new transfer
 * once the running one is over 30000 bytes) and keeps several in flight (MaxPending1394IsoWrite) */
#define IT_CHUNK_BYTES 32768u
#define IT_CHUNK_SOFT  30000u
#define IT_CHUNKS      6u
#define IT_WRITE_TIMEOUT_MS 3000u

#define REQ_QUEUE 16u
#define REQ_MAX   640u

struct req_slot { uint8_t rec[REQ_MAX]; unsigned len, ack; };

struct chunk { uint8_t data[IT_CHUNK_BYTES]; unsigned len; };

struct pout_link {
    pout_link_cfg_t cfg;
    pinnacle_device_t dev;
    pinnacle_lock_t *lock;
    int dev_open;
    pinnacle_1394_t l;
    pout_csr_t csr;
    uint16_t irm;              /* node ID of the IRM we found, 0 = not looked yet */

    /* local connection resources */
    int have_res;
    unsigned res_channel, res_bw;

    /* requests copied by the hook, answered by pout_link_pump() */
    struct req_slot rq[REQ_QUEUE];
    unsigned rq_head, rq_tail;   /* tail - head = queued */
    unsigned rq_dropped;

    /* EP 0x06 */
    pout_it_t it;
    int it_running;
    pthread_t it_thread;
    int it_thread_started;
    pthread_mutex_t it_mtx;
    pthread_cond_t it_cv;
    struct chunk *chunks;                       /* IT_CHUNKS buffers */
    unsigned q[IT_CHUNKS], q_head, q_count;     /* indexes of chunks waiting for the sender */
    unsigned free_list[IT_CHUNKS], n_free;
    int cur;                                    /* chunk being filled, -1 = none */
    int sending;                                /* the sender holds a chunk */
    int it_stop_thread;
    int it_error;
    unsigned long long it_bytes;
};

/* --- the request hook ----------------------------------------------------- */

/* Called from inside p1394_pump(). It must not talk to the device (p1394_submit would pump
 * again from within the parse), so it only copies the record; pout_link_pump() answers. */
static int on_request(void *user, const uint8_t *rec, unsigned len, unsigned ack)
{
    pout_link_t *k = (pout_link_t *)user;
    if (k->rq_tail - k->rq_head >= REQ_QUEUE || len > REQ_MAX) {
        k->rq_dropped++;
        pin_logf(PIN_LOG_WARN, "playout: request queue full or oversized (%u bytes), dropped\n", len);
        return 1;   /* GUESS: better dropped (the sender retries) than answered wrongly by the core */
    }
    struct req_slot *s = &k->rq[k->rq_tail % REQ_QUEUE];
    memcpy(s->rec, rec, len);
    s->len = len;
    s->ack = ack;
    k->rq_tail++;
    return 1;
}

static unsigned next_tl(pinnacle_1394_t *l)
{
    unsigned tl = l->next_tl;               /* the rotation req_q0() in pinnacle_1394.c uses */
    l->next_tl = l->next_tl >= 63 ? 2 : l->next_tl + 1;
    return tl;
}

static int submit(pout_link_t *k, const pout_resp_t *r)
{
    return p1394_submit(&k->l, r->hdr, r->hdr_len, r->plen ? r->payload : NULL, r->plen);
}

int pout_link_send_fcp(pout_link_t *k, uint16_t dest, const uint8_t *frame, unsigned len)
{
    pout_resp_t r;
    if (pout_csr_fcp_write(&r, next_tl(&k->l), dest, frame, len) != 0)
        return -1;
    int evt = submit(k, &r);
    /* the controller acks a write with pending (and answers with a write response later, which the
     * link logs and ignores) or complete */
    return (evt == P1394_EVT_ACK_PENDING || evt == P1394_EVT_ACK_COMPLETE) ? 0 : -1;
}

static void dispatch_one(pout_link_t *k, struct req_slot *s)
{
    pout_req_t req;
    pout_resp_t resp;
    pout_csr_event_t ev;
    if (pout_csr_parse(s->rec, s->len, &req) != 0) {
        pin_logf(PIN_LOG_DEBUG, "playout: unparsable request (%u bytes)\n", s->len);
        return;
    }
    req.ack = s->ack;
    int owed = pout_csr_handle(&k->csr, &req, &resp, &ev);
    if (owed) {
        int evt = submit(k, &resp);
        pin_logf(PIN_LOG_DEBUG, "playout: response tcode %u rcode %u to %04x -> event 0x%02x\n",
                 (resp.hdr[0] >> 4) & 0xf, (resp.hdr[1] >> 12) & 0xf, req.src, evt);
    }
    if (ev.plug_changed && k->cfg.plug) {
        unsigned ch, sp;
        int on = pout_csr_tx(&k->csr, &ch, &sp);
        k->cfg.plug(k->cfg.plug_user, on, ch, sp);
    }
    if (ev.fcp && k->cfg.avc) {
        uint8_t out[POUT_FCP_MAX];
        int n = k->cfg.avc(k->cfg.avc_user, ev.fcp_src, ev.fcp, ev.fcp_len, out, sizeof(out));
        if (n > 0) {
            /* GUESS: the response write goes out after the write response above, to the node that
             * sent the command. A controller that sent the command as a quadlet write and wants the
             * answer within a few ms is served by this ordering; slower ones by the 100 ms timeout. */
            if (pout_link_send_fcp(k, ev.fcp_src, out, (unsigned)n) != 0)
                pin_logf(PIN_LOG_DEBUG, "playout: AV/C response to %04x not acked\n", ev.fcp_src);
        }
    }
}

int pout_link_pump(pout_link_t *k, unsigned timeout_ms)
{
    int handled = 0;
    if (p1394_pump(&k->l, timeout_ms) < 0)
        return -1;
    for (;;) {
        while (k->rq_head != k->rq_tail) {
            /* the slot stays queued while it is processed: submit() pumps and may add more */
            struct req_slot *s = &k->rq[k->rq_head % REQ_QUEUE];
            dispatch_one(k, s);
            k->rq_head++;
            handled++;
        }
        if (k->l.n_owed == 0)
            break;
        p1394_answer_owed(&k->l);
        if (k->rq_head == k->rq_tail)
            break;
    }
    return handled;
}

int pout_link_tx(const pout_link_t *k, unsigned *channel, unsigned *speed)
{
    return pout_csr_tx(&k->csr, channel, speed);
}

uint16_t pout_link_node(const pout_link_t *k) { return k->l.local_node; }
int pout_link_node_count(const pout_link_t *k) { return k->l.node_count; }

int pout_link_bus_generation(pout_link_t *k)
{
    uint32_t v;
    if (p1394_reg_read(&k->l, P1394_OHCI_SELF_ID_COUNT, &v) != 0)
        return -1;
    return (int)((v >> 16) & 0xff);
}

int pout_link_rescan(pout_link_t *k)
{
    k->irm = 0;
    return p1394_read_topology(&k->l);
}

/* --- open / close ----------------------------------------------------------- */

static void fail(char *err, size_t cap, const char *what, const char *detail)
{
    if (err && cap)
        snprintf(err, cap, "%s%s%s", what, detail && detail[0] ? ": " : "", detail ? detail : "");
}

static void progress(pout_link_t *k, const char *step)
{
    if (k->cfg.progress)
        k->cfg.progress(k->cfg.progress_user, step, -1);
}

static void teardown(pout_link_t *k)
{
    if (k->dev_open)
        pinnacle_close(&k->dev);
    k->dev_open = 0;
    if (k->lock)
        pinnacle_lock_release(k->lock);
    k->lock = NULL;
}

static int bring_up_link(pout_link_t *k)
{
    p1394_init(&k->l, &k->dev);
    k->l.rom_avc_unit = 1;
    p1394_set_request_handler(&k->l, on_request, k);
    progress(k, "Starting the FireWire (1394) link");
    return p1394_link_init(&k->l);
}

int pout_link_open(pout_link_t **out, const pout_link_cfg_t *cfg, char *err, size_t err_cap)
{
    pout_link_t *k = calloc(1, sizeof(*k));
    if (!k) {
        fail(err, err_cap, "out of memory", NULL);
        return -1;
    }
    k->cfg = *cfg;
    k->cur = -1;
    if (k->cfg.max_packet < 8)
        k->cfg.max_packet = 488;

    char id[PINNACLE_ENUM_ID_MAX];
    if (cfg->device_id && cfg->device_id[0]) {
        snprintf(id, sizeof(id), "%s", cfg->device_id);
    } else {
        pinnacle_enum_entry_t e[4];
        int n = pinnacle_enumerate(e, 4);
        if (n < 1) {
            fail(err, err_cap, "no Pinnacle Marvin device found", NULL);
            free(k);
            return -1;
        }
        snprintf(id, sizeof(id), "%s", e[0].id);
    }

    pinnacle_status_t st = pinnacle_lock_acquire(id, &k->lock);
    if (st != PINNACLE_OK) {
        fail(err, err_cap, "cannot take the device lock", pinnacle_strerror(st));
        free(k);
        return -1;
    }
    st = pinnacle_open_by_id(&k->dev, id);
    if (st != PINNACLE_OK) {
        fail(err, err_cap, "cannot open the device", pinnacle_strerror(st));
        pinnacle_lock_release(k->lock);
        free(k);
        return -1;
    }
    k->dev_open = 1;
    k->dev.progress = cfg->progress;
    k->dev.progress_user = cfg->progress_user;

    /* the same bring-up as pinnacle_stream_start() (FX2 firmware, FPGA design, link init), minus
     * the camera search and the receive context */
    st = pinnacle_init_hardware(&k->dev, cfg->bitstream_path);
    if (st != PINNACLE_OK) {
        fail(err, err_cap, "cannot start the device", pinnacle_strerror(st));
        teardown(k);
        free(k);
        return -1;
    }
    pinnacle_lock_update(k->lock, PINNACLE_LOCK_READY, k->dev.guid_hi, k->dev.guid_lo);

    int ok = bring_up_link(k) == 0;
    if (!ok && k->dev.warm_dv) {
        pin_logf(PIN_LOG_WARN, "playout: link init failed on the warm start, doing the full cold start\n");
        st = pinnacle_init_hardware_cold(&k->dev, k->dev.bitstream_path);
        if (st == PINNACLE_OK)
            ok = bring_up_link(k) == 0;
    }
    if (!ok) {
        fail(err, err_cap, "FireWire link init failed",
             k->l.step ? k->l.step : "check the cable and that the device is powered");
        teardown(k);
        free(k);
        return -1;
    }
    pin_logf(PIN_LOG_INFO, "playout: 1394 bus has %d node(s), we are node %u\n", k->l.node_count,
             k->l.local_node & 0x3f);

    pout_csr_cfg_t pc;
    pout_csr_cfg_default(&pc, k->cfg.max_packet);
    pout_csr_init(&k->csr, &pc);

    pthread_mutex_init(&k->it_mtx, NULL);
    pthread_cond_init(&k->it_cv, NULL);
    *out = k;
    return 0;
}

static void it_thread_stop(pout_link_t *k);

void pout_link_close(pout_link_t *k)
{
    if (!k)
        return;
    pout_link_it_stop(k);
    if (k->have_res)
        pout_link_irm_release(k, k->res_channel, k->res_bw);
    k->have_res = 0;
    it_thread_stop(k);
    pthread_mutex_destroy(&k->it_mtx);
    pthread_cond_destroy(&k->it_cv);
    free(k->chunks);
    teardown(k);
    free(k);
}

/* --- IRM -------------------------------------------------------------------------- */

/* Compare-swap on an IRM register: through the OHCI CSR registers if the IRM is us (selector 1
 * BANDWIDTH_AVAILABLE, 2 CHANNELS_AVAILABLE_HI, 3 _LO; FUN_0002e940: CSRData = new,
 * CSRCompareData = expected, CSRControl = selector, poll done in bit 31, read CSRData = old),
 * else a lock transaction (FUN_0003fa50). */
static int irm_cas(pout_link_t *k, uint16_t irm, uint32_t off, uint32_t arg, uint32_t data, uint32_t *old)
{
    if (irm != k->l.local_node)
        return p1394_compare_swap(&k->l, irm, off, arg, data, old);
    unsigned sel = off == IRM_BANDWIDTH_AVAILABLE ? 1 : off == IRM_CHANNELS_HI ? 2 : 3;
    uint32_t ctl = 0;
    if (p1394_reg_write(&k->l, OHCI_CSR_DATA, data) != 0 ||
        p1394_reg_write(&k->l, OHCI_CSR_COMPARE, arg) != 0 ||
        p1394_reg_write(&k->l, OHCI_CSR_CONTROL, sel) != 0)
        return -1;
    for (int i = 0; i < 20; i++) {
        if (p1394_reg_read(&k->l, OHCI_CSR_CONTROL, &ctl) == 0 && (ctl & 0x80000000u))
            return p1394_reg_read(&k->l, OHCI_CSR_DATA, old);
        sleep_ms(2);
    }
    return -1;
}

/* A read is a compare-swap that cannot change anything useful: compare with 0, write 0 back. */
static int irm_read(pout_link_t *k, uint16_t irm, uint32_t off, uint32_t *v)
{
    if (irm != k->l.local_node)
        return p1394_read_quadlet(&k->l, irm, off, v);
    return irm_cas(k, irm, off, 0, 0, v);
}

/* GUESS: the IRM is the highest-numbered node that answers a read of CHANNELS_AVAILABLE (every
 * node that is a contender and has its link on is a candidate and the highest wins; the camcorders
 * seen so far are contenders). We are always a candidate (the link init sets the contender bit). */
static int find_irm(pout_link_t *k)
{
    if (k->irm)
        return 0;
    for (int n = k->l.node_count - 1; n >= 0; n--) {
        uint16_t id = (uint16_t)(0xffc0u | (unsigned)n);
        uint32_t v;
        if (irm_read(k, id, IRM_CHANNELS_HI, &v) == 0) {
            k->irm = id;
            pin_logf(PIN_LOG_DEBUG, "playout: IRM is node %u%s\n", n, id == k->l.local_node ? " (us)" : "");
            return 0;
        }
    }
    return -1;
}

unsigned pout_link_bandwidth_units(unsigned max_packet, unsigned speed)
{
    /* GUESS (IEC 61883-1 / libiec61883): (payload quadlets + 3 for the isochronous header, its CRC
     * and the data CRC) at 16 >> speed units a quadlet, plus 512 units of gap overhead for overhead
     * ID 0. A DV packet at S400: (122 + 3) * 4 + 512 = 1012. */
    return (max_packet / 4 + 3) * (16u >> (speed > 2 ? 2 : speed)) + 512u;
}

int pout_link_irm_alloc(pout_link_t *k, unsigned bw)
{
    if (find_irm(k) != 0)
        return -1;
    int channel = -1;

    for (int attempt = 0; attempt < 4 && channel < 0; attempt++) {
        uint32_t hi, lo;
        if (irm_read(k, k->irm, IRM_CHANNELS_HI, &hi) != 0 || irm_read(k, k->irm, IRM_CHANNELS_LO, &lo) != 0)
            return -1;
        int ch = -1;
        for (int c = 0; c < 63 && ch < 0; c++) {
            uint32_t w = c < 32 ? hi : lo;
            if (w & (0x80000000u >> (c & 31)))
                ch = c;
        }
        if (ch < 0)
            return -1;
        uint32_t off = ch < 32 ? IRM_CHANNELS_HI : IRM_CHANNELS_LO, cur = ch < 32 ? hi : lo, old = 0;
        if (irm_cas(k, k->irm, off, cur, cur & ~(0x80000000u >> (ch & 31)), &old) != 0)
            return -1;
        if (old == cur)
            channel = ch;                /* else somebody took one meanwhile: look again */
    }
    if (channel < 0)
        return -1;

    int got_bw = 0;
    for (int attempt = 0; attempt < 4 && !got_bw; attempt++) {
        uint32_t cur, old = 0;
        if (irm_read(k, k->irm, IRM_BANDWIDTH_AVAILABLE, &cur) != 0 || cur < bw)
            break;
        if (irm_cas(k, k->irm, IRM_BANDWIDTH_AVAILABLE, cur, cur - bw, &old) != 0)
            break;
        got_bw = old == cur;
    }
    if (!got_bw) {
        pout_link_irm_release(k, (unsigned)channel, 0);
        return -1;
    }
    pin_logf(PIN_LOG_DEBUG, "playout: IRM: channel %d, %u bandwidth units\n", channel, bw);
    return channel;
}

void pout_link_irm_release(pout_link_t *k, unsigned channel, unsigned bw)
{
    if (!k->irm)
        return;
    for (int attempt = 0; attempt < 4; attempt++) {
        uint32_t off = channel < 32 ? IRM_CHANNELS_HI : IRM_CHANNELS_LO, cur, old = 0;
        if (irm_read(k, k->irm, off, &cur) != 0)
            break;
        if (irm_cas(k, k->irm, off, cur, cur | (0x80000000u >> (channel & 31)), &old) == 0 && old == cur)
            break;
    }
    for (int attempt = 0; bw && attempt < 4; attempt++) {
        uint32_t cur, old = 0;
        if (irm_read(k, k->irm, IRM_BANDWIDTH_AVAILABLE, &cur) != 0)
            break;
        if (irm_cas(k, k->irm, IRM_BANDWIDTH_AVAILABLE, cur, cur + bw, &old) == 0 && old == cur)
            break;
    }
}

int pout_link_local_connect(pout_link_t *k, int on)
{
    if (on) {
        if (k->have_res)
            return (int)k->res_channel;
        unsigned sp = (k->csr.opcr & POUT_PCR_RATE_MASK) >> POUT_PCR_RATE_SHIFT;
        unsigned bw = pout_link_bandwidth_units(k->cfg.max_packet, sp);
        int ch = pout_link_irm_alloc(k, bw);
        if (ch < 0)
            return -1;
        k->have_res = 1;
        k->res_channel = (unsigned)ch;
        k->res_bw = bw;
        pout_csr_local_connect(&k->csr, (unsigned)ch, 1);
        return ch;
    }
    pout_csr_local_connect(&k->csr, 0, 0);
    if (k->have_res)
        pout_link_irm_release(k, k->res_channel, k->res_bw);
    k->have_res = 0;
    return 0;
}

/* --- cycle timer -------------------------------------------------------------------- */

int pout_link_cycle_timer(pout_link_t *k, uint32_t *cycle, unsigned *offset)
{
    uint32_t v;
    if (p1394_reg_read(&k->l, OHCI_CYCLE_TIMER, &v) != 0)
        return -1;
    *cycle = (v >> 25) * 8000u + ((v >> 12) & 0x1fff);
    if (offset)
        *offset = v & 0xfff;
    return 0;
}

/* --- EP 0x06 -------------------------------------------------------------------------- */

/* GUESS: a blocking bulk write of each chunk on its own thread keeps the pipe busy enough when
 * the queue never runs dry (the vendor has several transfers pending at the driver level, we have
 * IT_CHUNKS - 1 queued behind the one in flight, which leaves a gap of microseconds between
 * transfers instead of none). If the FPGA's transmit FIFO underruns in those gaps, move this to
 * libusb asynchronous transfers (pinnacle_stream.c's read loop is the pattern). */
static void *it_sender(void *arg)
{
    pout_link_t *k = (pout_link_t *)arg;
    pthread_mutex_lock(&k->it_mtx);
    for (;;) {
        while (k->q_count == 0 && !k->it_stop_thread)
            pthread_cond_wait(&k->it_cv, &k->it_mtx);
        if (k->q_count == 0 && k->it_stop_thread)
            break;
        unsigned idx = k->q[k->q_head % IT_CHUNKS];
        k->q_head++;
        k->q_count--;
        k->sending = 1;
        pthread_mutex_unlock(&k->it_mtx);

        struct chunk *c = &k->chunks[idx];
        int n = k->it_error ? -1 : p1394_send_it(&k->l, c->data, (int)c->len, IT_WRITE_TIMEOUT_MS);

        pthread_mutex_lock(&k->it_mtx);
        if (n != (int)c->len && !k->it_error) {
            k->it_error = 1;
            pin_logf(PIN_LOG_ERROR, "playout: EP 0x06 write failed (%d of %u bytes, usb %d)\n", n, c->len,
                     k->l.last_usb_rc);
        } else {
            k->it_bytes += c->len;
        }
        c->len = 0;
        k->free_list[k->n_free++] = idx;
        k->sending = 0;
        pthread_cond_broadcast(&k->it_cv);
    }
    pthread_mutex_unlock(&k->it_mtx);
    return NULL;
}

static void it_thread_stop(pout_link_t *k)
{
    if (!k->it_thread_started)
        return;
    pthread_mutex_lock(&k->it_mtx);
    k->it_stop_thread = 1;
    pthread_cond_broadcast(&k->it_cv);
    pthread_mutex_unlock(&k->it_mtx);
    pthread_join(k->it_thread, NULL);
    k->it_thread_started = 0;
}

/* a free chunk, waiting up to ~2 s; -1 on timeout or after a write error. it_mtx held. */
static int take_free_locked(pout_link_t *k)
{
    unsigned long long deadline = now_ms() + 2000;
    while (k->n_free == 0 && !k->it_error) {
        unsigned long long now = now_ms();
        if (now >= deadline)
            return -1;
        /* the condition variable has no portable timed wait here without absolute times; poll */
        pthread_mutex_unlock(&k->it_mtx);
        sleep_ms(1);
        pthread_mutex_lock(&k->it_mtx);
    }
    if (k->it_error)
        return -1;
    return (int)k->free_list[--k->n_free];
}

static void submit_cur_locked(pout_link_t *k)
{
    if (k->cur < 0)
        return;
    if (k->chunks[k->cur].len == 0) {
        k->free_list[k->n_free++] = (unsigned)k->cur;
    } else {
        k->q[(k->q_head + k->q_count) % IT_CHUNKS] = (unsigned)k->cur;
        k->q_count++;
        pthread_cond_broadcast(&k->it_cv);
    }
    k->cur = -1;
}

int pout_link_it_start(pout_link_t *k, unsigned channel, unsigned speed)
{
    if (k->it_running)
        return 0;
    if (!k->chunks) {
        k->chunks = calloc(IT_CHUNKS, sizeof(struct chunk));
        if (!k->chunks)
            return -1;
    }
    /* GUESS: nothing else is needed before the first descriptor: the vendor only sets up host
     * state in FUN_0002d7b0. Clear run in case an earlier run left the context going. */
    if (p1394_reg_write(&k->l, POUT_IT_REG_CTRL_CLR, POUT_IT_CTX_RUN) != 0)
        return -1;
    pout_it_init(&k->it, channel, speed, k->cfg.max_packet);
    if (pout_it_ram_end(&k->it) > 0x2000)
        pin_logf(PIN_LOG_DEBUG, "playout: the IT ring reaches RAM 0x%x, past the self-ID buffer at 0x2000\n",
                 pout_it_ram_end(&k->it));

    pthread_mutex_lock(&k->it_mtx);
    k->q_head = k->q_count = 0;
    k->n_free = 0;
    for (unsigned i = 0; i < IT_CHUNKS; i++) {
        k->chunks[i].len = 0;
        k->free_list[k->n_free++] = i;
    }
    k->cur = -1;
    k->it_error = 0;
    k->it_stop_thread = 0;
    k->it_bytes = 0;
    pthread_mutex_unlock(&k->it_mtx);
    if (!k->it_thread_started) {
        if (pthread_create(&k->it_thread, NULL, it_sender, k) != 0)
            return -1;
        k->it_thread_started = 1;
    }
    k->it_running = 1;
    return 0;
}

int pout_link_it_packet(pout_link_t *k, const uint8_t *pkt, unsigned len)
{
    if (!k->it_running)
        return -1;
    size_t need = pout_it_max_bytes_per_packet(&k->it);
    int rc = -1;
    pthread_mutex_lock(&k->it_mtx);
    if (k->it_error)
        goto out;
    if (k->cur < 0) {
        k->cur = take_free_locked(k);
        if (k->cur < 0)
            goto out;
    }
    struct chunk *c = &k->chunks[k->cur];
    size_t n = pout_it_packet(&k->it, pkt, len, c->data + c->len, IT_CHUNK_BYTES - c->len);
    if (n == 0) {
        /* either malformed or the chunk is full: if it is full, hand it over and retry once */
        if (IT_CHUNK_BYTES - c->len >= need)
            goto out;               /* malformed packet */
        submit_cur_locked(k);
        k->cur = take_free_locked(k);
        if (k->cur < 0)
            goto out;
        c = &k->chunks[k->cur];
        n = pout_it_packet(&k->it, pkt, len, c->data, IT_CHUNK_BYTES);
        if (n == 0)
            goto out;
    }
    c->len += (unsigned)n;
    if (c->len > IT_CHUNK_SOFT)
        submit_cur_locked(k);
    rc = 0;
out:
    pthread_mutex_unlock(&k->it_mtx);
    return rc;
}

int pout_link_it_flush(pout_link_t *k)
{
    pthread_mutex_lock(&k->it_mtx);
    submit_cur_locked(k);
    int err = k->it_error;
    pthread_mutex_unlock(&k->it_mtx);
    return err ? -1 : 0;
}

int pout_link_it_running(const pout_link_t *k) { return k->it_running; }

int pout_link_it_stop(pout_link_t *k)
{
    if (!k->it_running)
        return 0;
    pout_link_it_flush(k);
    /* let the queue drain; GUESS: if the FPGA sits in a type-11 wait that never ends, this times out */
    unsigned long long deadline = now_ms() + 3000;
    pthread_mutex_lock(&k->it_mtx);
    while ((k->q_count || k->sending) && !k->it_error && now_ms() < deadline) {
        pthread_mutex_unlock(&k->it_mtx);
        sleep_ms(2);
        pthread_mutex_lock(&k->it_mtx);
    }
    pthread_mutex_unlock(&k->it_mtx);

    uint8_t msg[8];
    int rc = 0;
    if (pout_it_stop(&k->it, msg, sizeof(msg)) == 8 && p1394_send(&k->l, msg, 8) != 0)
        rc = -1;
    /* FUN_0002e450 re-clears run and polls the control register until active and run are both 0 */
    for (int i = 0; i < 20; i++) {
        uint32_t ctl = 0;
        if (p1394_reg_read(&k->l, POUT_IT_REG_CTRL_SET, &ctl) == 0 &&
            !(ctl & (POUT_IT_CTX_ACTIVE | POUT_IT_CTX_RUN)))
            break;
        p1394_reg_write(&k->l, POUT_IT_REG_CTRL_CLR, POUT_IT_CTX_RUN);
        sleep_ms(10);
    }
    k->it_running = 0;
    return rc;
}
