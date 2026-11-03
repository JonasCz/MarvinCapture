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
 * The preview renderer: a SurfaceView's ANativeWindow, EGL + OpenGL ES 3.0, the Y / Cb / Cr
 * planes as three R8 textures and the core's 3x4 matrix (pin_yuv_to_rgb_matrix) in the
 * fragment shader; the picture is letterboxed with pin_fit_rect. Port of the macOS
 * PreviewRenderer (Metal) and PreviewShaders.
 *
 * Threads and pacing
 * - One render thread per renderer owns EGL, GL and an ALooper with the AChoreographer.
 *   While nothing arrives it blocks in pin_preview_wait (50 ms slices), so an idle or
 *   signal-less preview costs nothing. Once a frame is queued it asks the choreographer for
 *   frame callbacks and, per display refresh, calls pin_preview_lock_due with the time that
 *   refresh's picture reaches the screen (the callback's frame time, CLOCK_MONOTONIC like
 *   pin_clock_now(), plus one refresh period), uploads, unlocks, draws and swaps. When the
 *   core has nothing queued any more it goes back to waiting.
 * - Session lifetime (as on macOS): the render thread only enters the core through
 *   enter_core(), which hands out the session while it is attached, and marks in_core.
 *   mr_detach() is synchronous: when it returns the thread is out of the core.
 * - Surface lifetime: mr_set_window() (SurfaceHolder callbacks, main thread) is synchronous
 *   too: it hands the window over and waits until the render thread has dropped the old EGL
 *   surface, so surfaceDestroyed can return safely.
 */

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/choreographer.h>
#include <android/looper.h>
#include <android/native_window_jni.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "jni_util.h"
#include "pin_api.h"

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t thread;
    int running;

    /* guarded by mu */
    pin_session_t *session;
    int in_core;
    int visible;
    ANativeWindow *pending_window;   /* handed over by the main thread (its own reference) */
    int window_seq, window_applied;
    int clear_requested;
    uint64_t last_seq;
    double last_frame_clock;         /* pin_clock_now() of the last presented frame, 0 = none */
    int frame_dar_num, frame_dar_den;
    ALooper *looper;

    /* render thread only */
    ANativeWindow *window;
    EGLDisplay dpy;
    EGLConfig cfg;
    EGLContext ctx;
    EGLSurface surf;
    GLuint prog, tex[3];
    GLint u_m[3], u_tex[3];
    int tw, th, tcw, tch;
    int have_frame;
    float matrix[12];
    int dar_num, dar_den;
    AChoreographer *choreo;
    int cb_pending;
    int tick_pending;
    int64_t tick_ns;
    int64_t period_ns;
} renderer_t;

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}

/* ---- GL ----------------------------------------------------------------------------------- */

static const char *VS =
    "#version 300 es\n"
    "out vec2 uv;\n"
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    uv = p;\n"
    "    gl_Position = vec4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);\n"
    "}\n";

/* Bilinear + clamped sampling does the DAR scaling and the chroma upsample (4:2:2 / 4:1:1 /
 * 4:2:0); the three rows are pin_yuv_to_rgb_matrix's 12 floats in order. */
static const char *FS =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 uv;\n"
    "uniform sampler2D texY, texU, texV;\n"
    "uniform vec4 m0, m1, m2;\n"
    "out vec4 color;\n"
    "void main() {\n"
    "    vec4 yuv = vec4(texture(texY, uv).r, texture(texU, uv).r, texture(texV, uv).r, 1.0);\n"
    "    color = vec4(clamp(vec3(dot(m0, yuv), dot(m1, yuv), dot(m2, yuv)), 0.0, 1.0), 1.0);\n"
    "}\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        MJ_LOGE("preview shader: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static int gl_init(renderer_t *r)
{
    if (r->prog)
        return 1;
    GLuint vs = compile(GL_VERTEX_SHADER, VS), fs = compile(GL_FRAGMENT_SHADER, FS);
    if (!vs || !fs)
        return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        MJ_LOGE("preview program did not link");
        glDeleteProgram(p);
        return 0;
    }
    r->prog = p;
    static const char *mn[3] = { "m0", "m1", "m2" }, *tn[3] = { "texY", "texU", "texV" };
    for (int i = 0; i < 3; i++) {
        r->u_m[i] = glGetUniformLocation(p, mn[i]);
        r->u_tex[i] = glGetUniformLocation(p, tn[i]);
    }
    glGenTextures(3, r->tex);
    for (int i = 0; i < 3; i++) {
        glBindTexture(GL_TEXTURE_2D, r->tex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    r->tw = r->th = 0;
    return 1;
}

static int egl_init(renderer_t *r)
{
    if (r->ctx != EGL_NO_CONTEXT)
        return 1;
    r->dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (r->dpy == EGL_NO_DISPLAY || !eglInitialize(r->dpy, NULL, NULL)) {
        MJ_LOGE("preview: no EGL display");
        return 0;
    }
    const EGLint attrs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE,
    };
    EGLint n = 0;
    if (!eglChooseConfig(r->dpy, attrs, &r->cfg, 1, &n) || n < 1) {
        MJ_LOGE("preview: no EGL config for OpenGL ES 3");
        return 0;
    }
    const EGLint cattrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    r->ctx = eglCreateContext(r->dpy, r->cfg, EGL_NO_CONTEXT, cattrs);
    if (r->ctx == EGL_NO_CONTEXT) {
        MJ_LOGE("preview: eglCreateContext failed (0x%x)", eglGetError());
        return 0;
    }
    return 1;
}

