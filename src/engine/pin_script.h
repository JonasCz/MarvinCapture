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
 * The command-line language of docs/cli.md: a left-to-right list of settings
 * and actions ("--rew --wait --play --capture tape.avi --wait idle ..."),
 * parsed into a script that pin_script_run.c (the sequencer) executes.
 *
 * This file is the hardware-free half: the parser (pin_script_parse_args),
 * timecode/duration parsing, the extension -> format table and the help text.
 * pin_script_eval.[ch] holds the wait-condition evaluation. pin_api.c wraps
 * these as the public pin_script_* functions (pin_api.h).
 */

#ifndef PIN_SCRIPT_H
#define PIN_SCRIPT_H

#include "../api/pin_api.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PIN_SCRIPT_MAX_CONDS 8

/* HH:MM:SS:FF. sep is the character before FF (':' or ';' for drop-frame). */
typedef struct {
    int h, m, s, f;
    char sep;
} pin_tc_t;

/* Parses "HH:MM:SS:FF" (two digits each; minutes/seconds 0..59, frames 0..29;
 * ';' or ':' before the frames). Returns 1 on success, else 0 and, if why is
 * not NULL, a short reason. */
int pin_tc_parse(const char *s, pin_tc_t *out, char *why, size_t why_cap);
/* "HH:MM:SS:FF" with the original separator. */
void pin_tc_format(const pin_tc_t *tc, char *out, size_t cap);
/* <0, 0, >0 like strcmp, field by field (h, m, s, f). */
int pin_tc_compare(const pin_tc_t *a, const pin_tc_t *b);
/* Length in seconds; fps <= 0 means 25. */
double pin_tc_seconds(const pin_tc_t *tc, double fps);

typedef enum {
    PIN_COND_IDLE = 0,
    PIN_COND_NOSIGNAL,      /* tc = how long (duration) */
    PIN_COND_SIGNAL,
    PIN_COND_TIMECODE,      /* tc = the deck timecode to reach */
    PIN_COND_DURATION,      /* tc = how much time has to pass */
} pin_cond_kind_t;

typedef struct {
    pin_cond_kind_t kind;
    pin_tc_t tc;
} pin_script_cond_t;

/* Parses one wait condition (see docs/cli.md). */
int pin_script_cond_parse(const char *tok, pin_script_cond_t *out, char *why, size_t why_cap);
/* Canonical text: "idle", "nosignal=+00:01:00:00", "signal", "00:14:30:00", "+00:00:30:00". */
void pin_script_cond_text(const pin_script_cond_t *c, char *out, size_t cap);

/* File extension -> formats. out[] is indexed by pin_kind_t (analog, DV, HDV);
 * a kind the extension cannot be used for gets its default format and its bit
 * set in *unsupported_mask (1 << kind). Returns 0 if the extension is not one
 * of dv avi mov ts m2t mkv (outputs untouched), else 1. ext is without the dot. */
int pin_script_ext_formats(const char *ext, pin_format_t out[3], unsigned *unsupported_mask);
/* Copies path to base without its extension if it is one of the known ones;
 * returns the extension (without the dot, lower case in ext_out) or "". */
void pin_script_split_ext(const char *path, char *base, size_t base_cap, char *ext_out, size_t ext_cap);

typedef enum {
    PIN_SSTEP_REW = 0, PIN_SSTEP_FF, PIN_SSTEP_PLAY, PIN_SSTEP_PAUSE, PIN_SSTEP_STOP,
    PIN_SSTEP_CAPTURE, PIN_SSTEP_WAIT,
} pin_sstep_kind_t;

typedef enum {
    PIN_SITEM_STEP = 0,     /* an action */
    PIN_SITEM_INPUT,
    PIN_SITEM_STD,
    PIN_SITEM_CONTROL,
} pin_sitem_kind_t;

typedef struct {
    char path[PIN_PATH_MAX];    /* exactly as given ("-" = stdout) */
    char base[PIN_PATH_MAX];    /* without a recognised extension */
    int to_stdout;
    pin_format_t format[3];     /* by pin_kind_t */
    unsigned warn_mask;         /* kinds whose format fell back to the default because
                                   the extension does not suit them (1 << kind) */
    pin_aspect_t aspect;
    int split;
    char title[PIN_TEXT_MAX];
    int keep_raw;
    int overwrite;
} pin_script_capture_t;

typedef struct {
    pin_sitem_kind_t kind;
    int step;                   /* index among the actions, -1 for settings */
    pin_input_t input;          /* INPUT */
    pin_std_t std;              /* STD */
    pin_control_t ctl;          /* CONTROL */
    int32_t value;              /* CONTROL */
    pin_sstep_kind_t step_kind; /* STEP */
    pin_script_capture_t cap;   /* STEP: CAPTURE */
    int nconds;                 /* STEP: WAIT */
    pin_script_cond_t conds[PIN_SCRIPT_MAX_CONDS];
} pin_script_item_t;

struct pin_script {
    char device[PIN_PATH_MAX];
    int has_device;
    int help;
    int debug;
    pin_script_settings_t initial;  /* settings given before the first action (all of them if none) */
    pin_script_item_t *items;
    int nitems, cap_items;
    int nsteps;
};

/* Parses argv (no program name). On a usage error returns PIN_ERR_ARG with a
 * one-line message naming the offending argument. */
pin_status_t pin_script_parse_args(int argc, const char *const *argv, pin_script_t **out,
                                   char *err, size_t err_cap);
void pin_script_destroy(pin_script_t *sc);
pin_script_t *pin_script_clone(const pin_script_t *sc);
/* Text of action `step` ("rew", "capture tape01.avi", "wait idle,nosignal=+00:01:00:00"). */
pin_status_t pin_script_step_description(const pin_script_t *sc, int step, char *out, size_t cap);
const char *pin_script_help_text(void);

#ifdef __cplusplus
}
#endif

#endif /* PIN_SCRIPT_H */
