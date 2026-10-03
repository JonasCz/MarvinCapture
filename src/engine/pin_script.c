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

#include "pin_script.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- small helpers -------------------------------------------------------- */

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    return *a == *b;
}

static void why_set(char *why, size_t cap, const char *msg)
{
    if (why && cap)
        snprintf(why, cap, "%s", msg);
}

/* ---- timecodes and durations ------------------------------------------------ */

int pin_tc_parse(const char *s, pin_tc_t *out, char *why, size_t why_cap)
{
    static const char *shape = "malformed (expected HH:MM:SS:FF)";
    if (!s || strlen(s) != 11) {
        why_set(why, why_cap, shape);
        return 0;
    }
    for (int i = 0; i < 11; i++) {
        int sep = i == 2 || i == 5 || i == 8;
        if (sep ? !(s[i] == ':' || (i == 8 && s[i] == ';')) : !isdigit((unsigned char)s[i])) {
            why_set(why, why_cap, shape);
            return 0;
        }
    }
    pin_tc_t t;
    t.h = (s[0] - '0') * 10 + (s[1] - '0');
    t.m = (s[3] - '0') * 10 + (s[4] - '0');
    t.s = (s[6] - '0') * 10 + (s[7] - '0');
    t.f = (s[9] - '0') * 10 + (s[10] - '0');
    t.sep = s[8];
    if (t.m > 59) { why_set(why, why_cap, "minutes must be 0..59"); return 0; }
    if (t.s > 59) { why_set(why, why_cap, "seconds must be 0..59"); return 0; }
    if (t.f > 29) { why_set(why, why_cap, "frames must be 0..29"); return 0; }
    if (out)
        *out = t;
    return 1;
}

void pin_tc_format(const pin_tc_t *tc, char *out, size_t cap)
{
    snprintf(out, cap, "%02d:%02d:%02d%c%02d", tc->h, tc->m, tc->s, tc->sep ? tc->sep : ':', tc->f);
}

int pin_tc_compare(const pin_tc_t *a, const pin_tc_t *b)
{
    if (a->h != b->h) return a->h < b->h ? -1 : 1;
    if (a->m != b->m) return a->m < b->m ? -1 : 1;
    if (a->s != b->s) return a->s < b->s ? -1 : 1;
    if (a->f != b->f) return a->f < b->f ? -1 : 1;
    return 0;
}

double pin_tc_seconds(const pin_tc_t *tc, double fps)
{
    if (fps <= 0)
        fps = 25.0;
    return tc->h * 3600.0 + tc->m * 60.0 + tc->s + tc->f / fps;
}

/* ---- wait conditions ---------------------------------------------------------- */

static const char *k_default_nosignal = "+00:01:00:00";

int pin_script_cond_parse(const char *tok, pin_script_cond_t *out, char *why, size_t why_cap)
{
    pin_script_cond_t c;
    memset(&c, 0, sizeof(c));
    char inner[64];
    if (!tok || !*tok) {
        why_set(why, why_cap, "empty condition");
        return 0;
    }
    if (strcmp(tok, "idle") == 0) {
        c.kind = PIN_COND_IDLE;
    } else if (strcmp(tok, "signal") == 0) {
        c.kind = PIN_COND_SIGNAL;
    } else if (strncmp(tok, "nosignal", 8) == 0 && (tok[8] == 0 || tok[8] == '=')) {
        c.kind = PIN_COND_NOSIGNAL;
        const char *d = tok[8] == '=' ? tok + 9 : k_default_nosignal;
        if (*d != '+') {
            why_set(why, why_cap, "nosignal= needs a duration with a leading + (+HH:MM:SS:FF)");
            return 0;
        }
        if (!pin_tc_parse(d + 1, &c.tc, inner, sizeof(inner))) {
            if (why && why_cap)
                snprintf(why, why_cap, "bad nosignal duration: %s", inner);
            return 0;
        }
    } else if (tok[0] == '+') {
        c.kind = PIN_COND_DURATION;
        if (!pin_tc_parse(tok + 1, &c.tc, inner, sizeof(inner))) {
            if (why && why_cap)
                snprintf(why, why_cap, "bad duration: %s", inner);
            return 0;
        }
    } else if (isdigit((unsigned char)tok[0])) {
        c.kind = PIN_COND_TIMECODE;
        if (!pin_tc_parse(tok, &c.tc, inner, sizeof(inner))) {
            if (why && why_cap)
                snprintf(why, why_cap, "bad timecode: %s", inner);
            return 0;
        }
    } else {
        why_set(why, why_cap, "expected idle, signal, nosignal[=+HH:MM:SS:FF], HH:MM:SS:FF or +HH:MM:SS:FF");
        return 0;
    }
    if (out)
        *out = c;
    return 1;
}

