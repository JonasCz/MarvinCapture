/* pin_stub_util.c — time helpers, event queue, fake device table. */
#include "pin_stub.h"
#include <string.h>
#include <stdlib.h>

double pin_stub_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void pin_stub_sleep_ms(int ms) {
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* ---- event queue --------------------------------------------------------- */

void pin_evtq_init(pin_evtq_t *q) {
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->lock, NULL);
}

void pin_evtq_destroy(pin_evtq_t *q) {
    pthread_mutex_destroy(&q->lock);
}

/* index of the oldest LOG event, or -1. Caller holds q->lock. */
static int find_oldest_log(pin_evtq_t *q) {
    for (int i = 0; i < q->count; i++) {
        int idx = (q->head + i) % PIN_STUB_EVT_CAP;
        if (q->items[idx].kind == PIN_EVT_LOG) return idx;
    }
    return -1;
}

void pin_evtq_push(pin_evtq_t *q, pin_event_kind_t kind, int32_t a, const char *text) {
    pthread_mutex_lock(&q->lock);
    if (q->count >= PIN_STUB_EVT_CAP) {
        int drop = find_oldest_log(q);
        if (drop < 0) {
            /* drop oldest of any kind */
            q->head = (q->head + 1) % PIN_STUB_EVT_CAP;
            q->count--;
        } else {
            /* remove the item at `drop` by shifting the block before it
             * forward by one (small array, this is cheap enough) */
            int from = drop;
            int to = (drop + PIN_STUB_EVT_CAP - 1) % PIN_STUB_EVT_CAP;
            while (from != q->head) {
                q->items[from] = q->items[to];
                from = to;
                to = (to + PIN_STUB_EVT_CAP - 1) % PIN_STUB_EVT_CAP;
            }
            q->head = (q->head + 1) % PIN_STUB_EVT_CAP;
            q->count--;
        }
    }
    int tail = (q->head + q->count) % PIN_STUB_EVT_CAP;
    pin_event_t *ev = &q->items[tail];
    memset(ev, 0, sizeof(*ev));
    ev->size = sizeof(*ev);
    ev->kind = kind;
    ev->a = a;
    if (text) {
        strncpy(ev->text, text, sizeof(ev->text) - 1);
    }
    q->count++;
    pthread_mutex_unlock(&q->lock);
}

int pin_evtq_pop(pin_evtq_t *q, pin_event_t *out) {
    pthread_mutex_lock(&q->lock);
    if (q->count == 0) {
        pthread_mutex_unlock(&q->lock);
        return 0;
    }
    *out = q->items[q->head];
    q->head = (q->head + 1) % PIN_STUB_EVT_CAP;
    q->count--;
    pthread_mutex_unlock(&q->lock);
    return 1;
}

/* ---- shared geometry ------------------------------------------------------- */

void pin_stub_kind_geometry(pin_kind_t kind, int is_60hz, pin_aspect_t aspect_override,
                             int *w, int *h, int *dar_num, int *dar_den,
                             int *chroma_shift_x, int *chroma_shift_y, pin_matrix_t *matrix) {
    switch (kind) {
    case PIN_KIND_HDV:
        *w = 1440; *h = 1080;
        *chroma_shift_x = 1; *chroma_shift_y = 1; /* 4:2:0 */
        *matrix = PIN_MATRIX_BT709;
        *dar_num = 16; *dar_den = 9; /* HDV is 16:9 regardless of override per pin_api.h PIN_ASPECT_AUTO doc */
        break;
    case PIN_KIND_DV:
    case PIN_KIND_ANALOG:
    default:
        *w = 720; *h = is_60hz ? 480 : 576;
        *chroma_shift_x = 1; *chroma_shift_y = 0; /* 4:2:2 */
        *matrix = PIN_MATRIX_BT601;
        if (aspect_override == PIN_ASPECT_16_9) { *dar_num = 16; *dar_den = 9; }
        else { *dar_num = 4; *dar_den = 3; }
        break;
    }
    if (kind != PIN_KIND_HDV) {
        if (aspect_override == PIN_ASPECT_4_3) { *dar_num = 4; *dar_den = 3; }
        else if (aspect_override == PIN_ASPECT_16_9) { *dar_num = 16; *dar_den = 9; }
    }
}

/* ---- fake device table ----------------------------------------------------- */

pin_stub_device_t g_pin_stub_devices[PIN_STUB_DEVICE_COUNT];
pthread_mutex_t g_pin_stub_devices_lock = PTHREAD_MUTEX_INITIALIZER;

void pin_stub_devices_init(void) {
    static int done = 0;
    if (done) return;
    done = 1;

    memset(g_pin_stub_devices, 0, sizeof(g_pin_stub_devices));

    strncpy(g_pin_stub_devices[0].id, "usb:1-4", sizeof(g_pin_stub_devices[0].id) - 1);
    strncpy(g_pin_stub_devices[0].name, "Pinnacle Studio 500-USB", sizeof(g_pin_stub_devices[0].name) - 1);
    g_pin_stub_devices[0].vid = 0x2304;
    g_pin_stub_devices[0].pid = 0x0213;
    g_pin_stub_devices[0].base_state = PIN_DEV_READY;
    g_pin_stub_devices[0].owner_pid = 0;
    /* fixed fake 1394 GUIDs so the GUI can show a short stable id */
    strncpy(g_pin_stub_devices[0].serial, "0800460104A1B2C3", sizeof(g_pin_stub_devices[0].serial) - 1);

    strncpy(g_pin_stub_devices[1].id, "usb:2-1", sizeof(g_pin_stub_devices[1].id) - 1);
    strncpy(g_pin_stub_devices[1].name, "Pinnacle Studio 500-USB", sizeof(g_pin_stub_devices[1].name) - 1);
    g_pin_stub_devices[1].vid = 0x2304;
    g_pin_stub_devices[1].pid = 0x0213;
    g_pin_stub_devices[1].base_state = PIN_DEV_IN_USE;
    g_pin_stub_devices[1].owner_pid = 1234;
    strncpy(g_pin_stub_devices[1].serial, "080046010477F00D", sizeof(g_pin_stub_devices[1].serial) - 1);
}
