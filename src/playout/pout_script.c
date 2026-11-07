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

/* MarvinPlayout's argument parser and sequencer. See pout_script.h. */

#include "pout_script.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *pout_script_help_text(void)
{
    return
        "Usage: MarvinPlayout [settings] FILE... [settings and actions...]\n"
        "\n"
        "Plays DV (.dv) or HDV (.m2t, .ts) files out over FireWire as a tape in a virtual deck. The files are one tape, in the order given, one recording each. The deck answers AV/C commands from the device on the other end (PLAY, PAUSE, STOP, FF, REW, timecode search) and can also be driven by actions on this command line. With no actions it serves as a deck until Ctrl-C (with --dry-run: plays the whole tape once and exits).\n"
        "Arguments are processed left to right, as in MarvinCaptureCLI. Files and settings come before the first action. An action's argument is the next word unless it starts with --; --wait=idle also works.\n"
        "Transport actions return as soon as the deck has taken the command; like a real deck, the tape needs time: a mode change takes a moment, winding runs at a fixed multiple of real time. Use a wait to wait for the action to complete (for example --rew --wait).\n"
        "\n"
        "Settings:\n"
        "  -f, --file FILE          a tape file; same as a bare FILE\n"
        "  -d, --device ID          device id (usb:2-1) or 16-hex serial (default: first device)\n"
        "  --debug                  debug logging (raw AV/C traffic, bring-up details)\n"
        "  --channel N              isochronous channel to send on, 0 to 63 (default 63)\n"
        "  --speed s100|s200|s400   FireWire speed of the stream (default s100)\n"
        "  --no-virtual-deck        do not answer AV/C commands; only the actions on this command line drive the deck\n"
        "  --wind-speed X           FF, REW and search run at X times real time (default 30)\n"
        "  --start-delay SEC        seconds from PLAY to the first picture, stopped or paused (default 1)\n"
        "  --search-settle SEC      seconds a timecode search waits after arriving, before it reports paused (default 1.5)\n"
        "  --dry-run OUT            no device: write what would be sent to the file OUT (.dv or .m2t) through the loopback output\n"
        "  --realtime               with --dry-run: pace like the bus instead of as fast as possible\n"
        "  --stay                   keep serving the virtual deck after the actions, until Ctrl-C\n"
        "\n"
        "Actions:\n"
        "  --play --pause --stop --ff --rew\n"
        "                           transport command; returns as soon as the deck has taken it. A search ends paused.\n"
        "  --goto HH:MM:SS:FF       timecode search: wind to that timecode, settle, then pause there. Fails if the timecode is not on the tape.\n"
        "  --wait-any COND[,COND...]\n"
        "                           block until any condition is met\n"
        "  --wait-all COND[,COND...]\n"
        "                           block until all conditions are met\n"
        "  --wait [COND[,COND...]]  same as --wait-any; bare --wait is --wait-any idle\n"
        "\n"
        "Wait conditions (DUR is HH:MM:SS or HH:MM:SS:FF, no leading +):\n"
        "  idle[=DUR]               the deck was moving after the last transport command and is now stopped or paused, and has stayed still for DUR (default 00:00:05; never less than about 3 s)\n"
        "  timecode=HH:MM:SS:FF     the deck timecode reaches or passes this value in the direction the tape moves; fails if the deck goes idle first\n"
        "  wallclock=DUR            DUR of real time has passed since the wait began\n"
        "  (signal, nosignal and captured of MarvinCaptureCLI do not apply here.) Waits are evaluated as in MarvinCaptureCLI, see docs/cli.md.\n"
        "\n"
        "Exit codes: 0 ok, 1 usage error, 2 device or bring-up error, 3 a wait failed or the deck could not do an action, 130 cancelled (Ctrl-C).\n"
        "\n"
        "Examples:\n"
        "  Play a tape from the start, send it out and stop at 10 s:\n"
        "    tape.m2t --rew --wait --play --wait timecode=00:00:10:00 --stop\n"
        "  Two recordings as one tape, start at the second:\n"
        "    a.dv b.dv --goto 00:12:00:00 --wait --play --wait\n"
        "  Write the stream to a file as fast as possible, to check it:\n"
        "    tape.dv --dry-run out.dv\n"
        "  Serve as a deck for a capture program on the other end of the cable:\n"
        "    tape.dv\n"
        "\n"
        "-h, --help, or no arguments at all: this text.\n";
}

/* ---- parser ---------------------------------------------------------------------- */

typedef struct {
    pout_script_t *sc;
    char *err;
    size_t err_cap;
    int started;        /* an action has been seen */
} parser_t;