void pin_script_cond_text(const pin_script_cond_t *c, char *out, size_t cap)
{
    char tc[16];
    pin_tc_format(&c->tc, tc, sizeof(tc));
    switch (c->kind) {
    case PIN_COND_IDLE: snprintf(out, cap, "idle"); break;
    case PIN_COND_SIGNAL: snprintf(out, cap, "signal"); break;
    case PIN_COND_NOSIGNAL: snprintf(out, cap, "nosignal=+%s", tc); break;
    case PIN_COND_TIMECODE: snprintf(out, cap, "%s", tc); break;
    case PIN_COND_DURATION: snprintf(out, cap, "+%s", tc); break;
    }
}

/* ---- extensions and formats ------------------------------------------------------ */

static const pin_format_t k_default_format[3] = { PIN_FMT_ANALOG_AVI, PIN_FMT_DV_RAW, PIN_FMT_HDV_TS };

int pin_script_ext_formats(const char *ext, pin_format_t out[3], unsigned *unsupported_mask)
{
    /* analog, DV, HDV; PIN_FMT_COUNT = not possible */
    static const struct { const char *ext; pin_format_t f[3]; } tbl[] = {
        { "dv",  { PIN_FMT_COUNT, PIN_FMT_DV_RAW, PIN_FMT_COUNT } },
        { "avi", { PIN_FMT_ANALOG_AVI, PIN_FMT_DV_AVI, PIN_FMT_COUNT } },
        { "mov", { PIN_FMT_COUNT, PIN_FMT_DV_MOV, PIN_FMT_HDV_MOV } },
        { "ts",  { PIN_FMT_COUNT, PIN_FMT_COUNT, PIN_FMT_HDV_TS } },
        { "m2t", { PIN_FMT_COUNT, PIN_FMT_COUNT, PIN_FMT_HDV_TS } },
        { "mkv", { PIN_FMT_ANALOG_FFV1_MKV, PIN_FMT_COUNT, PIN_FMT_HDV_MKV } },
    };
    if (!ext)
        return 0;
    for (unsigned i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
        if (!ieq(ext, tbl[i].ext))
            continue;
        unsigned mask = 0;
        for (int k = 0; k < 3; k++) {
            if (tbl[i].f[k] == PIN_FMT_COUNT) {
                out[k] = k_default_format[k];
                mask |= 1u << k;
            } else {
                out[k] = tbl[i].f[k];
            }
        }
        if (unsupported_mask)
            *unsupported_mask = mask;
        return 1;
    }
    return 0;
}

void pin_script_split_ext(const char *path, char *base, size_t base_cap, char *ext_out, size_t ext_cap)
{
    snprintf(base, base_cap, "%s", path);
    if (ext_out && ext_cap)
        ext_out[0] = 0;
    const char *dot = strrchr(path, '.');
    const char *s1 = strrchr(path, '/'), *s2 = strrchr(path, '\\');
    const char *sep = s1 && s2 ? (s1 > s2 ? s1 : s2) : (s1 ? s1 : s2);
    if (!dot || (sep && dot < sep) || dot == path)
        return;
    pin_format_t tmp[3];
    if (!pin_script_ext_formats(dot + 1, tmp, NULL))
        return;
    size_t n = (size_t)(dot - path);
    if (n >= base_cap)
        n = base_cap - 1;
    base[n] = 0;
    if (ext_out && ext_cap) {
        snprintf(ext_out, ext_cap, "%s", dot + 1);
        for (char *p = ext_out; *p; p++)
            *p = (char)tolower((unsigned char)*p);
    }
}

