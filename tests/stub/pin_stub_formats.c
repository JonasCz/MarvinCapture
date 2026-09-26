/* pin_stub_formats.c — the static pin_formats()/pin_format_info() table. */
#include "pin_stub.h"
#include <string.h>

typedef struct {
    pin_format_t format;
    pin_kind_t kind;
    const char *label;
    const char *ext;
    int supports_title;
    int supports_scene_split;
    int supports_multi_pass;
    int is_default;
} fmt_row_t;

/* PIN_FMT_HDV_MKV exists in addition to PIN_FMT_HDV_MOV because stock
 * Windows players handle "hdv2" MOV poorly (Suggestions item 9 in the
 * plan); MKV costs nothing since it goes through the same rewrap sink. */
static const fmt_row_t k_formats[PIN_FMT_COUNT] = {
    [PIN_FMT_ANALOG_AVI]     = {PIN_FMT_ANALOG_AVI, PIN_KIND_ANALOG, "Uncompressed AVI", "avi", 1, 0, 0, 1},
    [PIN_FMT_ANALOG_FFV1_MKV]= {PIN_FMT_ANALOG_FFV1_MKV, PIN_KIND_ANALOG, "FFV1 (MKV)", "mkv", 1, 0, 0, 0},
    [PIN_FMT_DV_RAW]         = {PIN_FMT_DV_RAW, PIN_KIND_DV, "Raw DV (.dv)", "dv", 0, 1, 1, 1},
    [PIN_FMT_DV_AVI]         = {PIN_FMT_DV_AVI, PIN_KIND_DV, "DV in AVI (type 2)", "avi", 1, 1, 1, 0},
    [PIN_FMT_DV_MOV]         = {PIN_FMT_DV_MOV, PIN_KIND_DV, "DV in QuickTime", "mov", 1, 1, 1, 0},
    [PIN_FMT_HDV_TS]         = {PIN_FMT_HDV_TS, PIN_KIND_HDV, "HDV Transport Stream (.ts)", "ts", 0, 1, 1, 1},
    [PIN_FMT_HDV_MOV]        = {PIN_FMT_HDV_MOV, PIN_KIND_HDV, "HDV in QuickTime", "mov", 1, 1, 1, 0},
    [PIN_FMT_HDV_MKV]        = {PIN_FMT_HDV_MKV, PIN_KIND_HDV, "HDV in Matroska", "mkv", 1, 1, 1, 0},
};

static void fill(pin_format_info_t *out, const fmt_row_t *r) {
    uint32_t caller_size = out->size;
    memset(out, 0, sizeof(*out));
    out->size = caller_size ? caller_size : sizeof(*out);
    out->format = r->format;
    out->kind = r->kind;
    strncpy(out->label, r->label, sizeof(out->label) - 1);
    strncpy(out->extension, r->ext, sizeof(out->extension) - 1);
    out->supports_title = r->supports_title;
    out->supports_scene_split = r->supports_scene_split;
    out->supports_multi_pass = r->supports_multi_pass;
    out->is_default = r->is_default;
}

int pin_formats(pin_kind_t kind, pin_format_info_t *out, int max) {
    int n = 0;
    for (int i = 0; i < PIN_FMT_COUNT; i++) {
        if (k_formats[i].kind != kind) continue;
        if (n < max) fill(&out[n], &k_formats[i]);
        n++;
    }
    return n;
}

pin_status_t pin_format_info(pin_format_t f, pin_format_info_t *out) {
    if (!out || f < 0 || f >= PIN_FMT_COUNT) return PIN_ERR_ARG;
    fill(out, &k_formats[f]);
    return PIN_OK;
}

void pin_capture_opts_defaults(pin_capture_opts_t *o) {
    if (!o) return;
    uint32_t caller_size = o->size;
    memset(o, 0, sizeof(*o));
    o->size = caller_size ? caller_size : sizeof(*o);
    o->format_analog = PIN_FMT_ANALOG_AVI;
    o->format_dv = PIN_FMT_DV_RAW;
    o->format_hdv = PIN_FMT_HDV_TS;
    o->aspect = PIN_ASPECT_AUTO;
    o->scene_split = 0;
    o->idle_stop_minutes = 0;
    o->passes = 1;
    o->start_deck = 0;
    o->keep_raw = 0;
}