static pin_status_t fail(parser_t *p, const char *fmt, ...)
{
    if (p->err && p->err_cap) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(p->err, p->err_cap, fmt, ap);
        va_end(ap);
    }
    return PIN_ERR_ARG;
}

static int parse_int(const char *s, long *out)
{
    char *end;
    if (!s || !*s)
        return 0;
    long v = strtol(s, &end, 10);
    if (*end)
        return 0;
    *out = v;
    return 1;
}

static int parse_double(const char *s, double *out)
{
    char *end;
    if (!s || !*s)
        return 0;
    double v = strtod(s, &end);
    if (*end || !isfinite(v))
        return 0;
    *out = v;
    return 1;
}

static pout_action_t *action_add(pout_script_t *sc, int *cap)
{
    if (sc->nactions >= *cap) {
        int n = *cap ? *cap * 2 : 8;
        pout_action_t *a = (pout_action_t *)realloc(sc->actions, (size_t)n * sizeof(*a));
        if (!a)
            return NULL;
        sc->actions = a;
        *cap = n;
    }
    pout_action_t *a = &sc->actions[sc->nactions++];
    memset(a, 0, sizeof(*a));
    return a;
}

static int file_add(pout_script_t *sc, const char *path)
{
    char **f = (char **)realloc(sc->files, (size_t)(sc->nfiles + 1) * sizeof(*f));
    if (!f)
        return 0;
    sc->files = f;
    size_t n = strlen(path) + 1;
    f[sc->nfiles] = (char *)malloc(n);
    if (!f[sc->nfiles])
        return 0;
    memcpy(f[sc->nfiles], path, n);
    sc->nfiles++;
    return 1;
}

void pout_script_destroy(pout_script_t *sc)
{
    if (!sc)
        return;
    for (int i = 0; i < sc->nfiles; i++)
        free(sc->files[i]);
    free(sc->files);
    free(sc->actions);
    free(sc);
}

