/*
 * pin_stub_preview.c — the preview frame generator thread, the
 * pin_preview_* API, pin_fit_rect, pin_yuv_to_rgb_matrix and the audio
 * monitor ring.
 *
 * Frames are classic 7-bar (75%) colour bars, computed directly in
 * planar YCbCr (no RGB roundtrip), that scroll horizontally over time so
 * motion is visible. Generated at 25fps into a triple buffer with a seq
 * counter; pin_preview_wait() blocks on a condvar rather than spinning.
 */
#include "pin_stub.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define PREVIEW_FPS 25
#define PREVIEW_PERIOD_MS (1000 / PREVIEW_FPS)

/* 75% SMPTE-ish bars, limited-range 8-bit YCbCr, left to right. */
static const uint8_t k_bar_y[7]  = {235, 210, 170, 145, 106, 81, 41};
static const uint8_t k_bar_cb[7] = {128, 16, 166, 54, 202, 90, 240};
static const uint8_t k_bar_cr[7] = {128, 146, 16, 34, 222, 240, 110};

static int ensure_plane_capacity(pin_stub_frame_t *f, int w, int h, int cx, int cy) {
    int cw = w >> cx, ch = h >> cy;
    size_t needed[3] = { (size_t)w * h, (size_t)cw * ch, (size_t)cw * ch };
    for (int p = 0; p < 3; p++) {
        if (!f->plane[p] || f->plane_cap[p] < needed[p]) {
            free(f->plane[p]);
            size_t alloc_n = needed[p] > 0 ? needed[p] : 1;
            f->plane[p] = (uint8_t *)malloc(alloc_n);
            if (!f->plane[p]) { f->plane_cap[p] = 0; return 0; }
            f->plane_cap[p] = alloc_n;
        }
    }
    f->stride[0] = w;
    f->stride[1] = cw;
    f->stride[2] = cw;
    return 1;
}

static void render_bars(pin_stub_frame_t *f, int w, int h, int cx, int cy,
                         pin_matrix_t matrix, int dar_num, int dar_den, double t) {
    int cw = w >> cx, ch = h >> cy;
    double speed_px_per_s = w / 6.0; /* full scroll every ~6s */
    int shift = ((int)(t * speed_px_per_s)) % w;
    if (shift < 0) shift += w;

    uint8_t *Y = f->plane[0], *Cb = f->plane[1], *Cr = f->plane[2];
    for (int y = 0; y < h; y++) {
        uint8_t *row = Y + (size_t)y * f->stride[0];
        for (int x = 0; x < w; x++) {
            int sx = (x + shift) % w;
            int bar = sx * 7 / w;
            if (bar > 6) bar = 6;
            row[x] = k_bar_y[bar];
        }
    }
    for (int y = 0; y < ch; y++) {
        uint8_t *rowu = Cb + (size_t)y * f->stride[1];
        uint8_t *rowv = Cr + (size_t)y * f->stride[2];
        for (int x = 0; x < cw; x++) {
            int lx = x << cx;
            int sx = (lx + shift) % w;
            int bar = sx * 7 / w;
            if (bar > 6) bar = 6;
            rowu[x] = k_bar_cb[bar];
            rowv[x] = k_bar_cr[bar];
        }
    }

    f->width = w; f->height = h;
    f->chroma_shift_x = cx; f->chroma_shift_y = cy;
    f->matrix = matrix;
    f->full_range = 0;
    f->dar_num = dar_num; f->dar_den = dar_den;
    f->interlaced = 1;
    f->top_field_first = 1;
}

