/*
 * pin_stub_launch.c — pin_launch_parse / pin_launch_help / pin_run_actions.
 *
 * Implements the real CLI syntax from the plan's "Changes after your
 * review" section:
 *   --device <id|first> --input <dv|svideo|composite> --output <path>
 *   --format <name> --title <text> --std <name> --split --passes N
 *   --idle-min N --aspect <auto|4:3|16:9>
 *   --actions a,b,... (rewind,play,stop,capture,wait-eot)
 *   --exit-when-done [--preset file.ini]
 *
 * --preset is parsed (so it doesn't trip up argv scanning) but not
 * implemented: given a path, pin_launch_parse returns PIN_ERR_ARG saying
 * so; if the flag is simply absent nothing happens. TODO: real preset
 * loading belongs in the real core (Phase 8), not this hardware-free stub.
 */
#include "pin_stub.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static int ieq(const char *a, const char *b) {
#if defined(_WIN32)
    return _stricmp(a, b) == 0;
#else
    return strcasecmp(a, b) == 0;
#endif
}

static const char *k_help =
    "Usage: pinctl [options]\n"
    "\n"
    "  --device <id|first>      device to open (default: first)\n"
    "  --input <dv|svideo|composite>\n"
    "  --output <path>          base output path (extension added from --format)\n"
    "  --format <name>          analog-avi | analog-ffv1 | dv-raw | dv-avi |\n"
    "                            dv-mov | hdv-ts | hdv-mov | hdv-mkv\n"
    "  --title <text>           metadata title (ignored where unsupported)\n"
    "  --std <name>              Auto | PAL | NTSC | PAL-M | PAL-N | PAL-60 |\n"
    "                            NTSC-443 | NTSC-J | SECAM\n"
    "  --split                  new numbered file per recording (DV/HDV)\n"
    "  --passes N                capture the tape N times (DV/HDV, default 1)\n"
    "  --idle-min N              stop after N minutes without data (0 = never)\n"
    "  --aspect <auto|4:3|16:9>  aspect override for preview and capture\n"
    "  --actions a,b,...         rewind, play, stop, capture, wait-eot, in order\n"
    "  --exit-when-done          exit once the action list finishes\n"
    "  --preset <file.ini>       not implemented in this stub build (TODO)\n"
    "  --help                    this text\n";

const char *pin_launch_help(void) { return k_help; }

static pin_status_t map_format(const char *name, pin_format_t *out) {
    static const struct { const char *name; pin_format_t f; } tbl[] = {
        {"analog-avi", PIN_FMT_ANALOG_AVI}, {"analog-ffv1", PIN_FMT_ANALOG_FFV1_MKV},
        {"dv-raw", PIN_FMT_DV_RAW}, {"dv-avi", PIN_FMT_DV_AVI}, {"dv-mov", PIN_FMT_DV_MOV},
        {"hdv-ts", PIN_FMT_HDV_TS}, {"hdv-mov", PIN_FMT_HDV_MOV}, {"hdv-mkv", PIN_FMT_HDV_MKV},
    };
    for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
        if (ieq(name, tbl[i].name)) { *out = tbl[i].f; return PIN_OK; }
    }
    return PIN_ERR_ARG;
}

static pin_status_t map_std(const char *name, pin_std_t *out) {
    for (int i = 0; i < PIN_STD_COUNT; i++) {
        if (ieq(name, pin_std_name((pin_std_t)i))) { *out = (pin_std_t)i; return PIN_OK; }
    }
    return PIN_ERR_ARG;
}

static pin_status_t map_action(const char *name, pin_action_t *out) {
    if (ieq(name, "rewind")) { *out = PIN_ACT_REWIND; return PIN_OK; }
    if (ieq(name, "play")) { *out = PIN_ACT_PLAY; return PIN_OK; }
    if (ieq(name, "stop")) { *out = PIN_ACT_STOP; return PIN_OK; }
    if (ieq(name, "capture")) { *out = PIN_ACT_CAPTURE; return PIN_OK; }
    if (ieq(name, "wait-eot")) { *out = PIN_ACT_WAIT_EOT; return PIN_OK; }
    return PIN_ERR_ARG;
}

#define NEED_VALUE(flag) \
    do { if (i + 1 >= argc) { snprintf(err, err_cap, "%s needs a value", flag); return PIN_ERR_ARG; } } while (0)

static pin_status_t launch_parse_native(int argc, const char *const *argv, pin_launch_t *out,
                                        char *err, size_t err_cap);

pin_status_t pin_launch_parse(int argc, const char *const *argv, pin_launch_t *out,
                               char *err, size_t err_cap) {
    if (!out) return PIN_ERR_ARG;
    if (err && err_cap) err[0] = 0;
    if (out->size == sizeof(*out)) return launch_parse_native(argc, argv, out, err, err_cap);
    if (!PIN_NATIVE_IS_V2 && out->size == PIN_LAUNCH_V2_SIZE) {
        /* caller already uses the extended layout (see pin_stub_abi2.h) */
        pin_launch_t tmp;
        tmp.size = sizeof(tmp);
        pin_status_t st = launch_parse_native(argc, argv, &tmp, err, err_cap);
        pin_launch_native_to_v2(&tmp, out);
        return st;
    }
    return PIN_ERR_ABI;
}