pin_status_t pout_script_parse(int argc, const char *const *argv, pout_script_t **out, char *err, size_t err_cap)
{
    *out = NULL;
    if (err && err_cap)
        err[0] = 0;
    pout_script_t *sc = (pout_script_t *)calloc(1, sizeof(*sc));
    if (!sc)
        return PIN_ERR_NOMEM;
    parser_t p = { sc, err, err_cap, 0 };
    sc->channel = 63;
    sc->speed = 100;
    sc->virtual_deck = 1;
    pout_deck_timing_defaults(&sc->timing);

    if (argc <= 0) {
        sc->help = 1;
        *out = sc;
        return PIN_OK;
    }
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            sc->help = 1;
            *out = sc;
            return PIN_OK;
        }
    }

    static const char *const value_opts[] = { "--device", "--file", "--channel", "--speed", "--wind-speed",
        "--start-delay", "--search-settle", "--dry-run" };
    pin_status_t rc = PIN_OK;
    int acap = 0;
    for (int i = 0; i < argc; i++) {
        const char *arg = argv[i];
        char name[64];
        const char *inl = NULL;     /* value after '=' */

#define FAIL(...) do { rc = fail(&p, __VA_ARGS__); goto parse_end; } while (0)
        if (arg[0] != '-') {
            if (p.started)
                FAIL("\"%s\": tape files must come before the first action", arg);
            if (!*arg || strlen(arg) >= PIN_PATH_MAX)
                FAIL("bad file name \"%s\"", arg);
            if (!file_add(sc, arg)) { rc = PIN_ERR_NOMEM; goto parse_end; }
            continue;
        }
        if (arg[1] == '-') {
            const char *eq = strchr(arg, '=');
            size_t n = eq ? (size_t)(eq - arg) : strlen(arg);
            if (n >= sizeof(name))
                FAIL("unknown option \"%s\"", arg);
            memcpy(name, arg, n);
            name[n] = 0;
            inl = eq ? eq + 1 : NULL;
        } else if (arg[1] == 'd' && !arg[2]) {
            snprintf(name, sizeof(name), "--device");
        } else if (arg[1] == 'f' && !arg[2]) {
            snprintf(name, sizeof(name), "--file");
        } else {
            FAIL("unknown option \"%s\"", arg);
        }

        const char *val = NULL;
        int takes_value = 0;
        for (unsigned k = 0; k < sizeof(value_opts) / sizeof(value_opts[0]); k++)
            if (strcmp(name, value_opts[k]) == 0)
                takes_value = 1;
        if (takes_value) {
            if (inl)
                val = inl;
            else if (i + 1 < argc)
                val = argv[++i];
            else
                FAIL("%s needs a value", name);
            if (p.started)
                FAIL("%s must come before the first action", name);
        }
        int is_flag = strcmp(name, "--debug") == 0 || strcmp(name, "--no-virtual-deck") == 0 ||
                      strcmp(name, "--realtime") == 0 || strcmp(name, "--stay") == 0;
        if (is_flag && p.started)
            FAIL("%s must come before the first action", name);

        long iv;
        double dv;
        if (strcmp(name, "--file") == 0) {
            if (!*val || strlen(val) >= PIN_PATH_MAX)
                FAIL("--file needs a file name");
            if (!file_add(sc, val)) { rc = PIN_ERR_NOMEM; goto parse_end; }
        } else if (strcmp(name, "--device") == 0) {
            if (!*val || strlen(val) >= sizeof(sc->device))
                FAIL("--device needs a device id");
            snprintf(sc->device, sizeof(sc->device), "%s", val);
            sc->has_device = 1;
        } else if (strcmp(name, "--channel") == 0) {
            if (!parse_int(val, &iv) || iv < 0 || iv > 63)
                FAIL("--channel: expected a whole number from 0 to 63, got \"%s\"", val);
            sc->channel = (int)iv;
        } else if (strcmp(name, "--speed") == 0) {
            if (strcmp(val, "s100") == 0) sc->speed = 100;
            else if (strcmp(val, "s200") == 0) sc->speed = 200;
            else if (strcmp(val, "s400") == 0) sc->speed = 400;
            else FAIL("--speed: expected s100, s200 or s400, got \"%s\"", val);
        } else if (strcmp(name, "--wind-speed") == 0) {
            if (!parse_double(val, &dv) || dv < 0.1 || dv > 1000.0)
                FAIL("--wind-speed: expected a number from 0.1 to 1000, got \"%s\"", val);
            sc->timing.wind_speed = dv;
        } else if (strcmp(name, "--start-delay") == 0) {
            if (!parse_double(val, &dv) || dv < 0 || dv > 60.0)
                FAIL("--start-delay: expected seconds from 0 to 60, got \"%s\"", val);
            sc->timing.play_delay = dv;
        } else if (strcmp(name, "--search-settle") == 0) {
            if (!parse_double(val, &dv) || dv < 0 || dv > 60.0)
                FAIL("--search-settle: expected seconds from 0 to 60, got \"%s\"", val);
            sc->timing.search_settle = dv;
        } else if (strcmp(name, "--dry-run") == 0) {
            if (!*val || strlen(val) >= sizeof(sc->dry_run))
                FAIL("--dry-run needs an output file name");
            snprintf(sc->dry_run, sizeof(sc->dry_run), "%s", val);
            sc->has_dry_run = 1;
        } else if (is_flag) {
            if (inl)
                FAIL("%s takes no value (got \"%s\")", name, inl);
            if (strcmp(name, "--debug") == 0) sc->debug = 1;
            else if (strcmp(name, "--no-virtual-deck") == 0) sc->virtual_deck = 0;
            else if (strcmp(name, "--realtime") == 0) sc->realtime = 1;
            else sc->stay = 1;
        } else if (strcmp(name, "--play") == 0 || strcmp(name, "--pause") == 0 || strcmp(name, "--stop") == 0 ||
                   strcmp(name, "--ff") == 0 || strcmp(name, "--rew") == 0) {
            if (inl)
                FAIL("%s takes no value (got \"%s\")", name, inl);
            pout_action_t *a = action_add(sc, &acap);
            if (!a) { rc = PIN_ERR_NOMEM; goto parse_end; }
            a->kind = strcmp(name, "--play") == 0 ? POUT_ACT_PLAY : strcmp(name, "--pause") == 0 ? POUT_ACT_PAUSE
                      : strcmp(name, "--stop") == 0 ? POUT_ACT_STOP : strcmp(name, "--ff") == 0 ? POUT_ACT_FF
                      : POUT_ACT_REW;
            p.started = 1;
        } else if (strcmp(name, "--goto") == 0) {
            if (!inl && i + 1 < argc && !(argv[i + 1][0] == '-' && argv[i + 1][1] == '-'))
                inl = argv[++i];
            if (!inl)
                FAIL("--goto needs a timecode (HH:MM:SS:FF)");
            pin_tc_t tc;
            char why[96];
            if (!pin_tc_parse(inl, &tc, why, sizeof(why)))
                FAIL("--goto %s: %s", inl, why);
            pout_action_t *a = action_add(sc, &acap);
            if (!a) { rc = PIN_ERR_NOMEM; goto parse_end; }
            a->kind = POUT_ACT_GOTO;
            a->tc = tc;
            p.started = 1;
        } else if (strcmp(name, "--wait") == 0 || strcmp(name, "--wait-any") == 0 || strcmp(name, "--wait-all") == 0) {
            int bare_ok = strcmp(name, "--wait") == 0;
            if (!inl && i + 1 < argc && !(argv[i + 1][0] == '-' && argv[i + 1][1] == '-'))
                inl = argv[++i];
            if (!inl && !bare_ok)
                FAIL("%s needs a condition list", name);
            pout_action_t *a = action_add(sc, &acap);
            if (!a) { rc = PIN_ERR_NOMEM; goto parse_end; }
            a->kind = POUT_ACT_WAIT;
            a->wait_all = strcmp(name, "--wait-all") == 0;
            if (!inl) {
                a->nconds = 1;
                a->conds[0].kind = PIN_COND_IDLE;
                pin_dur_parse("00:00:05", &a->conds[0].tc, NULL, 0);
            } else {
                char buf[256];
                if (strlen(inl) >= sizeof(buf))
                    FAIL("%s: condition list too long", name);
                snprintf(buf, sizeof(buf), "%s", inl);
                char *save = buf;
                for (;;) {
                    char *comma = strchr(save, ',');
                    if (comma)
                        *comma = 0;
                    char why[160];
                    if (a->nconds >= PIN_SCRIPT_MAX_CONDS)
                        FAIL("%s: too many conditions (max %d)", name, PIN_SCRIPT_MAX_CONDS);
                    if (!pin_script_cond_parse(save, &a->conds[a->nconds], why, sizeof(why)))
                        FAIL("%s %s: %s", name, inl, why);
                    pin_script_cond_t *cd = &a->conds[a->nconds++];
                    if (cd->kind == PIN_COND_SIGNAL || cd->kind == PIN_COND_NOSIGNAL || cd->kind == PIN_COND_CAPTURED)
                        FAIL("%s %s: %s does not apply to MarvinPlayout (there is no capture; use idle, "
                             "timecode=HH:MM:SS:FF or wallclock=DUR)", name, inl,
                             cd->kind == PIN_COND_SIGNAL ? "signal" : cd->kind == PIN_COND_NOSIGNAL ? "nosignal"
                                                                                                  : "captured");
                    if (!comma)
                        break;
                    save = comma + 1;
                }
            }
            p.started = 1;
        } else {
            FAIL("unknown option \"%s\"", arg);
        }
    }
    if (sc->nfiles == 0)
        FAIL("no tape file given (a .dv, .m2t or .ts file; see --help)");
    if (sc->realtime && !sc->has_dry_run)
        FAIL("--realtime only applies with --dry-run (a real output is always paced by the bus)");
    if (sc->has_dry_run && sc->has_device)
        FAIL("--dry-run uses no device; leave out --device");
