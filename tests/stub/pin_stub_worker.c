/*
 * pin_stub_worker.c — the session's worker thread: drives the PREPARING
 * timer, the DV/HDV alternation, and ticks the deck + capture simulations.
 * Runs at a fixed ~50ms period (20Hz), which is more than enough for a
 * GUI polling status at ~10Hz to see smooth progress.
 */
#include "pin_stub.h"
#include <string.h>
#include <stdlib.h>

#define WORKER_TICK_MS 50

static void apply_ready_geometry(pin_session_t *s) {
    /* caller holds s->lock */
    pin_kind_t kind;
    if (s->input == PIN_INPUT_DV) {
        int want_hdv = s->hdv_locked;
        if (!want_hdv && s->device_index == 0) {
            /* usb:1-4 alternates DV/HDV every 30s of wall clock */
            double now = pin_stub_now();
            if (now >= s->hdv_toggle_at) {
                s->is_dv_alternate_hdv = !s->is_dv_alternate_hdv;
                s->hdv_toggle_at = now + 30.0;
            }
            want_hdv = s->is_dv_alternate_hdv;
        }
        kind = want_hdv ? PIN_KIND_HDV : PIN_KIND_DV;
    } else {
        kind = PIN_KIND_ANALOG;
    }
    s->stream_kind = kind;
    pin_matrix_t matrix;
    int cx, cy;
    pin_stub_kind_geometry(kind, s->is_60hz, s->aspect, &s->width, &s->height,
                            &s->dar_num, &s->dar_den, &cx, &cy, &matrix);
    s->signal = 1;
}

void *pin_stub_worker_main(void *arg) {
    pin_session_t *s = (pin_session_t *)arg;

    for (;;) {
        pthread_mutex_lock(&s->lock);
        if (s->closing) { pthread_mutex_unlock(&s->lock); break; }

        double now = pin_stub_now();

        if (s->prepare_pending && now >= s->prepare_until) {
            s->prepare_pending = 0;
            apply_ready_geometry(s);
            pin_stub_set_state(s, PIN_STATE_READY);
        } else if (s->state == PIN_STATE_READY && s->input == PIN_INPUT_DV) {
            /* keep the DV/HDV alternation flowing even without a fresh
             * prepare, and keep geometry current for aspect overrides */
            apply_ready_geometry(s);
        }

        pthread_mutex_unlock(&s->lock);

        pin_stub_deck_tick(s, now);
        pin_stub_capture_tick(s, now);

        pin_stub_sleep_ms(WORKER_TICK_MS);
    }
    return NULL;
}

void pin_set_aspect(pin_session_t *s, pin_aspect_t aspect) {
    if (!s) return;
    pthread_mutex_lock(&s->lock);
    s->aspect = aspect;
    if (s->state == PIN_STATE_READY || s->state == PIN_STATE_CAPTURING) {
        apply_ready_geometry(s);
    }
    pthread_mutex_unlock(&s->lock);
}
