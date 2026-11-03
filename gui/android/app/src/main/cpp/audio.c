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
 * Audio with AAudio (NDK):
 *
 * 1. The live monitor: an output stream whose data callback pumps the core's PCM ring
 *    (pin_monitor_read, 48 kHz s16 interleaved stereo) straight into the stream's buffer. Port
 *    of the macOS CoreAudioMonitor: the core bounds the latency, so this is a dumb pump plus the
 *    anti-crackle prebuffer (30 ms) and a restart when the output device changes (headphones).
 *    The callback runs on AAudio's real-time thread: it only TRY-locks the session lock
 *    (silence for that buffer if the main thread holds it) and keeps it for the whole buffer;
 *    audioDetach() takes the lock, so when it returns the callback is out of the core.
 *
 * 2. The keep-alive: a silent low-latency output stream while an analog capture runs. On
 *    several phones an active low-latency stream makes the power HAL hold a CPU wake-up
 *    latency request (Pixel: the AUDIO_STREAMING_LOW_LATENCY power hint, which writes
 *    /dev/cpu_dma_latency), the remedy analog capture needs and an app cannot request itself.
 *    Device dependent; it costs little where it does nothing.
 */

#include <aaudio/AAudio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "jni_util.h"
#include "pin_api.h"

#define RATE 48000
#define PREBUFFER_FRAMES (RATE * 30 / 1000)

typedef struct {
    pthread_mutex_t lock;     /* session + priming; try-locked by the callback */
    pin_session_t *session;
    int primed, prime_frames;

    pthread_mutex_t ctl;      /* stream start / stop / restart */
    AAudioStream *stream;
    int want_running;
} monitor_t;

static aaudio_data_callback_result_t monitor_cb(AAudioStream *st, void *user, void *data, int32_t n)
{
    (void)st;
    monitor_t *m = user;
    int16_t *out = data;
    int played = 0;
    if (pthread_mutex_trylock(&m->lock) == 0) {
        if (m->session) {
            int got = pin_monitor_read(m->session, out, n);
            if (got < 0) got = 0;
            if (got > n) got = n;
            /* After an underrun stay silent until enough consecutive full reads cover the
             * prebuffer, rather than alternating partial audio and silence. */
            int full = got == n;
            if (!m->primed) {
                if (full) {
                    m->prime_frames += got;
                    if (m->prime_frames >= PREBUFFER_FRAMES)
                        m->primed = 1;
                } else {
                    m->prime_frames = 0;
                }
            } else if (got == 0) {
                m->primed = 0;
                m->prime_frames = 0;
            }
            if (m->primed) {
                /* a read a few frames short (clock drift): hold the last sample, which clicks
                 * less than a jump to zero */
                if (got > 0 && got < n) {
                    int16_t l = out[2 * (got - 1)], r = out[2 * (got - 1) + 1];
                    for (int i = got; i < n; i++) { out[2 * i] = l; out[2 * i + 1] = r; }
                }
                played = got > 0 ? n : 0;
            }
        }
        pthread_mutex_unlock(&m->lock);
    }
    if (played < n)
        memset(out + 2 * played, 0, (size_t)(n - played) * 4);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void monitor_error_cb(AAudioStream *st, void *user, aaudio_result_t error);

static AAudioStream *open_output(int low_latency, AAudioStream_dataCallback cb,
                                 AAudioStream_errorCallback ecb, void *user)
{
    AAudioStreamBuilder *b = NULL;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK)
        return NULL;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(b, RATE);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(b, low_latency ? AAUDIO_PERFORMANCE_MODE_LOW_LATENCY
                                                          : AAUDIO_PERFORMANCE_MODE_NONE);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_MEDIA);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_MOVIE);
    AAudioStreamBuilder_setDataCallback(b, cb, user);
    if (ecb)
        AAudioStreamBuilder_setErrorCallback(b, ecb, user);
    AAudioStream *s = NULL;
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &s);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        MJ_LOGW("audio: openStream failed: %s", AAudio_convertResultToText(r));
        return NULL;
    }
    r = AAudioStream_requestStart(s);
    if (r != AAUDIO_OK) {
        MJ_LOGW("audio: requestStart failed: %s", AAudio_convertResultToText(r));
        AAudioStream_close(s);
        return NULL;
    }
    return s;
}

/* ctl held */
static void monitor_close_stream(monitor_t *m)
{
    if (m->stream) {
        AAudioStream_requestStop(m->stream);
        AAudioStream_close(m->stream);
        m->stream = NULL;
        MJ_LOGI("audio monitor: stopped");
    }
}