#undef FAIL
parse_end:
    if (rc != PIN_OK) {
        pout_script_destroy(sc);
        return rc;
    }
    *out = sc;
    return PIN_OK;
}

void pout_script_action_text(const pout_script_t *sc, int i, char *out, size_t cap)
{
    if (cap)
        out[0] = 0;
    if (i < 0 || i >= sc->nactions)
        return;
    const pout_action_t *a = &sc->actions[i];
    switch (a->kind) {
    case POUT_ACT_PLAY:  snprintf(out, cap, "play"); break;
    case POUT_ACT_PAUSE: snprintf(out, cap, "pause"); break;
    case POUT_ACT_STOP:  snprintf(out, cap, "stop"); break;
    case POUT_ACT_FF:    snprintf(out, cap, "ff"); break;
    case POUT_ACT_REW:   snprintf(out, cap, "rew"); break;
    case POUT_ACT_GOTO: {
        char t[16];
        pin_tc_format(&a->tc, t, sizeof(t));
        snprintf(out, cap, "goto %s", t);
        break;
    }
    case POUT_ACT_WAIT: {
        size_t n = (size_t)snprintf(out, cap, "%s ", a->wait_all ? "wait-all" : "wait-any");
        for (int k = 0; k < a->nconds && n < cap; k++) {
            char t[32];
            pin_script_cond_text(&a->conds[k], t, sizeof(t));
            n += (size_t)snprintf(out + n, cap - n, "%s%s", k ? "," : "", t);
        }
        break;
    }
    }
}

