/*
 * pin_stub_session.c — pin_open/close, enumerate, input switching, analog
 * controls, status snapshot, event polling, and the worker thread that
 * drives all of the session's fake timers.
 */
#include "pin_stub.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

/* ==== version / errors ===================================================== */

uint32_t pin_api_version(void) { return PIN_API_VERSION; }

const char *pin_version_string(void) {
    return "pinnacle-oss-core 0.stub (tests/stub, hardware-free)";
}

const char *pin_strerror(pin_status_t s) {
    switch (s) {
    case PIN_OK: return "ok";
    case PIN_ERR_ABI: return "struct size / API version mismatch";
    case PIN_ERR_ARG: return "bad argument";
    case PIN_ERR_STATE: return "not allowed in the current state";
    case PIN_ERR_NOT_FOUND: return "device not found";
    case PIN_ERR_BUSY: return "device in use by another process";
    case PIN_ERR_NO_DRIVER: return "device not bound to WinUSB";
    case PIN_ERR_USB: return "USB error";
    case PIN_ERR_FIRMWARE: return "FPGA bitstream missing, unreadable or rejected";
    case PIN_ERR_NOT_READY: return "device needs a power cycle (replug)";
    case PIN_ERR_NO_CAMERA: return "no camera / deck on the 1394 bus";
    case PIN_ERR_DECK: return "AV/C command rejected";
    case PIN_ERR_IO: return "file system error";
    case PIN_ERR_EXISTS: return "output would overwrite an existing file";
    case PIN_ERR_DISK_FULL: return "disk full";
    case PIN_ERR_CODEC: return "muxer / encoder failure";
    case PIN_ERR_NOMEM: return "out of memory";
    case PIN_ERR_INTERNAL: return "internal error";
    default: return "unknown error";
    }
}

static char g_firmware_dir[PIN_PATH_MAX] = "firmware";

pin_status_t pin_set_firmware_dir(const char *utf8_dir) {
    if (!utf8_dir) return PIN_ERR_ARG;
    strncpy(g_firmware_dir, utf8_dir, sizeof(g_firmware_dir) - 1);
    g_firmware_dir[sizeof(g_firmware_dir) - 1] = 0;
    return PIN_OK; /* stub never actually loads bitstreams */
}

/* ==== enumeration =========================================================== */

int pin_enumerate(pin_device_info_t *out, int max) {
    pin_stub_devices_init();
    pthread_mutex_lock(&g_pin_stub_devices_lock);
    int n = PIN_STUB_DEVICE_COUNT;
    int fill = (max < n) ? max : n;
    for (int i = 0; i < fill; i++) {
        pin_stub_device_t *d = &g_pin_stub_devices[i];
        pin_device_info_t *o = &out[i];
        uint32_t caller_size = o->size;
        memset(o, 0, sizeof(*o));
        o->size = caller_size ? caller_size : sizeof(*o);
        strncpy(o->id, d->id, sizeof(o->id) - 1);
        strncpy(o->name, d->name, sizeof(o->name) - 1);
        strncpy(o->serial, d->serial, sizeof(o->serial) - 1);
        o->vid = d->vid;
        o->pid = d->pid;
        o->owner_pid = d->owner_pid;
        if (d->opened_here) o->state = PIN_DEV_OPEN_HERE;
        else o->state = d->base_state;
    }
    pthread_mutex_unlock(&g_pin_stub_devices_lock);
    return n;
}

/* ==== open / close =========================================================== */

static void gen_fake_serial(pin_stub_device_t *d) {
    if (d->serial[0]) return;
    unsigned seed = (unsigned)time(NULL) ^ (unsigned)(uintptr_t)d;
    snprintf(d->serial, sizeof(d->serial), "%04X%04X%04X%04X",
              ((d->vid << 4) ^ seed) & 0xFFFF, (unsigned)(seed >> 8) & 0xFFFF,
              (unsigned)(seed * 2654435761u) & 0xFFFF, (unsigned)(seed ^ 0xA5A5) & 0xFFFF);
}

static void reset_frame(pin_stub_frame_t *f) {
    memset(f, 0, sizeof(*f));
}