/* ---- help ------------------------------------------------------------------------ */

const char *pin_script_help_text(void)
{
    return
        "Usage: <program> [settings and actions...]\n"
        "\n"
        "Arguments are processed left to right. Settings change what the steps\n"
        "after them do; actions each do one thing. An action's argument is the\n"
        "next word unless it starts with --; --wait=idle also works.\n"
        "\n"
        "Settings:\n"
        "  -d, --device ID          device id (usb:2-1), 16-hex serial, or a .dv/.ts\n"
        "                           file to replay (default: first device). Only\n"
        "                           before the first action.\n"
        "  -i, --input dv|svideo|composite\n"
        "                           dv = DV and HDV over FireWire (default)\n"
        "  --std S                  analog standard: auto pal ntsc pal-m pal-n pal-60\n"
        "                           ntsc-443 ntsc-j secam (default auto)\n"
        "  --format KEY             dv dv-avi dv-mov hdv-ts hdv-mov hdv-mkv avi\n"
        "                           ffv1-mkv; default from the file extension; may be\n"
        "                           given once per kind\n"
        "  --aspect auto|4:3|16:9   (default auto)\n"
        "  --split                  scene split (DV/HDV)\n"
        "  --title T                metadata title, where the format supports it\n"
        "  --keep-raw               keep the raw .dv/.ts next to a rewrapped file\n"
        "  --overwrite              overwrite an existing target file (else an error)\n"
        "  --brightness --contrast --saturation --hue --sharpness --audio-gain N\n"
        "                           analog controls (default: the device's)\n"
        "  --debug                  status as one plain line per second, plus debug\n"
        "                           logging (raw AV/C traffic, bring-up details)\n"
        "  Settings cannot change while a capture is open (from --capture until\n"
        "  the next transport action, --capture or the end of the arguments).\n"
        "\n"
        "Actions:\n"
        "  --rew --ff --play --pause --stop\n"
        "                           transport command; returns as soon as the deck\n"
        "                           accepted it. Closes an open capture first (the\n"
        "                           tape keeps running); --stop also stops the tape.\n"
        "                           DV/HDV input only.\n"
        "  --capture PATH           start capturing to PATH; the file format comes\n"
        "                           from the extension (.dv .avi .mov .ts .m2t .mkv).\n"
        "                           It stays open until the next transport action,\n"
        "                           the next --capture or the end of the arguments.\n"
        "                           PATH - writes the stream to stdout (pipe it into\n"
        "                           a program): DV as raw DIF, HDV as MPEG-TS, analog\n"
        "                           as NUT (YUY2 + PCM). --format is ignored; --split\n"
        "                           is refused; once per command line. If the reader\n"
        "                           exits or cannot keep up the capture stops, exit 4.\n"
        "  --wait [COND[,COND...]]  block until the first condition is met; default\n"
        "                           idle\n"
        "\n"
        "Wait conditions:\n"
        "  idle                     the deck was moving after the last transport\n"
        "                           command and is now stopped or paused (stable\n"
        "                           about 3 s); met at once if already idle\n"
        "  nosignal[=+HH:MM:SS:FF]  no signal for that long, counted from the start\n"
        "                           of the wait (default +00:01:00:00)\n"
        "  signal                   a signal is present\n"
        "  HH:MM:SS:FF              the deck timecode reaches or passes this value\n"
        "                           in the direction the tape moves; fails if the\n"
        "                           deck goes idle first\n"
        "  +HH:MM:SS:FF             that much time has passed\n"
        "  Analog inputs only allow signal, nosignal and durations.\n"
        "  Winding for a duration (--rew --wait +00:00:30:00) depends on the deck's\n"
        "  speed and is not precise; do not use it to position the tape. Many decks\n"
        "  report the timecode only while playing or stopped, so timecode waits\n"
        "  are most reliable during --play.\n"
        "\n"
        "Exit codes: 0 ok, 1 usage error, 2 device or bring-up error, 3 deck error,\n"
        "4 capture ended abnormally, 130 cancelled (Ctrl-C).\n"
        "\n"
        "Examples:\n"
        "  --rew --wait --play --capture tape01.avi --wait idle,nosignal --rew --wait\n"
        "  --rew --wait --play --wait 00:14:30:00 --capture clip.dv --wait 00:21:00:00 --stop\n"
        "  -i svideo --std pal --capture vhs.mkv --wait signal --wait nosignal=+00:00:30:00,+04:00:00:00\n"
        "\n"
        "-h, --help, or no arguments at all: this text.\n";
}

