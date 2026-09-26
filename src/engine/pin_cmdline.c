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

#include "pin_cmdline.h"
#include "pin_settings.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* strtok_r isn't in strict C11 (it's POSIX); strtok_s is C11 Annex K,
 * which not every libc implements either (MSYS2's ucrt does, glibc
 * doesn't). A tiny hand-rolled version avoids the portability maze. */
static char *strtok_r_compat(char *str, const char *delim, char **saveptr);

static void seterr(char *err_buf, size_t err_buf_size, const char *fmt, ...)
{
    if (!err_buf || err_buf_size == 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err_buf, err_buf_size, fmt, ap);
    va_end(ap);
}

void pin_cmdline_defaults(pin_cmdline_opts_t *opts)
{
    memset(opts, 0, sizeof(*opts));
    snprintf(opts->device, sizeof(opts->device), "first");
    opts->input = PIN_CMDLINE_INPUT_UNSET;
    opts->passes = 1;
    opts->aspect = PIN_CMDLINE_ASPECT_AUTO;
}

static int parse_input(const char *s, pin_cmdline_input_t *out)
{
    if (strcmp(s, "dv") == 0) { *out = PIN_CMDLINE_INPUT_DV; return 0; }
    if (strcmp(s, "svideo") == 0) { *out = PIN_CMDLINE_INPUT_SVIDEO; return 0; }
    if (strcmp(s, "composite") == 0) { *out = PIN_CMDLINE_INPUT_COMPOSITE; return 0; }
    return -1;
}

static int parse_aspect(const char *s, pin_cmdline_aspect_t *out)
{
    if (strcmp(s, "auto") == 0) { *out = PIN_CMDLINE_ASPECT_AUTO; return 0; }
    if (strcmp(s, "4:3") == 0) { *out = PIN_CMDLINE_ASPECT_4_3; return 0; }
    if (strcmp(s, "16:9") == 0) { *out = PIN_CMDLINE_ASPECT_16_9; return 0; }
    return -1;
}

static int parse_bool_flagword(const char *s, int *out)
{
    if (strcmp(s, "1") == 0 || strcmp(s, "true") == 0 || strcmp(s, "yes") == 0) { *out = 1; return 0; }
    if (strcmp(s, "0") == 0 || strcmp(s, "false") == 0 || strcmp(s, "no") == 0) { *out = 0; return 0; }
    return -1;
}

/* --device <value>: "first", a device id ("usb:1-8"), a device serial (16
 * hex chars, matched case-insensitively -- resolved later, against the live
 * device list, by pin_session.c's resolve_device_id()), or a path to an
 * existing file. This hardware-free module has no session/replay state of
 * its own to register a file with, so it only detects the file (a plain
 * fopen, like every other existing-file check in this codebase) and leaves
 * opts->device_replay_path for the caller to act on. */
static void set_device(pin_cmdline_opts_t *opts, const char *value)
{
    opts->device_is_replay_file = 0;
    opts->device_replay_path[0] = '\0';
    FILE *f = fopen(value, "rb");
    if (f) {
        fclose(f);
        const char *base = value;
        for (const char *p = value; *p; p++)
            if (*p == '/' || *p == '\\')
                base = p + 1;
        snprintf(opts->device, sizeof(opts->device), "replay:%s", base);
        snprintf(opts->device_replay_path, sizeof(opts->device_replay_path), "%s", value);
        opts->device_is_replay_file = 1;
    } else {
        snprintf(opts->device, sizeof(opts->device), "%s", value);
    }
}

static int parse_uint(const char *s, unsigned *out)
{
    if (!*s)
        return -1;
    char *end;
    long v = strtol(s, &end, 10);
    if (*end != '\0' || v < 0)
        return -1;
    *out = (unsigned)v;
    return 0;
}

int pin_cmdline_parse_actions(const char *csv, pin_cmdline_action_t *out_actions,
                               unsigned max_actions, unsigned *num_actions, char *err_buf,
                               size_t err_buf_size)
{
    *num_actions = 0;
    char buf[2048];
    snprintf(buf, sizeof(buf), "%s", csv);

    char *save = NULL;
    char *tok = strtok_r_compat(buf, ",", &save);
    while (tok) {
        pin_cmdline_action_t a;
        if (strcmp(tok, "rewind") == 0) a = PIN_ACTION_REWIND;
        else if (strcmp(tok, "play") == 0) a = PIN_ACTION_PLAY;
        else if (strcmp(tok, "stop") == 0) a = PIN_ACTION_STOP;
        else if (strcmp(tok, "capture") == 0) a = PIN_ACTION_CAPTURE;
        else if (strcmp(tok, "wait-eot") == 0) a = PIN_ACTION_WAIT_EOT;
        else {
            seterr(err_buf, err_buf_size, "unknown action \"%s\" (expected one of "
                   "rewind, play, stop, capture, wait-eot)", tok);
            return -1;
        }
        if (*num_actions >= max_actions) {
            seterr(err_buf, err_buf_size, "too many actions (max %u)", max_actions);
            return -1;
        }
        out_actions[(*num_actions)++] = a;
        tok = strtok_r_compat(NULL, ",", &save);
    }
    return 0;
}

static char *strtok_r_compat(char *str, const char *delim, char **saveptr)
{
    char *s = str ? str : *saveptr;
    if (!s)
        return NULL;
    while (*s && strchr(delim, *s))
        s++;
    if (!*s) {
        *saveptr = NULL;
        return NULL;
    }
    char *start = s;
    while (*s && !strchr(delim, *s))
        s++;
    if (*s) {
        *s = '\0';
        *saveptr = s + 1;
    } else {
        *saveptr = NULL;
    }
    return start;
}

static void load_preset_file(const char *path, pin_cmdline_opts_t *opts)
{
    pin_settings_t s;
    pin_settings_init(&s);
    if (pin_settings_load(&s, path) != 0) {
        pin_settings_free(&s);
        return;
    }

    const char *v;
    if (!opts->device_set && (v = pin_settings_get_string(&s, "Preset", "device", NULL))) {
        set_device(opts, v);
    }
    if (!opts->input_set && (v = pin_settings_get_string(&s, "Preset", "input", NULL))) {
        pin_cmdline_input_t in;
        if (parse_input(v, &in) == 0)
            opts->input = in;
    }
    if (!opts->output_set && (v = pin_settings_get_string(&s, "Preset", "output", NULL)))
        snprintf(opts->output, sizeof(opts->output), "%s", v);
    if (!opts->format_set && (v = pin_settings_get_string(&s, "Preset", "format", NULL)))
        snprintf(opts->format, sizeof(opts->format), "%s", v);
    if (!opts->title_set && (v = pin_settings_get_string(&s, "Preset", "title", NULL)))
        snprintf(opts->title, sizeof(opts->title), "%s", v);
    if (!opts->std_set && (v = pin_settings_get_string(&s, "Preset", "std", NULL)))
        snprintf(opts->std, sizeof(opts->std), "%s", v);
    if (!opts->split_set)
        opts->split = pin_settings_get_bool(&s, "Preset", "split", opts->split);
    if (!opts->passes_set)
        opts->passes = (unsigned)pin_settings_get_int(&s, "Preset", "passes", (int)opts->passes);
    if (!opts->idle_min_set)
        opts->idle_min =
            (unsigned)pin_settings_get_int(&s, "Preset", "idle_min", (int)opts->idle_min);
    if (!opts->aspect_set && (v = pin_settings_get_string(&s, "Preset", "aspect", NULL))) {
        pin_cmdline_aspect_t asp;
        if (parse_aspect(v, &asp) == 0)
            opts->aspect = asp;
    }

    pin_settings_free(&s);
}

int pin_cmdline_parse(int argc, char **argv, pin_cmdline_opts_t *opts, char *err_buf,
                       size_t err_buf_size)
{
    if (err_buf && err_buf_size)
        err_buf[0] = '\0';

    /* First pass: find --preset so it can seed values before the explicit
     * flags (which must win) are applied below. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--preset") == 0 && i + 1 < argc) {
            snprintf(opts->preset_file, sizeof(opts->preset_file), "%s", argv[i + 1]);
            opts->preset_file_set = 1;
            load_preset_file(opts->preset_file, opts);
            break;
        }
    }

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;

#define NEED_VALUE()                                                         \
    do {                                                                     \
        if (!val) {                                                          \
            seterr(err_buf, err_buf_size, "%s requires a value", a);         \
            return -1;                                                      \
        }                                                                    \
    } while (0)

        if (strcmp(a, "--help") == 0) {
            opts->help_requested = 1;
        } else if (strcmp(a, "--preset") == 0) {
            NEED_VALUE();
            i++; /* already applied above */
        } else if (strcmp(a, "--device") == 0) {
            NEED_VALUE();
            set_device(opts, val);
            opts->device_set = 1;
            i++;
        } else if (strcmp(a, "--input") == 0) {
            NEED_VALUE();
            pin_cmdline_input_t in;
            if (parse_input(val, &in) != 0) {
                seterr(err_buf, err_buf_size,
                       "--input must be dv, svideo or composite (got \"%s\")", val);
                return -1;
            }
            opts->input = in;
            opts->input_set = 1;
            i++;
        } else if (strcmp(a, "--output") == 0) {
            NEED_VALUE();
            snprintf(opts->output, sizeof(opts->output), "%s", val);
            opts->output_set = 1;
            i++;
        } else if (strcmp(a, "--format") == 0) {
            NEED_VALUE();
            snprintf(opts->format, sizeof(opts->format), "%s", val);
            opts->format_set = 1;
            i++;
        } else if (strcmp(a, "--title") == 0) {
            NEED_VALUE();
            snprintf(opts->title, sizeof(opts->title), "%s", val);
            opts->title_set = 1;
            i++;
        } else if (strcmp(a, "--std") == 0) {
            NEED_VALUE();
            snprintf(opts->std, sizeof(opts->std), "%s", val);
            opts->std_set = 1;
            i++;
        } else if (strcmp(a, "--split") == 0) {
            if (val && parse_bool_flagword(val, &opts->split) == 0) {
                i++;
            } else {
                opts->split = 1; /* bare --split means "on" */
            }
            opts->split_set = 1;
        } else if (strcmp(a, "--passes") == 0) {
            NEED_VALUE();
            if (parse_uint(val, &opts->passes) != 0) {
                seterr(err_buf, err_buf_size, "--passes must be a non-negative integer (got \"%s\")",
                       val);
                return -1;
            }
            opts->passes_set = 1;
            i++;
        } else if (strcmp(a, "--idle-min") == 0) {
            NEED_VALUE();
            if (parse_uint(val, &opts->idle_min) != 0) {
                seterr(err_buf, err_buf_size,
                       "--idle-min must be a non-negative integer (got \"%s\")", val);
                return -1;
            }
            opts->idle_min_set = 1;
            i++;
        } else if (strcmp(a, "--aspect") == 0) {
            NEED_VALUE();
            pin_cmdline_aspect_t asp;
            if (parse_aspect(val, &asp) != 0) {
                seterr(err_buf, err_buf_size, "--aspect must be auto, 4:3 or 16:9 (got \"%s\")",
                       val);
                return -1;
            }
            opts->aspect = asp;
            opts->aspect_set = 1;
            i++;
        } else if (strcmp(a, "--actions") == 0) {
            NEED_VALUE();
            char actions_err[256];
            if (pin_cmdline_parse_actions(val, opts->actions, PIN_CMDLINE_MAX_ACTIONS,
                                           &opts->num_actions, actions_err,
                                           sizeof(actions_err)) != 0) {
                seterr(err_buf, err_buf_size, "--actions: %s", actions_err);
                return -1;
            }
            i++;
        } else if (strcmp(a, "--exit-when-done") == 0) {
            opts->exit_when_done = 1;
        } else {
            seterr(err_buf, err_buf_size, "unrecognised argument \"%s\"", a);
            return -1;
        }
#undef NEED_VALUE
    }

    return 0;
}