static void drop_surface(renderer_t *r)
{
    if (r->surf != EGL_NO_SURFACE) {
        eglMakeCurrent(r->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(r->dpy, r->surf);
        r->surf = EGL_NO_SURFACE;
    }
    if (r->window) {
        ANativeWindow_release(r->window);
        r->window = NULL;
    }
}

static void draw(renderer_t *r, int with_frame)
{
    if (r->surf == EGL_NO_SURFACE)
        return;
    EGLint w = 0, h = 0;
    eglQuerySurface(r->dpy, r->surf, EGL_WIDTH, &w);
    eglQuerySurface(r->dpy, r->surf, EGL_HEIGHT, &h);
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (with_frame && r->have_frame && r->prog) {
        int x = 0, y = 0, rw = w, rh = h;
        pin_fit_rect(r->dar_num, r->dar_den, w, h, &x, &y, &rw, &rh);
        glViewport(x, h - y - rh, rw > 0 ? rw : 1, rh > 0 ? rh : 1);   /* GL's origin is bottom left */
        glUseProgram(r->prog);
        for (int i = 0; i < 3; i++) {
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, r->tex[i]);
            glUniform1i(r->u_tex[i], i);
            glUniform4fv(r->u_m[i], 1, r->matrix + 4 * i);
        }
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    if (!eglSwapBuffers(r->dpy, r->surf))
        MJ_LOGW("preview: eglSwapBuffers failed (0x%x)", eglGetError());
}

static void upload_plane(int w, int h, int stride, const uint8_t *p)
{
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, stride);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED, GL_UNSIGNED_BYTE, p);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

