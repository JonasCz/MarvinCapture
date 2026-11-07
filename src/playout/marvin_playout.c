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
 * MarvinPlayout: plays DV/HDV files out over FireWire through the Pinnacle device as a virtual
 * deck, and/or writes the stream to a file (--dry-run). See docs/playout.md. Diagnostics and status
 * go to stderr; only --help prints to stdout.
 */

#include "pin_log.h"
#include "pout_hw_session.h"
#include "pout_out.h"
#include "pout_script.h"
#include "pout_session.h"
#include "pout_switch.h"
#include "pout_tape.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

/* ---- Ctrl-C (as MarvinCaptureCLI): the first press stops in order, the second quits at once ---- */

static volatile sig_atomic_t g_interrupts;

static void note_interrupt(void)
{
    if (++g_interrupts >= 2)
        _exit(POUT_EXIT_CANCELLED);
}

#if defined(_WIN32)
static BOOL WINAPI on_console_ctrl(DWORD type)
{
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        note_interrupt();
        return TRUE;
    }
    return FALSE;
}
static void install_interrupt_handler(void) { SetConsoleCtrlHandler(on_console_ctrl, TRUE); }
#else
static void on_signal(int sig) { (void)sig; note_interrupt(); }
static void install_interrupt_handler(void)
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
}
#endif

static int interrupted(void) { return g_interrupts > 0; }

/* ---- logging: pin_logf() lines on stderr, debug ones only with --debug ---- */

static int g_debug;

static void log_sink(pin_log_level_t level, const char *msg, void *user)
{
    (void)user;
    if (level == PIN_LOG_DEBUG && !g_debug)
        return;
    fwrite(msg, 1, strlen(msg), stderr);
}

/* ---- dry run ---- */

static void dry_status(void *u, const char *line)
{
    (void)u;
    fprintf(stderr, "%s\n", line);
    fflush(stderr);
}

static int dry_interrupted(void *u) { (void)u; return interrupted(); }

static int dry_run(const pout_script_t *sc, pout_tape_t *tape, char *err, size_t err_cap)
{
    pout_out_t *out = pout_out_file_create(sc->dry_run, err, err_cap);
    if (!out)
        return POUT_EXIT_DEVICE;
    pout_session_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.tape = tape;
    cfg.sc = sc;
    cfg.out = out;
    cfg.realtime = sc->realtime;
    cfg.hooks.interrupted = dry_interrupted;
    cfg.hooks.status = dry_status;
    pout_session_t *s = pout_session_create(&cfg, err, err_cap);
    if (!s) {
        out->destroy(out);
        return POUT_EXIT_DEVICE;
    }
    int code = pout_session_run(s, err, err_cap);
    int nf = pout_out_file_count(out);
    if (nf > 1) {
        fprintf(stderr, "%lu frames sent to %d files:", pout_session_frames_sent(s), nf);
        for (int i = 0; i < nf; i++)
            fprintf(stderr, "%s %s", i ? "," : "", pout_out_file_name(out, i));
        fprintf(stderr, "\n");
    } else {
        fprintf(stderr, "%lu frames sent to %s\n", pout_session_frames_sent(s), sc->dry_run);
    }
    pout_session_destroy(s);
    out->destroy(out);
    return code;
}

int main(int argc, char **argv)
{
    char err[512] = "";
    pout_script_t *sc = NULL;

    pin_status_t st = pout_script_parse(argc - 1, (const char *const *)argv + 1, &sc, err, sizeof(err));
    if (st != PIN_OK) {
        fprintf(stderr, "error: %s\nRun MarvinPlayout --help for usage.\n", err[0] ? err : "bad arguments");
        return POUT_EXIT_USAGE;
    }
    if (sc->help) {
        fputs(pout_script_help_text(), stdout);
        pout_script_destroy(sc);
        return POUT_EXIT_OK;
    }
    g_debug = sc->debug;
    pin_log_set_sink(log_sink, NULL);

    if (sc->nfiles == 0) {
        fprintf(stderr, "error: no tape file given.\nRun MarvinPlayout --help for usage.\n");
        pout_script_destroy(sc);
        return POUT_EXIT_USAGE;
    }
    if (!sc->has_dry_run && !sc->virtual_deck && sc->nactions == 0) {
        fprintf(stderr, "error: --no-virtual-deck without any action: nothing would ever play.\n");
        pout_script_destroy(sc);
        return POUT_EXIT_USAGE;
    }

    for (int i = 0; i < sc->nswitches; i++)
        if (pout_switch_set(sc->switches[i], err, sizeof(err)) != 0) {
            fprintf(stderr, "error: %s\n", err);
            pout_script_destroy(sc);
            return POUT_EXIT_USAGE;
        }

    pout_tape_t *tape = NULL;
    if (pout_tape_open(&tape, (const char *const *)sc->files, sc->nfiles, err, sizeof(err)) != 0) {
        fprintf(stderr, "error: %s\n", err);
        pout_script_destroy(sc);
        return POUT_EXIT_USAGE;
    }
    char desc[200];
    pout_tape_describe(tape, desc, sizeof(desc));
    fprintf(stderr, "Tape: %s\n", desc);

    install_interrupt_handler();
    err[0] = 0;
    int code;
    if (sc->has_dry_run) {
        code = dry_run(sc, tape, err, sizeof(err));
    } else {
        code = pout_hw_run(sc, tape, interrupted, err, sizeof(err));
    }
    if (code == POUT_EXIT_CANCELLED)
        fprintf(stderr, "Cancelled.\n");
    else if (code != POUT_EXIT_OK)
        fprintf(stderr, "error: %s\n", err[0] ? err : "failed");
    pout_tape_close(tape);
    pout_script_destroy(sc);
    return code;
}