void *pin_stub_preview_main(void *arg) {
    pin_session_t *s = (pin_session_t *)arg;
    double next_frame_at = pin_stub_now();

    for (;;) {
        pthread_mutex_lock(&s->preview_lock);
        while (!s->closing && !s->preview_enabled) {
            pthread_cond_wait(&s->preview_cond, &s->preview_lock);
        }
        int closing = s->closing;
        pthread_mutex_unlock(&s->preview_lock);
        if (closing) break;

        pthread_mutex_lock(&s->lock);
        pin_kind_t kind = s->stream_kind;
        int is_60hz = s->is_60hz;
        pin_aspect_t aspect = s->aspect;
        int have_signal = (s->state == PIN_STATE_READY || s->state == PIN_STATE_CAPTURING) && s->signal;
        pthread_mutex_unlock(&s->lock);

        if (!have_signal) {
            pin_stub_sleep_ms(PREVIEW_PERIOD_MS);
            continue;
        }

        int w, h, darn, dard, cx, cy;
        pin_matrix_t matrix;
        pin_stub_kind_geometry(kind, is_60hz, aspect, &w, &h, &darn, &dard, &cx, &cy, &matrix);

        pthread_mutex_lock(&s->preview_lock);
        int idx = -1;
        for (int i = 0; i < 3; i++) {
            if (i != s->frame_front && i != s->locked_index) { idx = i; break; }
        }
        pthread_mutex_unlock(&s->preview_lock);
        if (idx < 0) { pin_stub_sleep_ms(PREVIEW_PERIOD_MS); continue; }

        pin_stub_frame_t *f = &s->frame[idx];
        if (!ensure_plane_capacity(f, w, h, cx, cy)) { pin_stub_sleep_ms(PREVIEW_PERIOD_MS); continue; }
        render_bars(f, w, h, cx, cy, matrix, darn, dard, pin_stub_now() - s->opened_at);

        pthread_mutex_lock(&s->preview_lock);
        s->preview_seq++;
        f->seq = s->preview_seq;
        s->frame_front = idx;
        pthread_cond_broadcast(&s->preview_cond);
        pthread_mutex_unlock(&s->preview_lock);

        next_frame_at += PREVIEW_PERIOD_MS / 1000.0;
        double now = pin_stub_now();
        double sleep_s = next_frame_at - now;
        if (sleep_s < 0) { next_frame_at = now; sleep_s = 0; }
        pin_stub_sleep_ms((int)(sleep_s * 1000));
    }
    return NULL;
}

int pin_preview_wait(pin_session_t *s, uint64_t after_seq, int timeout_ms) {
    if (!s) return -1;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }

    pthread_mutex_lock(&s->preview_lock);
    int rc = 1;
    while (!s->closing && s->preview_seq <= after_seq) {
        int r = pthread_cond_timedwait(&s->preview_cond, &s->preview_lock, &ts);
        if (r != 0) { rc = 0; break; } /* timed out */
    }
    if (s->closing) rc = -1;
    pthread_mutex_unlock(&s->preview_lock);
    return rc;
}

pin_status_t pin_preview_lock(pin_session_t *s, pin_frame_t *out) {
    if (!s || !out) return PIN_ERR_ARG;
    pthread_mutex_lock(&s->preview_lock);
    if (s->preview_seq == 0) {
        pthread_mutex_unlock(&s->preview_lock);
        return PIN_ERR_STATE;
    }
    int idx = s->frame_front;
    s->locked_index = idx;
    pin_stub_frame_t *f = &s->frame[idx];
    uint32_t caller_size = out->size;
    memset(out, 0, sizeof(*out));
    out->size = caller_size;
    out->seq = f->seq;
    out->width = f->width; out->height = f->height;
    out->chroma_shift_x = f->chroma_shift_x; out->chroma_shift_y = f->chroma_shift_y;
    out->plane[0] = f->plane[0]; out->plane[1] = f->plane[1]; out->plane[2] = f->plane[2];
    out->stride[0] = f->stride[0]; out->stride[1] = f->stride[1]; out->stride[2] = f->stride[2];
    out->matrix = f->matrix;
    out->full_range = f->full_range;
    out->dar_num = f->dar_num; out->dar_den = f->dar_den;
    out->interlaced = f->interlaced; out->top_field_first = f->top_field_first;
    pthread_mutex_unlock(&s->preview_lock);
    return PIN_OK;
}

void pin_preview_unlock(pin_session_t *s) {
    if (!s) return;
    pthread_mutex_lock(&s->preview_lock);
    s->locked_index = -1;
    pthread_mutex_unlock(&s->preview_lock);
}

void pin_preview_enable(pin_session_t *s, int enabled) {
    if (!s) return;
    pthread_mutex_lock(&s->preview_lock);
    s->preview_enabled = enabled ? 1 : 0;
    pthread_cond_broadcast(&s->preview_cond);
    pthread_mutex_unlock(&s->preview_lock);
}

/* ---- letterbox / colour maths (real math, not faked) ---------------------- */

