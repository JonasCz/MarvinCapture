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
 * The step sequencer behind pin_script_run() (docs/cli.md): a helper thread
 * that performs a parsed script's settings and actions against a session, and
 * evaluates the wait conditions (pin_script_eval.c) on status snapshots.
 *
 * Why it is shaped the way it is:
 *  - The session's command mailbox has a single slot, so after every command
 *    the sequencer waits until the worker has taken it (cmd.pending clear) and,
 *    for a deck command, until the AV/C transaction is done (deck_busy clear).
 *  - A capture is open from pin_capture_start() until the next transport
 *    action, the next --capture or the end. Whether *its* end was abnormal is
 *    read from the session's stop reason, matched to this capture by
 *    capture_end_seq (the stop_reason in a snapshot may still be the previous
 *    capture's until the worker has processed the start command).
 *  - Every wait loop goes through poll(), which checks for cancellation, a
 *    capture that ended abnormally and a session in ERROR.
 */

#include "pin_script.h"
#include "pin_script_eval.h"
#include "pin_session.h"
#include "pin_session_priv.h"
#include "pin_stop.h"
#include "../core/pin_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

#define EXIT_USAGE 1
#define EXIT_DEVICE 2
#define EXIT_DECK 3
#define EXIT_CAPTURE 4
#define EXIT_CANCELLED 130

#define POLL_MS 100

typedef struct {
    pin_session_t *s;
    pin_script_t *sc;
    pin_script_track_t track;
    pin_script_obs_t obs;           /* of the last poll() */
    char reason[PIN_PATH_MAX + 64]; /* why the script stops, when it does */

    int capture_open;
    unsigned end_seq0;              /* capture_end_seq before the capture was started */
    const pin_script_capture_t *cap;
    int ext_checked;

    int moved_tape;                 /* a transport command was sent */
    pin_deck_cmd_t last_cmd;
} run_t;

static void nap(int ms)
{
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

static int failf(run_t *r, int code, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->reason, sizeof(r->reason), fmt, ap);
    va_end(ap);
    return code;
}

static int cancelled(run_t *r)
{
    pin_session_lock(r->s);
    int c = r->s->script_cancel;
    pin_session_unlock(r->s);
    return c;
}

static const char *kind_name(int kind)
{
    return kind == PIN_KIND_HDV ? "HDV" : kind == PIN_KIND_DV ? "DV" : "analog";
}

static const char *kind_default_ext(int kind)
{
    return kind == PIN_KIND_HDV ? "ts" : kind == PIN_KIND_DV ? "dv" : "avi";
}

/* The capture of this script ended (by itself or on our stop): consumes it and
 * returns the exit code if it was abnormal, else 0. */
static int collect_capture_end(run_t *r)
{
    pin_session_lock(r->s);
    unsigned seq = r->s->capture_end_seq;
    pin_stop_reason_t why = r->s->stop_reason;
    char text[PIN_TEXT_MAX];
    snprintf(text, sizeof(text), "%s", r->s->stop_text);
    pin_session_unlock(r->s);
    if (seq == r->end_seq0)
        return -1; /* still going */
    r->capture_open = 0;
    if (pin_stop_abnormal(why))
        return failf(r, EXIT_CAPTURE, "%s", text[0] ? text : "the capture ended abnormally");
    return 0;
}

/* One look at the session: snapshot, observation for the evaluator, and the
 * things that stop a script whatever it is doing. Returns 0 or an exit code. */
static int poll(run_t *r, pin_status_snapshot_t *st)
{
    pin_status_snapshot_t local;
    if (!st)
        st = &local;
    if (cancelled(r))
        return failf(r, EXIT_CANCELLED, "cancelled");
    memset(st, 0, sizeof(*st));
    st->size = sizeof(*st);
    pin_session_get_status(r->s, st);

    if (r->capture_open) {
        int c = collect_capture_end(r);
        if (c > 0)
            return c;
        if (r->capture_open && !r->ext_checked && st->state == PIN_STATE_CAPTURING) {
            r->ext_checked = 1;
            int k = (int)st->stream_kind;
            if (r->cap && k >= 0 && k < 3 && (r->cap->warn_mask & (1u << k)))
                pin_logf(PIN_LOG_WARN, "script: %s: the file name's extension does not suit %s video; "
                         "writing the default format (.%s) instead\n", r->cap->path, kind_name(k),
                         kind_default_ext(k));
        }
    }
    if (st->state == PIN_STATE_ERROR) {
        int deck = st->last_error == PIN_ERR_NO_CAMERA || st->last_error == PIN_ERR_DECK;
        return failf(r, deck ? EXIT_DECK : EXIT_DEVICE, "%s",
                     st->error_text[0] ? st->error_text : pin_strerror(st->last_error));
    }

    r->obs.now = pin_session_now();
    r->obs.deck = st->deck;
    r->obs.camera_present = st->camera_present;
    r->obs.signal = st->signal;
    snprintf(r->obs.timecode, sizeof(r->obs.timecode), "%s", st->timecode);
    r->obs.fps = st->video_fps_num > 0 && st->video_fps_den > 0
                     ? (double)st->video_fps_num / st->video_fps_den : 25.0;
    pin_script_track_observe(&r->track, &r->obs);
    return 0;
}

/* Waits until the session is READY (bring-up, a capture being finalised...). */
static int wait_ready(run_t *r, double timeout_s)
{
    double t0 = pin_session_now();
    for (;;) {
        pin_status_snapshot_t st;
        int rc = poll(r, &st);
        if (rc)
            return rc;
        if (st.state == PIN_STATE_READY)
            return 0;
        if (pin_session_now() - t0 > timeout_s)
            return failf(r, EXIT_DEVICE, "the device did not become ready in %.0f s", timeout_s);
        nap(POLL_MS);
    }
}

static int ensure_input(run_t *r, pin_input_t want)
{
    pin_status_snapshot_t st = { .size = sizeof(st) };
    pin_session_get_status(r->s, &st);
    if (st.state == PIN_STATE_ERROR)
        return failf(r, EXIT_DEVICE, "%s", st.error_text[0] ? st.error_text : pin_strerror(st.last_error));
    if (st.state == PIN_STATE_CLOSED || st.input != want) {
        pin_status_t rc = pin_session_set_input(r->s, want);
        if (rc != PIN_OK)
            return failf(r, EXIT_DEVICE, "cannot switch the input: %s", pin_strerror(rc));
    }
    return wait_ready(r, 300.0);
}

/* Stops the open capture (the tape keeps running unless sd says so) and waits
 * until its files are closed. Not interruptible: finalising is the point. */
static int close_capture(run_t *r, pin_stop_deck_t sd)
{
    if (!r->capture_open)
        return 0;
    pin_session_capture_stop(r->s, sd);
    double t0 = pin_session_now();
    while (pin_session_capture_busy(r->s)) {
        if (pin_session_now() - t0 > 1800.0)
            return failf(r, EXIT_CAPTURE, "the capture did not finish closing");
        nap(50);
    }
    int c = collect_capture_end(r);
    if (c > 0)
        return c;
    r->capture_open = 0;
    return 0;
}

/* After a deck command: waits until the worker took it and the AV/C
 * transaction is over (the mailbox has one slot; a second command would
 * overwrite the first). */
static int wait_command_sent(run_t *r)
{
    double t0 = pin_session_now();
    for (;;) {
        pin_session_lock(r->s);
        int busy = r->s->cmd.pending || r->s->deck_busy || r->s->deck_q_valid;
        pin_session_unlock(r->s);
        if (!busy)
            return 0;
        int rc = poll(r, NULL);
        if (rc)
            return rc;
        if (pin_session_now() - t0 > 15.0)
            return failf(r, EXIT_DECK, "the deck did not answer the command");
        nap(50);
    }
}

static int do_transport(run_t *r, pin_deck_cmd_t cmd)
{
    int rc = close_capture(r, PIN_STOP_DECK_NO);
    if (rc)
        return rc;
    /* is a camera there (the answer takes a moment after bring-up)? */
    double t0 = pin_session_now();
    pin_status_snapshot_t st;
    for (;;) {
        rc = poll(r, &st);
        if (rc)
            return rc;
        if (st.camera_present != -1 || pin_session_now() - t0 > 10.0)
            break;
        nap(POLL_MS);
    }
    if (st.camera_present == 0)
        return failf(r, EXIT_DECK, "no camera or deck on the FireWire bus");
    rc = wait_command_sent(r); /* a command from before may still be in flight */
    if (rc)
        return rc;
    pin_status_t ps = pin_session_deck(r->s, cmd);
    if (ps != PIN_OK)
        return failf(r, EXIT_DECK, "the deck command was refused: %s", pin_strerror(ps));
    pin_script_track_transport(&r->track, cmd, pin_session_now());
    r->moved_tape = 1;
    r->last_cmd = cmd;
    return wait_command_sent(r);
}

static int do_capture(run_t *r, const pin_script_capture_t *c)
{
    int rc = close_capture(r, PIN_STOP_DECK_NO);
    if (rc)
        return rc;
    rc = wait_ready(r, 120.0);
    if (rc)
        return rc;

    pin_capture_opts_t o;
    pin_capture_opts_defaults(&o);
    snprintf(o.path, sizeof(o.path), "%s", c->base);
    o.format_analog = c->format[PIN_KIND_ANALOG];
    o.format_dv = c->format[PIN_KIND_DV];
    o.format_hdv = c->format[PIN_KIND_HDV];
    snprintf(o.title, sizeof(o.title), "%s", c->title);
    o.aspect = c->aspect;
    o.scene_split = c->split;
    o.keep_raw = c->keep_raw;
    o.start_deck = 0;
    o.rewind_first = 0;
    o.passes = 1;
    o.idle_stop_minutes = 0;
    o.max_duration_minutes = 0;

    pin_output_check_t chk;
    pin_status_t cs = pin_session_check_output(r->s, &o, &chk);
    if (cs == PIN_ERR_ARG)
        return failf(r, EXIT_USAGE, "--capture %s: %s", c->path, chk.message[0] ? chk.message : "unusable file name");
    if (!c->overwrite) {
        /* pin_check_output() looks at the kind that is arriving, which a stopped deck has
         * not told us yet: check the file name of every kind that could arrive. */
        pin_status_snapshot_t st;
        rc = poll(r, &st);
        if (rc)
            return rc;
        pin_format_t cand[2];
        int ncand = 0;
        if (st.input != PIN_INPUT_DV) {
            cand[ncand++] = o.format_analog;
        } else if (st.signal && st.stream_kind == PIN_KIND_HDV) {
            cand[ncand++] = o.format_hdv;
        } else if (st.signal && st.stream_kind == PIN_KIND_DV) {
            cand[ncand++] = o.format_dv;
        } else {
            cand[ncand++] = o.format_dv;
            cand[ncand++] = o.format_hdv;
        }
        for (int i = 0; i < ncand; i++) {
            pin_format_info_t fi = { .size = sizeof(fi) };
            char match[PIN_PATH_MAX];
            if (pin_session_format_info(cand[i], &fi) == PIN_OK &&
                pin_naming_collides(c->base, fi.extension, match, sizeof(match)) > 0)
                return failf(r, EXIT_USAGE, "%s already exists (use --overwrite to replace it)", match);
        }
    }

    pin_session_lock(r->s);
    r->end_seq0 = r->s->capture_end_seq;
    pin_session_unlock(r->s);
    pin_status_t ps = pin_session_capture_start(r->s, &o, c->overwrite);
    if (ps == PIN_ERR_EXISTS)
        return failf(r, EXIT_USAGE, "%s already exists (use --overwrite to replace it)", c->path);
    if (ps != PIN_OK)
        return failf(r, EXIT_DEVICE, "cannot start the capture: %s", pin_strerror(ps));
    r->capture_open = 1;
    r->cap = c;
    r->ext_checked = c->warn_mask == 0;
    /* the worker takes the command; the capture itself may wait for its first frame */
    double t0 = pin_session_now();
    for (;;) {
        pin_session_lock(r->s);
        int pending = r->s->cmd.pending;
        pin_session_unlock(r->s);
        if (!pending)
            return 0;
        rc = poll(r, NULL);
        if (rc)
            return rc;
        if (pin_session_now() - t0 > 15.0)
            return failf(r, EXIT_DEVICE, "the capture did not start");
        nap(20);
    }
}

static int do_wait(run_t *r, const pin_script_item_t *it)
{
    int rc = poll(r, NULL);
    if (rc)
        return rc;
    pin_script_track_wait_begin(&r->track, &r->obs);
    for (;;) {
        rc = poll(r, NULL);
        if (rc)
            return rc;
        char why[160];
        int idx = 0;
        pin_eval_result_t res = pin_script_eval(it->conds, it->nconds, &r->obs, &r->track, &idx, why, sizeof(why));
        if (res == PIN_EVAL_MET)
            return 0;
        if (res == PIN_EVAL_ERROR)
            return failf(r, EXIT_DECK, "%s", why);
        nap(POLL_MS);
    }
}

/* A --capture whose file exists (and no --overwrite) fails when its step is reached,
 * by which time earlier steps may have started the tape. Check them all up front,
 * before the first command, for the kinds that can arrive with the input that is
 * selected at that point of the script. Returns an exit code, 0 if fine. */
static int precheck_targets(run_t *r, pin_input_t start_input)
{
    pin_script_t *sc = r->sc;
    pin_input_t input = start_input;
    for (int i = 0; i < sc->nitems; i++) {
        const pin_script_item_t *it = &sc->items[i];
        if (it->kind == PIN_SITEM_INPUT) {
            input = it->input;
            continue;
        }
        if (it->kind != PIN_SITEM_STEP || it->step_kind != PIN_SSTEP_CAPTURE || it->cap.overwrite)
            continue;
        pin_format_t cand[2];
        int ncand = 0;
        if (input != PIN_INPUT_DV) {
            cand[ncand++] = it->cap.format[PIN_KIND_ANALOG];
        } else {
            cand[ncand++] = it->cap.format[PIN_KIND_DV];
            cand[ncand++] = it->cap.format[PIN_KIND_HDV];
        }
        for (int k = 0; k < ncand; k++) {
            pin_format_info_t fi = { .size = sizeof(fi) };
            char match[PIN_PATH_MAX];
            if (pin_session_format_info(cand[k], &fi) == PIN_OK &&
                pin_naming_collides(it->cap.base, fi.extension, match, sizeof(match)) > 0)
                return failf(r, EXIT_USAGE, "%s already exists (use --overwrite to replace it)", match);
        }
    }
    return 0;
}

static int run_items(run_t *r)
{
    pin_script_t *sc = r->sc;
    pin_session_t *s = r->s;

    for (int i = 0; i < sc->nitems; i++) {
        const pin_script_item_t *it = &sc->items[i];
        if (it->kind == PIN_SITEM_STEP && it->step_kind == PIN_SSTEP_CAPTURE && it->cap.to_stdout)
            return failf(r, EXIT_USAGE, "--capture -: writing to stdout is not supported yet");
    }

    /* the input the first step needs: the script's, else DV if the session was never brought up */
    int have_pre = 0;
    pin_input_t pre = PIN_INPUT_DV;
    for (int i = 0; i < sc->nitems && sc->items[i].kind != PIN_SITEM_STEP; i++)
        if (sc->items[i].kind == PIN_SITEM_INPUT) {
            have_pre = 1;
            pre = sc->items[i].input;
        }
    pin_status_snapshot_t st = { .size = sizeof(st) };
    pin_session_get_status(s, &st);
    int rc = precheck_targets(r, have_pre || st.state == PIN_STATE_CLOSED ? pre : st.input);
    if (rc)
        return rc;
    if (have_pre || st.state == PIN_STATE_CLOSED)
        rc = ensure_input(r, pre);
    else
        rc = wait_ready(r, 300.0);
    if (rc)
        return rc;

    for (int i = 0; i < sc->nitems; i++) {
        const pin_script_item_t *it = &sc->items[i];
        rc = poll(r, &st);
        if (rc)
            return rc;
        switch (it->kind) {
        case PIN_SITEM_INPUT:
            rc = ensure_input(r, it->input);
            break;
        case PIN_SITEM_STD:
            if (st.input == PIN_INPUT_DV)
                pin_logf(PIN_LOG_WARN, "script: --std ignored: the DV input has no video standard setting\n");
            else if (pin_session_set_standard(s, it->std) != PIN_OK)
                pin_logf(PIN_LOG_WARN, "script: the standard could not be set\n");
            break;
        case PIN_SITEM_CONTROL:
            if (st.input == PIN_INPUT_DV)
                pin_logf(PIN_LOG_WARN, "script: analog control ignored: the DV input has none\n");
            else if (pin_session_set_control(s, it->ctl, it->value) != PIN_OK)
                pin_logf(PIN_LOG_WARN, "script: analog control %d could not be set to %d\n", (int)it->ctl,
                         (int)it->value);
            break;
        case PIN_SITEM_STEP: {
            char desc[PIN_PATH_MAX + 32];
            pin_script_step_description(sc, it->step, desc, sizeof(desc));
            pin_session_push_event(s, PIN_EVT_STEP, it->step, desc);
            switch (it->step_kind) {
            case PIN_SSTEP_REW: rc = do_transport(r, PIN_DECK_CMD_REW); break;
            case PIN_SSTEP_FF: rc = do_transport(r, PIN_DECK_CMD_FF); break;
            case PIN_SSTEP_PLAY: rc = do_transport(r, PIN_DECK_CMD_PLAY); break;
            case PIN_SSTEP_PAUSE: rc = do_transport(r, PIN_DECK_CMD_PAUSE); break;
            case PIN_SSTEP_STOP: rc = do_transport(r, PIN_DECK_CMD_STOP); break;
            case PIN_SSTEP_CAPTURE: rc = do_capture(r, &it->cap); break;
            case PIN_SSTEP_WAIT: rc = do_wait(r, it); break;
            }
            break;
        }
        }
        if (rc)
            return rc;
    }
    return 0;
}

static void *script_main(void *arg)
{
    run_t *r = arg;
    pin_session_t *s = r->s;
    r->reason[0] = 0;
    int code = run_items(r);
    char reason[sizeof(r->reason)];
    snprintf(reason, sizeof(reason), "%s", r->reason);

    /* Whatever happened, an open capture is finalised (the tape keeps running). */
    int rc2 = close_capture(r, PIN_STOP_DECK_NO);
    if (code == 0 && rc2) {
        code = rc2;
        snprintf(reason, sizeof(reason), "%s", r->reason);
    }
    /* Ctrl-C: stop the tape too if this script moved it. */
    if (code == EXIT_CANCELLED && r->moved_tape && r->last_cmd != PIN_DECK_CMD_STOP) {
        pin_status_snapshot_t st = { .size = sizeof(st) };
        pin_session_get_status(s, &st);
        if (st.camera_present != 0 && st.state != PIN_STATE_ERROR) {
            pin_session_deck(s, PIN_DECK_CMD_STOP);
            double t0 = pin_session_now();
            for (;;) {
                pin_session_lock(s);
                int busy = s->cmd.pending || s->deck_busy || s->deck_q_valid;
                pin_session_unlock(s);
                if (!busy || pin_session_now() - t0 > 15.0)
                    break;
                nap(50);
            }
        }
    }

    pin_session_lock(s);
    s->script_running = 0;
    pin_session_unlock(s);
    pin_session_push_event(s, PIN_EVT_DONE, code, code ? reason : NULL);
    pin_script_destroy(r->sc);
    free(r);
    return NULL;
}

pin_status_t pin_session_script_run(pin_session_t *s, const struct pin_script *sc)
{
    if (!s || !sc)
        return PIN_ERR_ARG;
    pin_session_lock(s);
    if (s->script_running) {
        pin_session_unlock(s);
        return PIN_ERR_STATE;
    }
    int join_old = s->script_thread_valid;
    pthread_t old = s->script_thread;
    s->script_thread_valid = 0;
    s->script_running = 1; /* reserved */
    s->script_cancel = 0;
    pin_session_unlock(s);
    if (join_old)
        pthread_join(old, NULL);

    run_t *r = calloc(1, sizeof(*r));
    pin_script_t *copy = pin_script_clone(sc);
    if (!r || !copy) {
        free(r);
        pin_script_destroy(copy);
        pin_session_lock(s);
        s->script_running = 0;
        pin_session_unlock(s);
        return PIN_ERR_NOMEM;
    }
    r->s = s;
    r->sc = copy;
    r->last_cmd = PIN_DECK_CMD_STOP;
    pin_script_track_init(&r->track);

    pthread_t t;
    if (pthread_create(&t, NULL, script_main, r) != 0) {
        pin_script_destroy(copy);
        free(r);
        pin_session_lock(s);
        s->script_running = 0;
        pin_session_unlock(s);
        return PIN_ERR_INTERNAL;
    }
    pin_session_lock(s);
    s->script_thread = t;
    s->script_thread_valid = 1;
    pin_session_unlock(s);
    return PIN_OK;
}

pin_status_t pin_session_script_cancel(pin_session_t *s)
{
    if (!s)
        return PIN_ERR_ARG;
    pin_session_lock(s);
    int running = s->script_running;
    if (running)
        s->script_cancel = 1;
    pin_session_unlock(s);
    return running ? PIN_OK : PIN_ERR_STATE;
}

void pin_session_script_shutdown(pin_session_t *s)
{
    pin_session_lock(s);
    int valid = s->script_thread_valid;
    pthread_t t = s->script_thread;
    s->script_thread_valid = 0;
    if (s->script_running)
        s->script_cancel = 1;
    pin_session_unlock(s);
    if (valid)
        pthread_join(t, NULL);
}