pin_status_t pin_open(const char *device_id, pin_session_t **out) {
    if (!out) return PIN_ERR_ARG;
    *out = NULL;
    pin_stub_devices_init();

    pthread_mutex_lock(&g_pin_stub_devices_lock);
    int idx = -1;
    if (!device_id || !device_id[0] || strcmp(device_id, "first") == 0) {
        for (int i = 0; i < PIN_STUB_DEVICE_COUNT; i++) {
            if (!g_pin_stub_devices[i].opened_here && g_pin_stub_devices[i].base_state == PIN_DEV_READY) {
                idx = i; break;
            }
        }
    } else {
        for (int i = 0; i < PIN_STUB_DEVICE_COUNT; i++) {
            if (strcmp(g_pin_stub_devices[i].id, device_id) == 0) { idx = i; break; }
        }
    }
    if (idx < 0) {
        pthread_mutex_unlock(&g_pin_stub_devices_lock);
        return PIN_ERR_NOT_FOUND;
    }
    pin_stub_device_t *d = &g_pin_stub_devices[idx];
    if (d->opened_here || d->base_state == PIN_DEV_IN_USE) {
        pthread_mutex_unlock(&g_pin_stub_devices_lock);
        return PIN_ERR_BUSY;
    }
    d->opened_here = 1;
    gen_fake_serial(d);
    pthread_mutex_unlock(&g_pin_stub_devices_lock);

    pin_session_t *s = (pin_session_t *)calloc(1, sizeof(pin_session_t));
    if (!s) {
        pthread_mutex_lock(&g_pin_stub_devices_lock);
        d->opened_here = 0;
        pthread_mutex_unlock(&g_pin_stub_devices_lock);
        return PIN_ERR_NOMEM;
    }
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->cond, NULL);
    pthread_mutex_init(&s->preview_lock, NULL);
    pthread_cond_init(&s->preview_cond, NULL);
    pthread_mutex_init(&s->ring_lock, NULL);
    pin_evtq_init(&s->events);

    s->device_index = idx;
    strncpy(s->device_id, d->id, sizeof(s->device_id) - 1);
    s->state = PIN_STATE_READY;
    s->input = PIN_INPUT_DV;
    s->stream_kind = PIN_KIND_DV;
    s->width = 720; s->height = 576; s->dar_num = 4; s->dar_den = 3;
    s->requested_std = PIN_STD_AUTO;
    s->detected_std = PIN_STD_NTSC; /* see pin_set_standard() comment on PIN_STD_AUTO */
    s->aspect = PIN_ASPECT_AUTO;
    s->locked_index = -1;
    s->preview_enabled = 1;
    s->opened_at = pin_stub_now();
    s->hdv_toggle_at = s->opened_at + 30.0;
    s->hdv_locked = getenv("PIN_STUB_HDV") && strcmp(getenv("PIN_STUB_HDV"), "1") == 0;
    for (int i = 0; i < 3; i++) reset_frame(&s->frame[i]);

    /* analog control defaults */
    s->controls[PIN_CTL_BRIGHTNESS] = 128;
    s->controls[PIN_CTL_CONTRAST]   = 64;
    s->controls[PIN_CTL_SATURATION] = 64;
    s->controls[PIN_CTL_HUE]        = 0;
    s->controls[PIN_CTL_SHARPNESS]  = 0;
    s->controls[PIN_CTL_AUDIO_GAIN] = 0;

    s->deck.state = PIN_DECK_STOPPED;
    s->deck.tape_percent = 0.0;
    s->deck.tc_frames = 0;

    long ring_frames = 48000 / 2; /* 0.5s ring */
    s->ring_cap = (int)ring_frames;
    s->ring = (int16_t *)calloc((size_t)ring_frames * 2, sizeof(int16_t));

    pthread_create(&s->worker_thread, NULL, pin_stub_worker_main, s);
    s->worker_started = 1;
    pthread_create(&s->preview_thread, NULL, pin_stub_preview_main, s);
    s->preview_started = 1;

    *out = s;
    pin_evtq_push(&s->events, PIN_EVT_STATE, (int32_t)s->state, "opened");
    return PIN_OK;
}