/* ---- the parser -------------------------------------------------------------------- */

typedef struct {
    pin_script_t *sc;
    char *err;
    size_t err_cap;
    pin_input_t input;
    pin_format_t fmt[3];
    int fmt_set[3];
    pin_aspect_t aspect;
    int split;
    char title[PIN_TEXT_MAX];
    int keep_raw;
    int overwrite;
    int started;
    int capture_open;
    int stdout_used;            /* a --capture - was seen: only one per script */
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

static pin_script_item_t *item_add(parser_t *p)
{
    pin_script_t *sc = p->sc;
    if (sc->nitems == sc->cap_items) {
        int ncap = sc->cap_items ? sc->cap_items * 2 : 16;
        pin_script_item_t *n = realloc(sc->items, (size_t)ncap * sizeof(*n));
        if (!n)
            return NULL;
        sc->items = n;
        sc->cap_items = ncap;
    }
    pin_script_item_t *it = &sc->items[sc->nitems++];
    memset(it, 0, sizeof(*it));
    it->step = -1;
    return it;
}

static int parse_int(const char *s, int32_t *out)
{
    if (!s || !*s)
        return 0;
    char *end;
    long v = strtol(s, &end, 10);
    if (*end || v < -1000000 || v > 1000000)
        return 0;
    *out = (int32_t)v;
    return 1;
}

static int fmt_kind(pin_format_t f)
{
    switch (f) {
    case PIN_FMT_ANALOG_AVI: case PIN_FMT_ANALOG_FFV1_MKV: return PIN_KIND_ANALOG;
    case PIN_FMT_DV_RAW: case PIN_FMT_DV_AVI: case PIN_FMT_DV_MOV: return PIN_KIND_DV;
    default: return PIN_KIND_HDV;
    }
}

static int fmt_from_key(const char *key, pin_format_t *out)
{
    static const struct { const char *key; pin_format_t f; } tbl[] = {
        { "dv", PIN_FMT_DV_RAW }, { "dv-avi", PIN_FMT_DV_AVI }, { "dv-mov", PIN_FMT_DV_MOV },
        { "hdv-ts", PIN_FMT_HDV_TS }, { "hdv-mov", PIN_FMT_HDV_MOV }, { "hdv-mkv", PIN_FMT_HDV_MKV },
        { "avi", PIN_FMT_ANALOG_AVI }, { "ffv1-mkv", PIN_FMT_ANALOG_FFV1_MKV },
    };
    for (unsigned i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++)
        if (strcmp(key, tbl[i].key) == 0) {
            *out = tbl[i].f;
            return 1;
        }
    return 0;
}

static int std_from_key(const char *s, pin_std_t *out)
{
    static const struct { const char *name; pin_std_t std; } tbl[] = {
        { "auto", PIN_STD_AUTO }, { "pal", PIN_STD_PAL }, { "ntsc", PIN_STD_NTSC },
        { "pal-m", PIN_STD_PAL_M }, { "pal-n", PIN_STD_PAL_N }, { "pal-60", PIN_STD_PAL_60 },
        { "ntsc-443", PIN_STD_NTSC_443 }, { "ntsc443", PIN_STD_NTSC_443 },
        { "ntsc-j", PIN_STD_NTSC_J }, { "secam", PIN_STD_SECAM },
    };
    for (unsigned i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++)
        if (ieq(s, tbl[i].name)) {
            *out = tbl[i].std;
            return 1;
        }
    return 0;
}

static int control_from_name(const char *name, pin_control_t *out)
{
    static const struct { const char *name; pin_control_t c; } tbl[] = {
        { "brightness", PIN_CTL_BRIGHTNESS }, { "contrast", PIN_CTL_CONTRAST },
        { "saturation", PIN_CTL_SATURATION }, { "hue", PIN_CTL_HUE },
        { "sharpness", PIN_CTL_SHARPNESS }, { "audio-gain", PIN_CTL_AUDIO_GAIN },
    };
    for (unsigned i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++)
        if (strcmp(name, tbl[i].name) == 0) {
            *out = tbl[i].c;
            return 1;
        }
    return 0;
}

static int32_t *settings_control_slot(pin_script_settings_t *st, pin_control_t c)
{
    switch (c) {
    case PIN_CTL_BRIGHTNESS: return &st->control_brightness;
    case PIN_CTL_CONTRAST: return &st->control_contrast;
    case PIN_CTL_SATURATION: return &st->control_saturation;
    case PIN_CTL_HUE: return &st->control_hue;
    case PIN_CTL_SHARPNESS: return &st->control_sharpness;
    case PIN_CTL_AUDIO_GAIN: return &st->control_audio_gain;
    default: return NULL;
    }
}

static int is_analog(const parser_t *p) { return p->input != PIN_INPUT_DV; }

pin_status_t pin_script_parse_args(int argc, const char *const *argv, pin_script_t **out,
                                   char *err, size_t err_cap)
{
    if (err && err_cap)
        err[0] = 0;
    if (!out)
        return PIN_ERR_ARG;
    *out = NULL;
    pin_script_t *sc = calloc(1, sizeof(*sc));
    if (!sc)
        return PIN_ERR_NOMEM;
    sc->initial.size = sizeof(sc->initial);

    parser_t p;
    memset(&p, 0, sizeof(p));
    p.sc = sc;
    p.err = err;
    p.err_cap = err_cap;
    p.input = PIN_INPUT_DV;
    for (int k = 0; k < 3; k++)
        p.fmt[k] = k_default_format[k];

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

    pin_status_t rc = PIN_OK;
    for (int i = 0; i < argc && rc == PIN_OK; i++) {
        const char *arg = argv[i];
        char name[64];
        const char *inl = NULL; /* value after '=' */
        if (arg[0] == '-' && arg[1] == '-') {
            const char *eq = strchr(arg, '=');
            size_t n = eq ? (size_t)(eq - arg) : strlen(arg);
            if (n >= sizeof(name))
                return pin_script_destroy(sc), fail(&p, "unknown option \"%s\"", arg);
            memcpy(name, arg, n);
            name[n] = 0;
            inl = eq ? eq + 1 : NULL;
        } else if (arg[0] == '-' && arg[1] && !arg[2]) {
            /* short options */
            const char *long_name = arg[1] == 'd' ? "--device" : arg[1] == 'i' ? "--input" : NULL;
            if (!long_name) {
                pin_script_destroy(sc);
                return fail(&p, "unknown option \"%s\"", arg);
            }
            snprintf(name, sizeof(name), "%s", long_name);
        } else {
            pin_script_destroy(sc);
            return fail(&p, "unexpected argument \"%s\" (a value belongs right after the option that takes it)", arg);
        }

#define FAIL(...) do { rc = fail(&p, __VA_ARGS__); goto parse_end; } while (0)
        /* the value of a setting: after '=' or the next word, whatever it is */
        const char *val = NULL;
        static const char *const value_opts[] = { "--device", "--input", "--std", "--format", "--aspect",
            "--title", "--brightness", "--contrast", "--saturation", "--hue", "--sharpness", "--audio-gain" };
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
        }
        int is_setting = takes_value || strcmp(name, "--split") == 0 || strcmp(name, "--keep-raw") == 0 ||
                         strcmp(name, "--overwrite") == 0;
        if (is_setting && p.capture_open)
            FAIL("%s cannot change while a capture is open (it ends at the next transport action, "
                 "--capture or the end of the arguments)", name);
        int is_flag = strcmp(name, "--split") == 0 || strcmp(name, "--keep-raw") == 0 ||
                      strcmp(name, "--overwrite") == 0 || strcmp(name, "--debug") == 0;
        if (is_flag && inl)
            FAIL("%s takes no value (got \"%s\")", name, inl);

        if (strcmp(name, "--device") == 0) {
            if (p.started)
                FAIL("%s must come before the first action", name);
            if (!*val)
                FAIL("%s needs a value", name);
            snprintf(sc->device, sizeof(sc->device), "%s", val);
            sc->has_device = 1;
        } else if (strcmp(name, "--input") == 0) {
            pin_input_t in;
            if (strcmp(val, "dv") == 0) in = PIN_INPUT_DV;
            else if (strcmp(val, "svideo") == 0) in = PIN_INPUT_SVIDEO;
            else if (strcmp(val, "composite") == 0) in = PIN_INPUT_COMPOSITE;
            else FAIL("--input: expected dv, svideo or composite, got \"%s\"", val);
            p.input = in;
            pin_script_item_t *it = item_add(&p);
            if (!it) { rc = PIN_ERR_NOMEM; break; }
            it->kind = PIN_SITEM_INPUT;
            it->input = in;
            if (!p.started) {
                sc->initial.has_input = 1;
                sc->initial.input = in;
            }
        } else if (strcmp(name, "--std") == 0) {
            pin_std_t std;
            if (!std_from_key(val, &std))
                FAIL("--std: unknown standard \"%s\" (auto pal ntsc pal-m pal-n pal-60 ntsc-443 ntsc-j secam)", val);
            pin_script_item_t *it = item_add(&p);
            if (!it) { rc = PIN_ERR_NOMEM; break; }
            it->kind = PIN_SITEM_STD;
            it->std = std;
            if (!p.started) {
                sc->initial.has_std = 1;
                sc->initial.std = std;
            }
        } else if (strcmp(name, "--format") == 0) {
            pin_format_t f;
            if (!fmt_from_key(val, &f))
                FAIL("--format: unknown format \"%s\" (dv dv-avi dv-mov hdv-ts hdv-mov hdv-mkv avi ffv1-mkv)", val);
            int k = fmt_kind(f);
            p.fmt[k] = f;
            p.fmt_set[k] = 1;
            if (!p.started) {
                if (k == PIN_KIND_ANALOG) { sc->initial.has_format_analog = 1; sc->initial.format_analog = f; }
                else if (k == PIN_KIND_DV) { sc->initial.has_format_dv = 1; sc->initial.format_dv = f; }
                else { sc->initial.has_format_hdv = 1; sc->initial.format_hdv = f; }
            }
        } else if (strcmp(name, "--aspect") == 0) {
            if (strcmp(val, "auto") == 0) p.aspect = PIN_ASPECT_AUTO;
            else if (strcmp(val, "4:3") == 0) p.aspect = PIN_ASPECT_4_3;
            else if (strcmp(val, "16:9") == 0) p.aspect = PIN_ASPECT_16_9;
            else FAIL("--aspect: expected auto, 4:3 or 16:9, got \"%s\"", val);
            if (!p.started) { sc->initial.has_aspect = 1; sc->initial.aspect = p.aspect; }
        } else if (strcmp(name, "--split") == 0) {
            p.split = 1;
            if (!p.started) { sc->initial.has_split = 1; sc->initial.split = 1; }
        } else if (strcmp(name, "--title") == 0) {
            if (strlen(val) >= PIN_TEXT_MAX)
                FAIL("--title: too long");
            snprintf(p.title, sizeof(p.title), "%s", val);
            if (!p.started) {
                sc->initial.has_title = 1;
                snprintf(sc->initial.title, sizeof(sc->initial.title), "%s", val);
            }
        } else if (strcmp(name, "--keep-raw") == 0) {
            p.keep_raw = 1;
            if (!p.started) { sc->initial.has_keep_raw = 1; sc->initial.keep_raw = 1; }
        } else if (strcmp(name, "--overwrite") == 0) {
            p.overwrite = 1;
        } else if (strcmp(name, "--debug") == 0) {
            sc->debug = 1;
        } else if (strcmp(name, "--brightness") == 0 || strcmp(name, "--contrast") == 0 ||
                   strcmp(name, "--saturation") == 0 || strcmp(name, "--hue") == 0 ||
                   strcmp(name, "--sharpness") == 0 || strcmp(name, "--audio-gain") == 0) {
            pin_control_t c;
            int32_t v;
            control_from_name(name + 2, &c);
            if (!parse_int(val, &v))
                FAIL("%s: expected a whole number, got \"%s\"", name, val);
            pin_script_item_t *it = item_add(&p);
            if (!it) { rc = PIN_ERR_NOMEM; break; }
            it->kind = PIN_SITEM_CONTROL;
            it->ctl = c;
            it->value = v;
            if (!p.started) {
                sc->initial.controls_set |= 1u << c;
                *settings_control_slot(&sc->initial, c) = v;
            }
        } else if (strcmp(name, "--rew") == 0 || strcmp(name, "--ff") == 0 || strcmp(name, "--play") == 0 ||
                   strcmp(name, "--pause") == 0 || strcmp(name, "--stop") == 0) {
            if (inl)
                FAIL("%s takes no value (got \"%s\")", name, inl);
            if (is_analog(&p))
                FAIL("%s needs the DV input; analog inputs have no deck", name);
            pin_script_item_t *it = item_add(&p);
            if (!it) { rc = PIN_ERR_NOMEM; break; }
            it->kind = PIN_SITEM_STEP;
            it->step = sc->nsteps++;
            it->step_kind = strcmp(name, "--rew") == 0 ? PIN_SSTEP_REW : strcmp(name, "--ff") == 0 ? PIN_SSTEP_FF
                            : strcmp(name, "--play") == 0 ? PIN_SSTEP_PLAY : strcmp(name, "--pause") == 0 ? PIN_SSTEP_PAUSE
                            : PIN_SSTEP_STOP;
            p.started = 1;
            p.capture_open = 0;
        } else if (strcmp(name, "--capture") == 0) {
            if (!inl && i + 1 < argc && !(argv[i + 1][0] == '-' && argv[i + 1][1] == '-'))
                inl = argv[++i];
            if (!inl || !*inl)
                FAIL("--capture needs a file name (or - for stdout)");
            if (strlen(inl) >= PIN_PATH_MAX)
                FAIL("--capture: path too long");
            pin_script_item_t *it = item_add(&p);
            if (!it) { rc = PIN_ERR_NOMEM; break; }
            it->kind = PIN_SITEM_STEP;
            it->step = sc->nsteps++;
            it->step_kind = PIN_SSTEP_CAPTURE;
            pin_script_capture_t *c = &it->cap;
            snprintf(c->path, sizeof(c->path), "%s", inl);
            c->aspect = p.aspect;
            c->split = p.split;
            snprintf(c->title, sizeof(c->title), "%s", p.title);
            c->keep_raw = p.keep_raw;
            c->overwrite = p.overwrite;
            for (int k = 0; k < 3; k++)
                c->format[k] = p.fmt[k];
            if (strcmp(inl, "-") == 0) {
                if (p.split)
                    FAIL("--capture -: --split does not apply to a stream on stdout");
                if (p.stdout_used)
                    FAIL("--capture - can be used only once (the stream has one stdout)");
                p.stdout_used = 1;
                c->to_stdout = 1;
            } else {
                char ext[16];
                pin_script_split_ext(inl, c->base, sizeof(c->base), ext, sizeof(ext));
                pin_format_t extfmt[3];
                unsigned unsupp = 0;
                int known = ext[0] && pin_script_ext_formats(ext, extfmt, &unsupp);
                if (!known && !(p.fmt_set[0] || p.fmt_set[1] || p.fmt_set[2]))
                    FAIL("--capture %s: cannot tell the format from the file name (use .dv .avi .mov .ts .m2t "
                         ".mkv, or give --format)", inl);
                for (int k = 0; k < 3; k++) {
                    if (p.fmt_set[k])
                        continue;
                    if (known) {
                        c->format[k] = extfmt[k];
                        if (unsupp & (1u << k))
                            c->warn_mask |= 1u << k;
                    }
                }
            }
            p.started = 1;
            p.capture_open = 1;
        } else if (strcmp(name, "--wait") == 0) {
            if (!inl && i + 1 < argc && !(argv[i + 1][0] == '-' && argv[i + 1][1] == '-'))
                inl = argv[++i];
            pin_script_item_t *it = item_add(&p);
            if (!it) { rc = PIN_ERR_NOMEM; break; }
            it->kind = PIN_SITEM_STEP;
            it->step = sc->nsteps++;
            it->step_kind = PIN_SSTEP_WAIT;
            if (!inl) {
                it->nconds = 1;
                it->conds[0].kind = PIN_COND_IDLE;
            } else {
                char buf[256];
                if (strlen(inl) >= sizeof(buf))
                    FAIL("--wait: condition list too long");
                snprintf(buf, sizeof(buf), "%s", inl);
                char *save = buf;
                for (;;) {
                    char *comma = strchr(save, ',');
                    if (comma)
                        *comma = 0;
                    char why[96];
                    if (it->nconds >= PIN_SCRIPT_MAX_CONDS)
                        FAIL("--wait: too many conditions (max %d)", PIN_SCRIPT_MAX_CONDS);
                    if (!pin_script_cond_parse(save, &it->conds[it->nconds], why, sizeof(why)))
                        FAIL("--wait %s: %s", inl, why);
                    pin_script_cond_t *cd = &it->conds[it->nconds++];
                    if (is_analog(&p) && (cd->kind == PIN_COND_IDLE || cd->kind == PIN_COND_TIMECODE))
                        FAIL("--wait %s: %s needs the DV input; analog inputs only allow signal, nosignal "
                             "and durations", inl, cd->kind == PIN_COND_IDLE ? "idle" : "a timecode");
                    if (!comma)
                        break;
                    save = comma + 1;
                }
            }
            p.started = 1;
        } else {
            FAIL("unknown option \"%s\"", arg);
        }
#undef FAIL
    }
parse_end:
    if (rc != PIN_OK) {
        pin_script_destroy(sc);
        return rc;
    }
    *out = sc;
    return PIN_OK;
}