int pin_cmdline_help(char *buf, size_t buf_size)
{
    static const char *help_text =
        "Presets:\n"
        "  --device <value>         \"first\", a device id (e.g. usb:1-8), a device\n"
        "                           serial (16 hex chars), or a path to an existing\n"
        "                           file to replay (default: first)\n"
        "  --input <dv|svideo|composite>\n"
        "  --output <path>\n"
        "  --format <key>           e.g. dv, dv-avi, dv-mov, hdv-ts, hdv-mov, avi, ffv1-mkv\n"
        "  --title <text>\n"
        "  --std <name>             analog video standard, or auto\n"
        "  --split [1|0]            scene split on/off (bare flag = on)\n"
        "  --passes <N>\n"
        "  --idle-min <N>           stop after N minutes with no data\n"
        "  --aspect <auto|4:3|16:9>\n"
        "  --preset <file.ini>      seed the above from an INI file; explicit\n"
        "                           flags on the command line still win\n"
        "\n"
        "Actions:\n"
        "  --actions <a,b,c>        rewind, play, stop, capture, wait-eot\n"
        "  --exit-when-done         quit once the action sequence finishes\n"
        "\n"
        "  --help                   print this text\n";
    size_t n = strlen(help_text);
    if (n + 1 > buf_size)
        return -1;
    memcpy(buf, help_text, n + 1);
    return 0;
}