void pin_close(pin_session_t *s) {
    if (!s) return;

    pthread_mutex_lock(&s->lock);
    s->closing = 1;
    pthread_mutex_unlock(&s->lock);

    pthread_mutex_lock(&s->preview_lock);
    pthread_cond_broadcast(&s->preview_cond);
    pthread_mutex_unlock(&s->preview_lock);
    pthread_mutex_lock(&s->lock);
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);

    if (s->worker_started) pthread_join(s->worker_thread, NULL);
    if (s->preview_started) pthread_join(s->preview_thread, NULL);

    /* close any open capture file cleanly */
    if (s->cap.fp) { fclose(s->cap.fp); s->cap.fp = NULL; }

    for (int i = 0; i < 3; i++) {
        free(s->frame[i].plane[0]);
        free(s->frame[i].plane[1]);
        free(s->frame[i].plane[2]);
    }
    free(s->ring);

    pthread_mutex_lock(&g_pin_stub_devices_lock);
    if (s->device_index >= 0 && s->device_index < PIN_STUB_DEVICE_COUNT)
        g_pin_stub_devices[s->device_index].opened_here = 0;
    pthread_mutex_unlock(&g_pin_stub_devices_lock);

    pin_evtq_destroy(&s->events);
    pthread_mutex_destroy(&s->lock);
    pthread_cond_destroy(&s->cond);
    pthread_mutex_destroy(&s->preview_lock);
    pthread_cond_destroy(&s->preview_cond);
    pthread_mutex_destroy(&s->ring_lock);
    free(s);
}

/* ==== input switching ======================================================== */

pin_status_t pin_set_input(pin_session_t *s, pin_input_t input) {
    if (!s) return PIN_ERR_ARG;
    pthread_mutex_lock(&s->lock);
    if (s->state == PIN_STATE_CAPTURING || s->state == PIN_STATE_STOPPING) {
        pthread_mutex_unlock(&s->lock);
        return PIN_ERR_STATE;
    }
    s->input = input;
    s->state = PIN_STATE_PREPARING;
    s->prepare_pending = 1;
    s->prepare_until = pin_stub_now() + 2.0; /* spec: 2s PREPARING */
    s->signal = 0;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    pin_evtq_push(&s->events, PIN_EVT_STATE, (int32_t)PIN_STATE_PREPARING, "input switch");
    return PIN_OK;
}

/* ==== analog controls ======================================================== */

typedef struct { int32_t min, max, step, def; const char *label; } ctl_range_t;

static const ctl_range_t k_ranges[PIN_CTL_COUNT] = {
    [PIN_CTL_BRIGHTNESS] = {0, 255, 1, 128, "Brightness"},
    [PIN_CTL_CONTRAST]   = {0, 127, 1, 64,  "Contrast"},
    [PIN_CTL_SATURATION] = {0, 127, 1, 64,  "Saturation"},
    [PIN_CTL_HUE]        = {-128, 127, 1, 0, "Hue"},
    [PIN_CTL_SHARPNESS]  = {0, 3, 1, 0,     "Sharpness"},
    [PIN_CTL_AUDIO_GAIN] = {-345, 120, 15, 0, "Audio gain"},
};

const char *pin_std_name(pin_std_t std) {
    switch (std) {
    case PIN_STD_AUTO: return "Auto";
    case PIN_STD_PAL: return "PAL";
    case PIN_STD_NTSC: return "NTSC";
    case PIN_STD_PAL_M: return "PAL-M";
    case PIN_STD_PAL_N: return "PAL-N";
    case PIN_STD_PAL_60: return "PAL-60";
    case PIN_STD_NTSC_443: return "NTSC-443";
    case PIN_STD_NTSC_J: return "NTSC-J";
    case PIN_STD_SECAM: return "SECAM";
    default: return "?";
    }
}

static int std_is_ntsc_family(pin_std_t std) {
    return std == PIN_STD_NTSC || std == PIN_STD_NTSC_443 || std == PIN_STD_NTSC_J;
}