/* ---- sequencer ------------------------------------------------------------------- */

void pout_seq_init(pout_seq_t *q, const pout_script_t *sc)
{
    memset(q, 0, sizeof(*q));
    q->sc = sc;
    pin_script_track_init(&q->track);
}

static pin_deck_state_t evaluator_deck_state(pout_deck_mode_t s)
{
    switch (s) {
    case POUT_DECK_PLAY:  return PIN_DECK_PLAYING;
    case POUT_DECK_PAUSE: return PIN_DECK_PAUSED;
    case POUT_DECK_FF:    return PIN_DECK_FAST_FORWARD;
    case POUT_DECK_REW:   return PIN_DECK_REWINDING;
    default:              return PIN_DECK_STOPPED;
    }
}

static pin_deck_cmd_t evaluator_cmd(pout_act_kind_t k)
{
    switch (k) {
    case POUT_ACT_PLAY:  return PIN_DECK_CMD_PLAY;
    case POUT_ACT_PAUSE: return PIN_DECK_CMD_PAUSE;
    case POUT_ACT_FF:    return PIN_DECK_CMD_FF;
    case POUT_ACT_REW:   return PIN_DECK_CMD_REW;
    default:             return PIN_DECK_CMD_STOP;
    }
}

static pout_seq_status_t finish(pout_seq_t *q, pout_seq_out_t *out, pout_seq_status_t st)
{
    out->status = st;
    out->exit_code = st == POUT_SEQ_FAILED ? POUT_EXIT_DECK : POUT_EXIT_OK;
    (void)q;
    return st;
}

pout_seq_status_t pout_seq_step(pout_seq_t *q, const pout_obs_t *obs, pout_seq_out_t *out)
{
    const pout_script_t *sc = q->sc;
    pin_script_obs_t o;
    memset(&o, 0, sizeof(o));
    o.now = obs->now;
    o.deck = evaluator_deck_state(obs->state);
    o.camera_present = 1;       /* the virtual deck is always there, and has a tape */
    o.signal = obs->picture;
    o.fps = obs->fps;
    if (obs->tc_valid)
        pin_tc_format(&obs->tc, o.timecode, sizeof(o.timecode));

    memset(out, 0, sizeof(*out));
    out->action = q->next;
    pin_script_track_observe(&q->track, &o);    /* motion between actions must not be missed */

    for (;;) {
        out->action = q->next;
        if (q->failed)
            return finish(q, out, POUT_SEQ_FAILED);
        if (q->finished || q->next >= sc->nactions) {
            q->finished = 1;
            return finish(q, out, POUT_SEQ_DONE);
        }
        const pout_action_t *a = &sc->actions[q->next];
        if (a->kind == POUT_ACT_WAIT) {
            if (!q->in_wait) {
                pin_script_track_wait_begin(&q->track, &o);
                q->in_wait = 1;
            }
            unsigned mask;
            pin_eval_result_t r = pin_script_eval(a->conds, a->nconds, a->wait_all, &o, &q->track, &mask,
                                                  out->why, sizeof(out->why));
            if (r == PIN_EVAL_PENDING) {
                out->status = POUT_SEQ_WAITING;
                return POUT_SEQ_WAITING;
            }
            if (r == PIN_EVAL_ERROR) {
                q->failed = 1;
                return finish(q, out, POUT_SEQ_FAILED);
            }
            q->in_wait = 0;
            q->next++;
            continue;
        }
        /* a transport action */
        pout_act_kind_t cmd = a->kind;
        pin_deck_cmd_t tcmd;
        if (a->kind == POUT_ACT_GOTO) {
            /* the evaluator needs the direction for timecode waits that follow */
            tcmd = obs->tc_valid && pin_tc_compare(&a->tc, &obs->tc) < 0 ? PIN_DECK_CMD_REW : PIN_DECK_CMD_FF;
            out->tc = a->tc;
        } else {
            tcmd = evaluator_cmd(a->kind);
        }
        pin_script_track_transport(&q->track, tcmd, obs->now);
        out->cmd = cmd;
        out->status = POUT_SEQ_COMMAND;
        q->next++;
        return POUT_SEQ_COMMAND;
    }
}

void pout_seq_command_failed(pout_seq_t *q, const char *why, pout_seq_out_t *out)
{
    q->failed = 1;
    memset(out, 0, sizeof(*out));
    out->action = q->next > 0 ? q->next - 1 : 0;
    snprintf(out->why, sizeof(out->why), "%s", why ? why : "the deck refused the command");
    finish(q, out, POUT_SEQ_FAILED);
}