static int upload(renderer_t *r, const pin_frame_t *f)
{
    int w = f->width, h = f->height;
    if (w <= 0 || h <= 0 || !f->plane[0] || !f->plane[1] || !f->plane[2] || !r->prog)
        return 0;
    int cw = w >> f->chroma_shift_x, ch = h >> f->chroma_shift_y;
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    if (w != r->tw || h != r->th || cw != r->tcw || ch != r->tch) {
        int dims[3][2] = { { w, h }, { cw, ch }, { cw, ch } };
        for (int i = 0; i < 3; i++) {
            glBindTexture(GL_TEXTURE_2D, r->tex[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, dims[i][0], dims[i][1], 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
        }
        r->tw = w; r->th = h; r->tcw = cw; r->tch = ch;
    }
    glBindTexture(GL_TEXTURE_2D, r->tex[0]);
    upload_plane(w, h, f->stride[0], f->plane[0]);
    glBindTexture(GL_TEXTURE_2D, r->tex[1]);
    upload_plane(cw, ch, f->stride[1], f->plane[1]);
    glBindTexture(GL_TEXTURE_2D, r->tex[2]);
    upload_plane(cw, ch, f->stride[2], f->plane[2]);
    pin_yuv_to_rgb_matrix(f->matrix, f->full_range, r->matrix);
    r->dar_num = f->dar_num > 0 ? f->dar_num : 4;
    r->dar_den = f->dar_den > 0 ? f->dar_den : 3;
    r->have_frame = 1;
    return 1;
}

/* ---- window handover (render thread, mu held) ------------------------------------------------- */

static void apply_window(renderer_t *r)
{
    if (r->window_applied == r->window_seq)
        return;
    ANativeWindow *w = r->pending_window;
    r->pending_window = NULL;
    drop_surface(r);
    if (w) {
        if (egl_init(r)) {
            r->surf = eglCreateWindowSurface(r->dpy, r->cfg, w, NULL);
            if (r->surf == EGL_NO_SURFACE) {
                MJ_LOGE("preview: eglCreateWindowSurface failed (0x%x)", eglGetError());
                ANativeWindow_release(w);
            } else {
                r->window = w;
                eglMakeCurrent(r->dpy, r->surf, r->surf, r->ctx);
                eglSwapInterval(r->dpy, 1);
                if (gl_init(r))
                    draw(r, 1);   /* the last picture at the new size, or black */
            }
        } else {
            ANativeWindow_release(w);
        }
    }
    r->window_applied = r->window_seq;
    pthread_cond_broadcast(&r->cv);
}

/* ---- choreographer -------------------------------------------------------------------------- */

static void frame_cb(int64_t frame_ns, void *data)
{
    renderer_t *r = data;
    r->cb_pending = 0;
    r->tick_pending = 1;
    r->tick_ns = frame_ns;
}

static void refresh_cb(int64_t period_ns, void *data)
{
    renderer_t *r = data;
    if (period_ns > 2000000 && period_ns < 100000000)
        r->period_ns = period_ns;
}

static void request_tick(renderer_t *r)
{
    if (r->choreo && !r->cb_pending) {
        r->cb_pending = 1;
        AChoreographer_postFrameCallback64(r->choreo, frame_cb, r);
    }
}

/* ---- render thread ---------------------------------------------------------------------------- */

static pin_session_t *enter_core(renderer_t *r)
{
    pthread_mutex_lock(&r->mu);
    pin_session_t *s = NULL;
    if (r->running && r->visible && r->session && r->surf != EGL_NO_SURFACE &&
        r->window_applied == r->window_seq) {
        s = r->session;
        r->in_core = 1;
    }
    pthread_mutex_unlock(&r->mu);
    return s;
}

static void leave_core(renderer_t *r)
{
    pthread_mutex_lock(&r->mu);
    r->in_core = 0;
    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->mu);
}

/* Frames are queued: one per display refresh as they come due. */
static void present_loop(renderer_t *r)
{
    for (;;) {
        request_tick(r);
        r->tick_pending = 0;
        /* Up to 100 ms for the refresh (none comes while the display is off: then the clock). */
        ALooper_pollOnce(100, NULL, NULL, NULL);
        pthread_mutex_lock(&r->mu);
        apply_window(r);
        pthread_mutex_unlock(&r->mu);

        double now = r->tick_pending ? (double)(r->tick_ns + r->period_ns) / 1e9 : pin_clock_now();
        pin_session_t *s = enter_core(r);
        if (!s)
            return;
        pin_frame_t f;
        memset(&f, 0, sizeof(f));
        f.size = sizeof(f);
        if (pin_preview_lock_due(s, now, &f) == PIN_OK) {
            int ok = upload(r, &f);
            pin_preview_unlock(s);   /* right after the upload; drawing needs only the textures */
            if (ok) {
                draw(r, 1);
                pthread_mutex_lock(&r->mu);
                r->last_seq = f.seq;
                r->last_frame_clock = pin_clock_now();
                r->frame_dar_num = r->dar_num;
                r->frame_dar_den = r->dar_den;
                pthread_mutex_unlock(&r->mu);
            }
        }
        double t = 0;
        int more = pin_preview_next_time(s, &t);
        leave_core(r);
        if (!more)
            return;
    }
}

static void *render_main(void *arg)
{
    renderer_t *r = arg;
    pthread_setname_np(pthread_self(), "Preview render");
    ALooper *looper = ALooper_prepare(0);
    r->choreo = AChoreographer_getInstance();
    if (r->choreo)
        AChoreographer_registerRefreshRateCallback(r->choreo, refresh_cb, r);
    pthread_mutex_lock(&r->mu);
    r->looper = looper;
    ALooper_acquire(looper);
    pthread_cond_broadcast(&r->cv);

    while (r->running) {
        apply_window(r);
        if (r->clear_requested) {
            r->clear_requested = 0;
            r->have_frame = 0;
            draw(r, 0);
        }
        if (!r->session || !r->visible || r->surf == EGL_NO_SURFACE) {
            /* parked; the choreographer's callbacks can wait */
            pthread_cond_wait(&r->cv, &r->mu);
            continue;
        }
        pin_session_t *s = r->session;
        uint64_t seq = r->last_seq;
        r->in_core = 1;
        pthread_mutex_unlock(&r->mu);
        int got = pin_preview_wait(s, seq, 50);
        pthread_mutex_lock(&r->mu);
        r->in_core = 0;
        pthread_cond_broadcast(&r->cv);
        if (got < 0) {
            pthread_mutex_unlock(&r->mu);
            sleep_ms(20);   /* closing: the service detaches first */
            pthread_mutex_lock(&r->mu);
            continue;
        }
        if (got == 0)
            continue;
        pthread_mutex_unlock(&r->mu);
        present_loop(r);
        pthread_mutex_lock(&r->mu);
    }
    /* end: release GL / EGL on this thread */
    if (r->pending_window) {
        ANativeWindow_release(r->pending_window);
        r->pending_window = NULL;
    }
    pthread_mutex_unlock(&r->mu);
    if (r->surf != EGL_NO_SURFACE && r->prog) {
        glDeleteTextures(3, r->tex);
        glDeleteProgram(r->prog);
    }
    drop_surface(r);
    if (r->ctx != EGL_NO_CONTEXT)
        eglDestroyContext(r->dpy, r->ctx);
    if (r->choreo)
        AChoreographer_unregisterRefreshRateCallback(r->choreo, refresh_cb, r);
    pthread_mutex_lock(&r->mu);
    r->window_applied = r->window_seq;
    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->mu);
    return NULL;
}