pin_status_t pin_set_standard(pin_session_t *s, pin_std_t std) {
    if (!s || std < 0 || std >= PIN_STD_COUNT) return PIN_ERR_ARG;
    pthread_mutex_lock(&s->lock);
    s->requested_std = std;
    if (std == PIN_STD_AUTO) {
        /* Design choice for this stub: PIN_STD_AUTO's *effective* detected
         * standard defaults to NTSC (there is no real decoder to ask). A
         * real core would read the 50/60Hz detector here. */
        s->detected_std = PIN_STD_NTSC;
        s->is_60hz = 1;
    } else {
        s->detected_std = std;
        s->is_60hz = std_is_ntsc_family(std) || std == PIN_STD_PAL_60 || std == PIN_STD_PAL_M;
    }
    pthread_mutex_unlock(&s->lock);
    pin_evtq_push(&s->events, PIN_EVT_INPUT_FORMAT, (int32_t)std, "standard changed");
    return PIN_OK;
}

pin_status_t pin_get_control(pin_session_t *s, pin_control_t c, pin_control_info_t *out) {
    if (!s || !out || c < 0 || c >= PIN_CTL_COUNT) return PIN_ERR_ARG;
    if (out->size != sizeof(*out)) return PIN_ERR_ABI;
    pthread_mutex_lock(&s->lock);
    const ctl_range_t *r = &k_ranges[c];
    memset(out, 0, sizeof(*out));
    out->size = sizeof(*out);
    strncpy(out->label, r->label, sizeof(out->label) - 1);
    out->min = r->min; out->max = r->max; out->step = r->step; out->def = r->def;
    out->value = s->controls[c];
    if (c == PIN_CTL_HUE) out->enabled = std_is_ntsc_family(s->detected_std);
    else out->enabled = 1;
    pthread_mutex_unlock(&s->lock);
    return PIN_OK;
}

pin_status_t pin_set_control(pin_session_t *s, pin_control_t c, int32_t value) {
    if (!s || c < 0 || c >= PIN_CTL_COUNT) return PIN_ERR_ARG;
    const ctl_range_t *r = &k_ranges[c];
    if (value < r->min) value = r->min;
    if (value > r->max) value = r->max;
    if (r->step > 1) {
        int32_t off = value - r->min;
        int32_t rounded = (off + r->step / 2) / r->step * r->step;
        value = r->min + rounded;
        if (value > r->max) value -= r->step;
    }
    pthread_mutex_lock(&s->lock);
    s->controls[c] = value;
    pthread_mutex_unlock(&s->lock);
    return PIN_OK;
}

/* ==== status / events ======================================================== */

void pin_stub_set_state(pin_session_t *s, pin_state_t st) {
    /* caller holds s->lock */
    if (s->state != st) {
        s->state = st;
        pin_evtq_push(&s->events, PIN_EVT_STATE, (int32_t)st, "");
    }
}

pin_status_t pin_get_status(pin_session_t *s, pin_status_snapshot_t *out) {
    if (!s || !out) return PIN_ERR_ARG;
    if (!pin_status_size_ok(out->size)) return PIN_ERR_ABI;
    uint32_t caller_size = out->size;
    pthread_mutex_lock(&s->lock);
    memset(out, 0, caller_size);
    out->size = caller_size;
    out->state = s->state;
    out->last_error = s->last_error;
    strncpy(out->error_text, s->error_text, sizeof(out->error_text) - 1);
    out->input = s->input;
    out->stream_kind = s->stream_kind;
    out->signal = s->signal;
    out->is_60hz = s->is_60hz;
    out->width = s->width;
    out->height = s->height;
    out->dar_num = s->dar_num;
    out->dar_den = s->dar_den;
    out->detected_std = s->detected_std;

    out->deck = s->deck.state;
    out->deck_busy = s->deck.busy;
    {
        long long f = s->deck.tc_frames;
        int fps = 25;
        long long total_secs = f / fps;
        int ff = (int)(f % fps);
        int hh = (int)(total_secs / 3600);
        int mm = (int)((total_secs / 60) % 60);
        int ss = (int)(total_secs % 60);
        if (s->deck.no_tape) out->timecode[0] = 0;
        else snprintf(out->timecode, sizeof(out->timecode), "%02d:%02d:%02d:%02d", hh, mm, ss, ff);
    }
    out->rec_datetime[0] = 0;
    out->tape_percent = s->deck.no_tape ? -1 : (int)s->deck.tape_percent;

    out->pass = s->cap.pass;
    out->passes = s->cap.passes;
    out->scene = s->cap.scene;
    strncpy(out->current_file, s->cap.path, sizeof(out->current_file) - 1);
    out->elapsed_s = s->cap.active ? (pin_stub_now() - s->cap.start_time) : 0.0;
    out->frames = s->cap.frames;
    out->bytes_written = s->cap.bytes_written;
    out->writer_backlog = 0;
    out->writer_backlog_max = 64 * 1024 * 1024;
    out->idle_s = 0.0;

    for (int ch = 0; ch < 2; ch++) {
        double t = pin_stub_now();
        double phase = t * 0.7 + ch * 1.1;
        double peak = -23.0 + 17.0 * sin(phase);
        double rms = peak - 6.0;
        out->audio_peak_db[ch] = (float)peak;
        out->audio_rms_db[ch] = (float)rms;
    }
    {
        uint64_t fb; double secs;
        pin_stub_estimate_disk(s, &fb, &secs);
        const uint64_t low_bytes = (uint64_t)50 * 1024 * 1024 * 1024;
        int low = fb > 0 && (fb < low_bytes || (secs >= 0 && secs < 3600.0));
        pin_status_set_disk(out, fb, secs, low);
    }
    pthread_mutex_unlock(&s->lock);
    return PIN_OK;
}

