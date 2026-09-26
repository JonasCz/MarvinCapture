/*
 * pin_stub_deck.c — fake deck (transport) state machine.
 *
 * Latency: commands take a believable <300ms to "land" (busy flag set,
 * cleared after a short delay), then the new state applies and a
 * PIN_EVT_DECK event fires. Tape percent and timecode advance while
 * playing/FF, and reverse while rewinding; timecode never goes negative
 * (clamped to 0, and prolonged rewind past 0 implies NO_TAPE -> STOPPED,
 * matching "no data" hardware behaviour instead of hanging forever).
 */
#include "pin_stub.h"
#include <string.h>
#include <stdlib.h>

#define TAPE_TOTAL_FRAMES ((long long)(60 * 60 * 25)) /* a 60-minute tape at 25fps */
#define DECK_LATENCY_S 0.22

pin_status_t pin_deck(pin_session_t *s, pin_deck_cmd_t cmd) {
    if (!s) return PIN_ERR_ARG;
    pthread_mutex_lock(&s->lock);
    if (s->state == PIN_STATE_CAPTURING && cmd != PIN_DECK_CMD_STOP) {
        pthread_mutex_unlock(&s->lock);
        return PIN_ERR_STATE;
    }
    if (s->input != PIN_INPUT_DV) {
        pthread_mutex_unlock(&s->lock);
        return PIN_ERR_STATE; /* deck control only applies to DV/HDV */
    }

    double now = pin_stub_now();
    pin_deck_state_t target;
    switch (cmd) {
    case PIN_DECK_CMD_PLAY:  target = PIN_DECK_PLAYING; break;
    case PIN_DECK_CMD_PAUSE: target = PIN_DECK_PAUSED; break;
    case PIN_DECK_CMD_STOP:  target = PIN_DECK_STOPPED; break;
    case PIN_DECK_CMD_FF:    target = PIN_DECK_FAST_FORWARD; break;
    case PIN_DECK_CMD_REW:   target = PIN_DECK_REWINDING; break;
    default:
        pthread_mutex_unlock(&s->lock);
        return PIN_ERR_ARG;
    }

    if (cmd == PIN_DECK_CMD_STOP && s->state == PIN_STATE_CAPTURING) {
        /* "while capturing: stops the capture first, then the deck" —
         * the worker tick will notice cap.active go false via
         * pin_capture_stop-equivalent handling; here we just flag it. */
        s->cap.stopping = 1;
        s->cap.stopping_since = now;
    }

    s->deck.busy = 1;
    s->deck.busy_until = now + DECK_LATENCY_S;
    s->deck.pending_state = target;
    if (s->deck.no_tape && target != PIN_DECK_STOPPED) {
        /* refuse transport commands with no tape, except settling to stop */
        s->deck.pending_state = PIN_DECK_NO_TAPE;
    }
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    return PIN_OK;
}

/* Called by the worker thread once per tick (caller does NOT hold s->lock). */
void pin_stub_deck_tick(pin_session_t *s, double now) {
    pthread_mutex_lock(&s->lock);
    pin_stub_deck_t *d = &s->deck;

    if (d->busy && now >= d->busy_until) {
        d->busy = 0;
        d->state = d->pending_state;
        pin_evtq_push(&s->events, PIN_EVT_DECK, (int32_t)d->state, "");
    }

    const double dt = 0.05; /* nominal worker tick period, see pin_stub_worker_main */

    switch (d->state) {
    case PIN_DECK_PLAYING:
        d->tc_frames += (long long)(25 * dt);
        d->tape_percent += (100.0 / (TAPE_TOTAL_FRAMES / 25.0)) * dt;
        break;
    case PIN_DECK_FAST_FORWARD:
        d->tc_frames += (long long)(25 * 8 * dt);
        d->tape_percent += (100.0 / (TAPE_TOTAL_FRAMES / 25.0)) * dt * 8;
        break;
    case PIN_DECK_REWINDING:
        d->tc_frames -= (long long)(25 * 8 * dt);
        d->tape_percent -= (100.0 / (TAPE_TOTAL_FRAMES / 25.0)) * dt * 8;
        if (d->tc_frames <= 0) {
            d->tc_frames = 0;
            d->tape_percent = 0;
            /* prolonged rewind past zero: BOT reached, settle to stopped */
            d->state = PIN_DECK_STOPPED;
            d->no_tape = 0;
            pin_evtq_push(&s->events, PIN_EVT_DECK, (int32_t)d->state, "rewound to start");
        }
        break;
    default:
        break;
    }

    if (d->tc_frames > TAPE_TOTAL_FRAMES) {
        d->tc_frames = TAPE_TOTAL_FRAMES;
        d->tape_percent = 100.0;
        if (d->state == PIN_DECK_PLAYING || d->state == PIN_DECK_FAST_FORWARD) {
            d->state = PIN_DECK_STOPPED; /* end of tape */
            pin_evtq_push(&s->events, PIN_EVT_DECK, (int32_t)d->state, "end of tape");
        }
    }
    if (d->tape_percent < 0) d->tape_percent = 0;
    if (d->tape_percent > 100) d->tape_percent = 100;

    pthread_mutex_unlock(&s->lock);
}