void pin_script_destroy(pin_script_t *sc)
{
    if (!sc)
        return;
    free(sc->items);
    free(sc);
}

pin_script_t *pin_script_clone(const pin_script_t *sc)
{
    if (!sc)
        return NULL;
    pin_script_t *c = malloc(sizeof(*c));
    if (!c)
        return NULL;
    *c = *sc;
    c->items = NULL;
    c->cap_items = c->nitems = 0;
    if (sc->nitems > 0) {
        c->items = malloc((size_t)sc->nitems * sizeof(*c->items));
        if (!c->items) {
            free(c);
            return NULL;
        }
        memcpy(c->items, sc->items, (size_t)sc->nitems * sizeof(*c->items));
        c->nitems = c->cap_items = sc->nitems;
    }
    return c;
}

pin_status_t pin_script_step_description(const pin_script_t *sc, int step, char *out, size_t cap)
{
    if (!sc || !out || cap == 0 || step < 0 || step >= sc->nsteps)
        return PIN_ERR_ARG;
    for (int i = 0; i < sc->nitems; i++) {
        const pin_script_item_t *it = &sc->items[i];
        if (it->kind != PIN_SITEM_STEP || it->step != step)
            continue;
        switch (it->step_kind) {
        case PIN_SSTEP_REW: snprintf(out, cap, "rew"); break;
        case PIN_SSTEP_FF: snprintf(out, cap, "ff"); break;
        case PIN_SSTEP_PLAY: snprintf(out, cap, "play"); break;
        case PIN_SSTEP_PAUSE: snprintf(out, cap, "pause"); break;
        case PIN_SSTEP_STOP: snprintf(out, cap, "stop"); break;
        case PIN_SSTEP_CAPTURE: snprintf(out, cap, "capture %s", it->cap.path); break;
        case PIN_SSTEP_WAIT: {
            size_t n = (size_t)snprintf(out, cap, "wait ");
            for (int k = 0; k < it->nconds && n < cap; k++) {
                char t[32];
                pin_script_cond_text(&it->conds[k], t, sizeof(t));
                n += (size_t)snprintf(out + n, cap - n, "%s%s", k ? "," : "", t);
            }
            break;
        }
        }
        return PIN_OK;
    }
    return PIN_ERR_ARG;
}