/* ctl held */
static void monitor_open_stream(monitor_t *m)
{
    if (m->stream || !m->want_running)
        return;
    pthread_mutex_lock(&m->lock);
    m->primed = 0;
    m->prime_frames = 0;
    pthread_mutex_unlock(&m->lock);
    m->stream = open_output(0, monitor_cb, monitor_error_cb, m);
    if (m->stream)
        MJ_LOGI("audio monitor: started (%d Hz, %d frames per burst)",
                AAudioStream_getSampleRate(m->stream), AAudioStream_getFramesPerBurst(m->stream));
}

/* A disconnected stream (output device changed) is reopened on a thread of its own: AAudio
 * forbids closing a stream from its own callback. */
static void *monitor_restart(void *arg)
{
    monitor_t *m = arg;
    pthread_mutex_lock(&m->ctl);
    monitor_close_stream(m);
    monitor_open_stream(m);
    pthread_mutex_unlock(&m->ctl);
    return NULL;
}

static void monitor_error_cb(AAudioStream *st, void *user, aaudio_result_t error)
{
    (void)st;
    MJ_LOGI("audio monitor: stream error %s, restarting", AAudio_convertResultToText(error));
    if (error != AAUDIO_ERROR_DISCONNECTED)
        return;
    pthread_t t;
    if (pthread_create(&t, NULL, monitor_restart, user) == 0)
        pthread_detach(t);
}

static monitor_t *M(jlong h) { return (monitor_t *)(intptr_t)h; }

MJ_FN(jlong, audioCreate)(JNIEnv *env, jclass c)
{
    (void)env; (void)c;
    monitor_t *m = calloc(1, sizeof(*m));
    if (!m)
        return 0;
    pthread_mutex_init(&m->lock, NULL);
    pthread_mutex_init(&m->ctl, NULL);
    return (jlong)(intptr_t)m;
}

MJ_FN(void, audioAttach)(JNIEnv *env, jclass c, jlong h, jlong session)
{
    (void)env; (void)c;
    monitor_t *m = M(h);
    if (!m)
        return;
    pthread_mutex_lock(&m->lock);
    m->session = (pin_session_t *)(intptr_t)session;
    m->primed = 0;
    m->prime_frames = 0;
    pthread_mutex_unlock(&m->lock);
}

/* Synchronous: when this returns the callback has left the core (see the file comment). */
MJ_FN(void, audioDetach)(JNIEnv *env, jclass c, jlong h)
{
    (void)env; (void)c;
    monitor_t *m = M(h);
    if (!m)
        return;
    pthread_mutex_lock(&m->lock);
    m->session = NULL;
    pthread_mutex_unlock(&m->lock);
}

/* Plays the monitor (on = unmuted and attached) or releases the output entirely. */
MJ_FN(void, audioSetRunning)(JNIEnv *env, jclass c, jlong h, jboolean on)
{
    (void)env; (void)c;
    monitor_t *m = M(h);
    if (!m)
        return;
    pthread_mutex_lock(&m->ctl);
    m->want_running = on ? 1 : 0;
    if (on)
        monitor_open_stream(m);
    else
        monitor_close_stream(m);
    pthread_mutex_unlock(&m->ctl);
}

/* ---- keep-alive ----------------------------------------------------------------------------- */

static pthread_mutex_t g_keep_mu = PTHREAD_MUTEX_INITIALIZER;
static AAudioStream *g_keep;

static aaudio_data_callback_result_t silence_cb(AAudioStream *st, void *user, void *data, int32_t n)
{
    (void)st; (void)user;
    memset(data, 0, (size_t)n * 4);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

MJ_FN(void, keepAliveSet)(JNIEnv *env, jclass c, jboolean on)
{
    (void)env; (void)c;
    pthread_mutex_lock(&g_keep_mu);
    if (on && !g_keep) {
        g_keep = open_output(1, silence_cb, NULL, NULL);
        if (g_keep)
            MJ_LOGI("audio keep-alive: started (performance mode %d, %d frames per burst)",
                    AAudioStream_getPerformanceMode(g_keep), AAudioStream_getFramesPerBurst(g_keep));
    } else if (!on && g_keep) {
        AAudioStream_requestStop(g_keep);
        AAudioStream_close(g_keep);
        g_keep = NULL;
        MJ_LOGI("audio keep-alive: stopped");
    } else if (on && g_keep && AAudioStream_getState(g_keep) == AAUDIO_STREAM_STATE_DISCONNECTED) {
        /* the output changed under it: reopen (called again on every service tick) */
        AAudioStream_close(g_keep);
        g_keep = open_output(1, silence_cb, NULL, NULL);
    }
    pthread_mutex_unlock(&g_keep_mu);
}
