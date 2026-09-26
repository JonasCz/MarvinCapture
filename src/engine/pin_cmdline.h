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
 * Command-line parsing shared by every GUI and by pinctl, per the plan's
 * "Changes after your review": parsed in the core so every front end gets
 * it for free (engine/pin_cmdline.c), instead of each GUI re-implementing
 * its own flag parsing.
 *
 * Two independent things are parsed:
 *   - Presets: one-shot capture settings (--device, --input, --output,
 *     --format, --title, --std, --split, --passes, --idle-min, --aspect),
 *     optionally seeded from an INI file (--preset file.ini, loaded via
 *     pin_settings.h) with any explicit flag on the command line
 *     overriding that file's value for the same setting.
 *   - Actions: an ordered sequence for a core action sequencer
 *     (pin_session_run_actions, added alongside the session engine) to
 *     run, e.g. --actions rewind,play,capture, plus --exit-when-done.
 */

#ifndef PIN_CMDLINE_H
#define PIN_CMDLINE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PIN_CMDLINE_INPUT_UNSET = 0,
    PIN_CMDLINE_INPUT_DV,
    PIN_CMDLINE_INPUT_SVIDEO,
    PIN_CMDLINE_INPUT_COMPOSITE,
} pin_cmdline_input_t;

typedef enum {
    PIN_CMDLINE_ASPECT_AUTO = 0,
    PIN_CMDLINE_ASPECT_4_3,
    PIN_CMDLINE_ASPECT_16_9,
} pin_cmdline_aspect_t;

typedef enum {
    PIN_ACTION_REWIND = 0,
    PIN_ACTION_PLAY,
    PIN_ACTION_STOP,
    PIN_ACTION_CAPTURE,
    PIN_ACTION_WAIT_EOT,
} pin_cmdline_action_t;

#define PIN_CMDLINE_MAX_ACTIONS 32
#define PIN_CMDLINE_STR 512

typedef struct {
    char device[PIN_CMDLINE_STR];   /* an ID string, or "first" */
    int device_set;

    pin_cmdline_input_t input;
    int input_set;

    char output[PIN_CMDLINE_STR];
    int output_set;

    char format[PIN_CMDLINE_STR];
    int format_set;

    char title[PIN_CMDLINE_STR];
    int title_set;

    char std[PIN_CMDLINE_STR]; /* e.g. "pal", "ntsc", "auto" */
    int std_set;

    int split;
    int split_set;

    unsigned passes;
    int passes_set;

    unsigned idle_min;
    int idle_min_set;

    pin_cmdline_aspect_t aspect;
    int aspect_set;

    char preset_file[PIN_CMDLINE_STR];
    int preset_file_set;

    pin_cmdline_action_t actions[PIN_CMDLINE_MAX_ACTIONS];
    unsigned num_actions;

    int exit_when_done;

    int help_requested; /* --help was given; caller should print help and exit */
} pin_cmdline_opts_t;

/* device = "first", input = PIN_CMDLINE_INPUT_UNSET, passes = 1, all other
 * fields zero/empty/unset. */
void pin_cmdline_defaults(pin_cmdline_opts_t *opts);

/* Parses argv[0..argc-1] (a full argv including the program name at index
 * 0, which is skipped) into *opts, which must already hold whatever
 * defaults the caller wants (pin_cmdline_defaults() is a reasonable
 * starting point). If --preset <file> is present, that INI file's values
 * (see pin_settings.h; keys match the flag names: device, input, output,
 * format, title, std, split, passes, idle_min, aspect, all in section
 * "Preset") are applied first, then every flag actually given on the
 * command line overrides them. Returns 0 on success, -1 on a bad flag or
 * value, with a human-readable message written into err_buf (safe to pass
 * NULL/0 to ignore it). */
int pin_cmdline_parse(int argc, char **argv, pin_cmdline_opts_t *opts, char *err_buf,
                       size_t err_buf_size);

/* Parses a comma-separated action list ("rewind,play,capture") into
 * out_actions (room for max_actions) and sets *num_actions. Returns 0 on
 * success, -1 on an unrecognised action name (err_buf gets why). */
int pin_cmdline_parse_actions(const char *csv, pin_cmdline_action_t *out_actions,
                               unsigned max_actions, unsigned *num_actions, char *err_buf,
                               size_t err_buf_size);

/* Fills buf with --help text listing every flag and action. Returns 0 on
 * success, -1 if buf_size was too small (buf is left unmodified). */
int pin_cmdline_help(char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif /* PIN_CMDLINE_H */