static pin_status_t launch_parse_native(int argc, const char *const *argv, pin_launch_t *out,
                                        char *err, size_t err_cap) {

    uint32_t caller_size = out->size;
    memset(out, 0, sizeof(*out));
    out->size = caller_size;
    out->device[0] = 0;
    pin_capture_opts_defaults(&out->capture);

    for (int i = 1; i < argc; i++) { /* argv[0] is the program */
        const char *a = argv[i];
        if (!a) continue;
        if (ieq(a, "--device")) {
            NEED_VALUE("--device");
            strncpy(out->device, argv[++i], sizeof(out->device) - 1);
        } else if (ieq(a, "--input")) {
            NEED_VALUE("--input");
            const char *v = argv[++i];
            if (ieq(v, "dv")) out->input = PIN_INPUT_DV;
            else if (ieq(v, "svideo")) out->input = PIN_INPUT_SVIDEO;
            else if (ieq(v, "composite")) out->input = PIN_INPUT_COMPOSITE;
            else { snprintf(err, err_cap, "--input: unknown value '%s'", v); return PIN_ERR_ARG; }
            out->has_input = 1;
        } else if (ieq(a, "--output")) {
            NEED_VALUE("--output");
            strncpy(out->capture.path, argv[++i], sizeof(out->capture.path) - 1);
            out->has_capture_opts = 1;
            out->capture_fields |= PIN_OPT_PATH;
        } else if (ieq(a, "--format")) {
            NEED_VALUE("--format");
            pin_format_t f;
            if (map_format(argv[++i], &f) != PIN_OK) {
                snprintf(err, err_cap, "--format: unknown value '%s'", argv[i]); return PIN_ERR_ARG;
            }
            pin_format_info_t fi; fi.size = sizeof(fi);
            pin_format_info(f, &fi);
            switch (fi.kind) {
            case PIN_KIND_ANALOG: out->capture.format_analog = f; break;
            case PIN_KIND_DV: out->capture.format_dv = f; break;
            case PIN_KIND_HDV: out->capture.format_hdv = f; break;
            }
            out->has_capture_opts = 1;
            out->capture_fields |= PIN_OPT_FORMAT;
        } else if (ieq(a, "--title")) {
            NEED_VALUE("--title");
            strncpy(out->capture.title, argv[++i], sizeof(out->capture.title) - 1);
            out->has_capture_opts = 1;
            out->capture_fields |= PIN_OPT_TITLE;
        } else if (ieq(a, "--std")) {
            NEED_VALUE("--std");
            pin_std_t std;
            if (map_std(argv[++i], &std) != PIN_OK) {
                snprintf(err, err_cap, "--std: unknown value '%s'", argv[i]); return PIN_ERR_ARG;
            }
            out->has_std = 1;
            out->std = std;
        } else if (ieq(a, "--split")) {
            out->capture.scene_split = 1;
            out->has_capture_opts = 1;
            out->capture_fields |= PIN_OPT_SPLIT;
        } else if (ieq(a, "--passes")) {
            NEED_VALUE("--passes");
            out->capture.passes = atoi(argv[++i]);
            if (out->capture.passes < 1) out->capture.passes = 1;
            out->has_capture_opts = 1;
            out->capture_fields |= PIN_OPT_PASSES;
        } else if (ieq(a, "--idle-min")) {
            NEED_VALUE("--idle-min");
            out->capture.idle_stop_minutes = atoi(argv[++i]);
            out->has_capture_opts = 1;
            out->capture_fields |= PIN_OPT_IDLE;
        } else if (ieq(a, "--aspect")) {
            NEED_VALUE("--aspect");
            const char *v = argv[++i];
            if (ieq(v, "auto")) out->capture.aspect = PIN_ASPECT_AUTO;
            else if (strcmp(v, "4:3") == 0) out->capture.aspect = PIN_ASPECT_4_3;
            else if (strcmp(v, "16:9") == 0) out->capture.aspect = PIN_ASPECT_16_9;
            else { snprintf(err, err_cap, "--aspect: unknown value '%s'", v); return PIN_ERR_ARG; }
            out->has_capture_opts = 1;
            out->capture_fields |= PIN_OPT_ASPECT;
        } else if (ieq(a, "--actions")) {
            NEED_VALUE("--actions");
            char buf[512];
            strncpy(buf, argv[++i], sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = 0;
            char *save = NULL;
            char *piece;
#if defined(_WIN32)
            piece = strtok_s(buf, ",", &save);
#else
            piece = strtok_r(buf, ",", &save);
#endif
            while (piece) {
                if (out->action_count >= PIN_MAX_ACTIONS) {
                    snprintf(err, err_cap, "--actions: too many actions (max %d)", PIN_MAX_ACTIONS);
                    return PIN_ERR_ARG;
                }
                pin_action_t act;
                if (map_action(piece, &act) != PIN_OK) {
                    snprintf(err, err_cap, "--actions: unknown action '%s'", piece);
                    return PIN_ERR_ARG;
                }
                out->actions[out->action_count++] = act;
#if defined(_WIN32)
                piece = strtok_s(NULL, ",", &save);
#else
                piece = strtok_r(NULL, ",", &save);
#endif
            }
        } else if (ieq(a, "--exit-when-done")) {
            out->exit_when_done = 1;
        } else if (ieq(a, "--preset")) {
            NEED_VALUE("--preset");
            snprintf(err, err_cap, "--preset %s: preset files are not implemented in this stub build", argv[++i]);
            return PIN_ERR_ARG;
        } else if (ieq(a, "--help") || ieq(a, "-h")) {
            snprintf(err, err_cap, "%s", k_help);
            return PIN_ERR_ARG;
        } else {
            snprintf(err, err_cap, "unknown option '%s'", a);
            return PIN_ERR_ARG;
        }
    }
    return PIN_OK;
}

/* ---- action sequencer -------------------------------------------------- */

typedef struct {
    pin_session_t *s;
    pin_launch_t launch;
} action_thread_arg_t;

static int wait_deck_state(pin_session_t *s, pin_deck_state_t want, double timeout_s) {
    double deadline = pin_stub_now() + timeout_s;
    for (;;) {
        pthread_mutex_lock(&s->lock);
        int closing = s->closing;
        pin_deck_state_t cur = s->deck.state;
        pthread_mutex_unlock(&s->lock);
        if (closing) return 0;
        if (cur == want) return 1;
        if (pin_stub_now() > deadline) return 0;
        pin_stub_sleep_ms(50);
    }
}

static void *action_runner(void *arg) {
    action_thread_arg_t *ta = (action_thread_arg_t *)arg;
    pin_session_t *s = ta->s;
    pin_status_t result = PIN_OK;

    for (int i = 0; i < ta->launch.action_count; i++) {
        pthread_mutex_lock(&s->lock);
        int closing = s->closing;
        pthread_mutex_unlock(&s->lock);
        if (closing) { result = PIN_ERR_STATE; break; }

        switch (ta->launch.actions[i]) {
        case PIN_ACT_REWIND:
            pin_deck(s, PIN_DECK_CMD_REW);
            wait_deck_state(s, PIN_DECK_STOPPED, 30.0);
            break;
        case PIN_ACT_PLAY:
            pin_deck(s, PIN_DECK_CMD_PLAY);
            break;
        case PIN_ACT_STOP:
            pin_deck(s, PIN_DECK_CMD_STOP);
            break;
        case PIN_ACT_CAPTURE:
            if (ta->launch.has_capture_opts && ta->launch.capture.path[0]) {
                result = pin_capture_start(s, &ta->launch.capture, 1);
            } else {
                result = PIN_ERR_ARG;
            }
            /* wait for capture to end on its own (idle-stop) or be
             * stopped by a later PIN_ACT_STOP handled elsewhere */
            for (;;) {
                pthread_mutex_lock(&s->lock);
                int active = s->cap.active;
                int closing2 = s->closing;
                pthread_mutex_unlock(&s->lock);
                if (!active || closing2) break;
                pin_stub_sleep_ms(100);
            }
            break;
        case PIN_ACT_WAIT_EOT:
            wait_deck_state(s, PIN_DECK_STOPPED, 3600.0);
            break;
        default:
            break;
        }
    }

    pin_evtq_push(&s->events, PIN_EVT_DONE, (int32_t)result, "");
    free(ta);
    return NULL;
}

pin_status_t pin_run_actions(pin_session_t *s, const pin_launch_t *launch) {
    if (!s || !launch) return PIN_ERR_ARG;
    pin_launch_t native;
    if (launch->size == sizeof(*launch)) {
        native = *launch;
    } else if (!PIN_NATIVE_IS_V2 && launch->size == PIN_LAUNCH_V2_SIZE) {
        pin_launch_v2_to_native(launch, &native);
    } else {
        return PIN_ERR_ABI;
    }
    launch = &native;

    action_thread_arg_t *ta = (action_thread_arg_t *)malloc(sizeof(*ta));
    if (!ta) return PIN_ERR_NOMEM;
    ta->s = s;
    ta->launch = *launch;

    if (launch->has_input) pin_set_input(s, launch->input);
    if (launch->has_std) pin_set_standard(s, launch->std);

    pthread_t th;
    if (pthread_create(&th, NULL, action_runner, ta) != 0) {
        free(ta);
        return PIN_ERR_INTERNAL;
    }
    pthread_detach(th);
    return PIN_OK;
}