void pin_format_status_line(const pin_status_snapshot_t *st, char *out, size_t cap) {
    if (!st || !out || cap == 0) return;
    const char *state_name = "?";
    switch (st->state) {
    case PIN_STATE_CLOSED: state_name = "Closed"; break;
    case PIN_STATE_PREPARING: state_name = "Preparing"; break;
    case PIN_STATE_READY: state_name = st->signal ? "Ready (signal)" : "Ready (no signal)"; break;
    case PIN_STATE_CAPTURING: state_name = "Capturing"; break;
    case PIN_STATE_STOPPING: state_name = "Stopping"; break;
    case PIN_STATE_REWINDING: state_name = "Rewinding"; break;
    case PIN_STATE_ERROR: state_name = "Error"; break;
    }
    if (st->state == PIN_STATE_CAPTURING) {
        snprintf(out, cap, "%s  %s  %llu frames  %.1f MB  pass %d/%d",
                 state_name, st->timecode[0] ? st->timecode : "--:--:--:--",
                 (unsigned long long)st->frames, st->bytes_written / (1024.0 * 1024.0),
                 st->pass, st->passes);
    } else {
        snprintf(out, cap, "%s%s%s", state_name,
                 st->timecode[0] ? "  " : "", st->timecode[0] ? st->timecode : "");
    }
}

void pin_format_window_title(const pin_status_snapshot_t *st, const char *device_name,
                              char *out, size_t cap) {
    if (!st || !out || cap == 0) return;
    const char *dev = device_name ? device_name : "";
    if (st->state == PIN_STATE_CAPTURING) {
        snprintf(out, cap, "\xE2\x97\x8F REC %s \xE2\x80\x94 %s",
                 st->timecode[0] ? st->timecode : "--:--:--:--", dev);
    } else {
        snprintf(out, cap, "%s", dev);
    }
}

int pin_poll_event(pin_session_t *s, pin_event_t *out) {
    if (!out) return 0;
    if (!s) return 0; /* stub has no process-wide log queue */
    return pin_evtq_pop(&s->events, out);
}

static int g_log_level = 1;
void pin_set_log_level(int level) { g_log_level = level; (void)g_log_level; }

/* Device-change waiting: the stub's device list never changes, so just
 * sleep out the timeout (or until woken). */
static volatile int g_stub_wake;

int pin_devices_wait(int timeout_ms) {
    for (int t = 0; t < timeout_ms && !g_stub_wake; t += 50) {
        struct timespec ts = { 0, 50 * 1000000L };
        nanosleep(&ts, NULL);
    }
    g_stub_wake = 0;
    return 0;
}

void pin_devices_wake(void) { g_stub_wake = 1; }

void pin_set_replay_file(const char *path) { (void)path; }