static void wake(renderer_t *r)
{
    pthread_cond_broadcast(&r->cv);
    if (r->looper)
        ALooper_wake(r->looper);
}

/* ---- JNI -------------------------------------------------------------------------------------- */

static renderer_t *R(jlong h) { return (renderer_t *)(intptr_t)h; }

MJ_FN(jlong, previewCreate)(JNIEnv *env, jclass c)
{
    (void)env; (void)c;
    renderer_t *r = calloc(1, sizeof(*r));
    if (!r)
        return 0;
    pthread_mutex_init(&r->mu, NULL);
    pthread_cond_init(&r->cv, NULL);
    r->running = 1;
    r->visible = 1;
    r->dpy = EGL_NO_DISPLAY;
    r->ctx = EGL_NO_CONTEXT;
    r->surf = EGL_NO_SURFACE;
    r->period_ns = 16666667;
    r->dar_num = 4;
    r->dar_den = 3;
    if (pthread_create(&r->thread, NULL, render_main, r) != 0) {
        free(r);
        return 0;
    }
    pthread_mutex_lock(&r->mu);
    while (!r->looper)
        pthread_cond_wait(&r->cv, &r->mu);
    pthread_mutex_unlock(&r->mu);
    return (jlong)(intptr_t)r;
}

/* surface: the SurfaceView's Surface, or null when it is destroyed. Synchronous. */
MJ_FN(void, previewSetSurface)(JNIEnv *env, jclass c, jlong h, jobject surface)
{
    (void)c;
    renderer_t *r = R(h);
    if (!r)
        return;
    ANativeWindow *w = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
    pthread_mutex_lock(&r->mu);
    if (r->pending_window)
        ANativeWindow_release(r->pending_window);
    r->pending_window = w;
    int seq = ++r->window_seq;
    wake(r);
    while (r->running && r->window_applied != seq)
        pthread_cond_wait(&r->cv, &r->mu);
    pthread_mutex_unlock(&r->mu);
}

MJ_FN(void, previewAttach)(JNIEnv *env, jclass c, jlong h, jlong session)
{
    (void)env; (void)c;
    renderer_t *r = R(h);
    if (!r)
        return;
    pthread_mutex_lock(&r->mu);
    r->session = (pin_session_t *)(intptr_t)session;
    r->last_seq = 0;
    r->last_frame_clock = 0;
    if (r->session)
        pin_preview_enable(r->session, r->visible);
    wake(r);
    pthread_mutex_unlock(&r->mu);
}

/* Synchronous: when this returns the render thread is out of the core and will not enter it
 * again with the old session. The picture goes black. */
MJ_FN(void, previewDetach)(JNIEnv *env, jclass c, jlong h)
{
    (void)env; (void)c;
    renderer_t *r = R(h);
    if (!r)
        return;
    pthread_mutex_lock(&r->mu);
    r->session = NULL;
    wake(r);
    while (r->in_core)
        pthread_cond_wait(&r->cv, &r->mu);
    r->last_seq = 0;
    r->last_frame_clock = 0;
    r->frame_dar_num = r->frame_dar_den = 0;
    r->clear_requested = 1;
    wake(r);
    pthread_mutex_unlock(&r->mu);
}

/* The activity is not visible: stop the core's decoding and park the thread. */
MJ_FN(void, previewSetVisible)(JNIEnv *env, jclass c, jlong h, jboolean visible)
{
    (void)env; (void)c;
    renderer_t *r = R(h);
    if (!r)
        return;
    pthread_mutex_lock(&r->mu);
    r->visible = visible ? 1 : 0;
    if (r->session)
        pin_preview_enable(r->session, r->visible);
    wake(r);
    pthread_mutex_unlock(&r->mu);
}

/* Returns 1 if a frame was presented within the last second; dar[0..1] = its display aspect
 * (0 / 0 before the first one). */
MJ_FN(jint, previewInfo)(JNIEnv *env, jclass c, jlong h, jintArray dar)
{
    (void)c;
    renderer_t *r = R(h);
    if (!r)
        return 0;
    pthread_mutex_lock(&r->mu);
    int recent = r->last_frame_clock > 0 && pin_clock_now() - r->last_frame_clock < 1.0;
    jint d[2] = { r->frame_dar_num, r->frame_dar_den };
    pthread_mutex_unlock(&r->mu);
    if (dar && (*env)->GetArrayLength(env, dar) >= 2)
        (*env)->SetIntArrayRegion(env, dar, 0, 2, d);
    return recent;
}