void pin_fit_rect(int dar_num, int dar_den, int w, int h, int *x, int *y, int *rw, int *rh) {
    if (!x || !y || !rw || !rh) return;
    if (dar_num <= 0 || dar_den <= 0 || w <= 0 || h <= 0) {
        *x = 0; *y = 0; *rw = w; *rh = h;
        return;
    }
    /* largest dar_num:dar_den rect inside w x h, centred */
    double target = (double)dar_num / (double)dar_den;
    double frame_ratio = (double)w / (double)h;
    int cw, ch;
    if (frame_ratio > target) {
        ch = h;
        cw = (int)((double)h * target + 0.5);
    } else {
        cw = w;
        ch = (int)((double)w / target + 0.5);
    }
    if (cw > w) cw = w;
    if (ch > h) ch = h;
    *rw = cw; *rh = ch;
    *x = (w - cw) / 2;
    *y = (h - ch) / 2;
}

void pin_yuv_to_rgb_matrix(pin_matrix_t m, int full_range, float out[12]) {
    if (!out) return;
    /* ITU-R BT.601 / BT.709 kr, kb constants */
    double kr, kb;
    if (m == PIN_MATRIX_BT709) { kr = 0.2126; kb = 0.0722; }
    else { kr = 0.299; kb = 0.114; } /* BT.601 */
    double kg = 1.0 - kr - kb;

    /* R = Y + 2(1-kr) * Cr
       B = Y + 2(1-kb) * Cb
       G = Y - (2*kb*(1-kb)/kg) * Cb - (2*kr*(1-kr)/kg) * Cr
       where Y in [0,1], Cb/Cr in [-0.5, 0.5] here; our input samples are
       normalised 0..1 with Cb/Cr centred at 0.5, so subtract 0.5 via the
       offset column below. */
    double r_cr = 2.0 * (1.0 - kr);
    double b_cb = 2.0 * (1.0 - kb);
    double g_cb = -2.0 * kb * (1.0 - kb) / kg;
    double g_cr = -2.0 * kr * (1.0 - kr) / kg;

    /* Range scaling: limited range 8-bit has Y in [16,235] -> scale 255/219,
     * Cb/Cr in [16,240] centred at 128 -> scale 255/224. Full range: 1:1. */
    double y_scale = full_range ? 1.0 : 255.0 / 219.0;
    double c_scale = full_range ? 1.0 : 255.0 / 224.0;
    double y_off = full_range ? 0.0 : -16.0 / 255.0 * y_scale;

    /* Row-major 3x4: [R;G;B] = M * [Y;Cb;Cr;1] */
    /* R */
    out[0] = (float)(1.0 * y_scale);
    out[1] = (float)(0.0);
    out[2] = (float)(r_cr * c_scale);
    out[3] = (float)(y_off - r_cr * c_scale * 0.5);
    /* G */
    out[4] = (float)(1.0 * y_scale);
    out[5] = (float)(g_cb * c_scale);
    out[6] = (float)(g_cr * c_scale);
    out[7] = (float)(y_off - (g_cb + g_cr) * c_scale * 0.5);
    /* B */
    out[8] = (float)(1.0 * y_scale);
    out[9] = (float)(b_cb * c_scale);
    out[10] = (float)(0.0);
    out[11] = (float)(y_off - b_cb * c_scale * 0.5);
}

/* ---- audio monitor ring ----------------------------------------------------- */

void pin_monitor_enable(pin_session_t *s, int enabled) {
    if (!s) return;
    pthread_mutex_lock(&s->ring_lock);
    s->monitor_enabled = enabled ? 1 : 0;
    if (!enabled) { s->ring_head = 0; s->ring_count = 0; }
    pthread_mutex_unlock(&s->ring_lock);
}

int pin_monitor_read(pin_session_t *s, int16_t *out, int max_frames) {
    if (!s || !out || max_frames <= 0) return 0;
    pthread_mutex_lock(&s->ring_lock);
    if (!s->monitor_enabled || !s->ring) {
        pthread_mutex_unlock(&s->ring_lock);
        return 0;
    }
    /* generate a quiet 440Hz sine tone on demand rather than a background
     * thread: cheap and avoids yet another thread for a stub. */
    int n = max_frames;
    for (int i = 0; i < n; i++) {
        s->audio_phase += 2.0 * M_PI * 440.0 / 48000.0;
        if (s->audio_phase > 2.0 * M_PI) s->audio_phase -= 2.0 * M_PI;
        int16_t sample = (int16_t)(3000.0 * sin(s->audio_phase));
        out[i * 2 + 0] = sample;
        out[i * 2 + 1] = sample;
    }
    pthread_mutex_unlock(&s->ring_lock);
    return n;
}
