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
 * The JNI layer over pin_api.h: thin wrappers, one per call the app needs, as static
 * native methods of jonascz.marvincapture.Core. Sessions travel as jlong handles. The
 * status snapshot goes to Java as a few int / long / double arrays plus the core's
 * ready-made strings (the Java side only shows them), so no struct is mirrored in Java;
 * the index constants are in Core.java (ST_*, SL_*, SD_*, SS_*, OPT_*) and must match the
 * order below. The preview renderer and the audio monitor live in preview.c and audio.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jni_util.h"
#include "pin_api.h"

/* ---- strings ------------------------------------------------------------------------ */

jstring mj_string(JNIEnv *env, const char *s)
{
    if (!s)
        s = "";
    size_t n = strlen(s);
    jchar stackbuf[512];
    jchar *u = n <= 512 ? stackbuf : malloc(n * sizeof(jchar));
    if (!u)
        return (*env)->NewStringUTF(env, "");
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)s, *end = p + n;
    while (p < end) {
        unsigned c = *p;
        unsigned cp;
        int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xe0) == 0xc0) { cp = c & 0x1f; len = 2; }
        else if ((c & 0xf0) == 0xe0) { cp = c & 0x0f; len = 3; }
        else if ((c & 0xf8) == 0xf0) { cp = c & 0x07; len = 4; }
        else { u[o++] = 0xfffd; p++; continue; }
        if (p + len > end) { u[o++] = 0xfffd; break; }
        int ok = 1;
        for (int i = 1; i < len; i++) {
            if ((p[i] & 0xc0) != 0x80) { ok = 0; break; }
            cp = (cp << 6) | (p[i] & 0x3f);
        }
        if (!ok) { u[o++] = 0xfffd; p++; continue; }
        p += len;
        if (cp >= 0x10000 && cp <= 0x10ffff) {
            cp -= 0x10000;
            u[o++] = (jchar)(0xd800 | (cp >> 10));
            u[o++] = (jchar)(0xdc00 | (cp & 0x3ff));
        } else if (cp < 0x10000) {
            u[o++] = (jchar)cp;
        } else {
            u[o++] = 0xfffd;
        }
    }
    jstring r = (*env)->NewString(env, u, (jsize)o);
    if (u != stackbuf)
        free(u);
    return r;
}

int mj_utf8(JNIEnv *env, jstring s, char *out, size_t cap)
{
    if (!out || cap == 0)
        return -1;
    out[0] = 0;
    if (!s)
        return 0;
    jsize n = (*env)->GetStringLength(env, s);
    const jchar *u = (*env)->GetStringChars(env, s, NULL);
    if (!u)
        return -1;
    size_t o = 0;
    int fit = 0;
    for (jsize i = 0; i < n; i++) {
        unsigned cp = u[i];
        if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < n && u[i + 1] >= 0xdc00 && u[i + 1] <= 0xdfff) {
            cp = 0x10000 + ((cp - 0xd800) << 10) + (u[i + 1] - 0xdc00);
            i++;
        } else if (cp >= 0xd800 && cp <= 0xdfff) {
            cp = 0xfffd;
        }
        char b[4];
        size_t len;
        if (cp < 0x80) { b[0] = (char)cp; len = 1; }
        else if (cp < 0x800) { b[0] = (char)(0xc0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3f)); len = 2; }
        else if (cp < 0x10000) {
            b[0] = (char)(0xe0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
            b[2] = (char)(0x80 | (cp & 0x3f)); len = 3;
        } else {
            b[0] = (char)(0xf0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
            b[2] = (char)(0x80 | ((cp >> 6) & 0x3f)); b[3] = (char)(0x80 | (cp & 0x3f)); len = 4;
        }
        if (o + len >= cap) { fit = -1; break; }
        memcpy(out + o, b, len);
        o += len;
    }
    out[o] = 0;
    (*env)->ReleaseStringChars(env, s, u);
    return fit;
}

static pin_session_t *S(jlong h) { return (pin_session_t *)(intptr_t)h; }

/* ---- process-wide ------------------------------------------------------------------- */

MJ_FN(jint, apiVersion)(JNIEnv *env, jclass c) { (void)env; (void)c; return (jint)pin_api_version(); }
MJ_FN(jstring, versionString)(JNIEnv *env, jclass c) { (void)c; return mj_string(env, pin_version_string()); }
MJ_FN(jstring, appVersion)(JNIEnv *env, jclass c) { (void)c; return mj_string(env, pin_app_version()); }
MJ_FN(jstring, strerror)(JNIEnv *env, jclass c, jint st) { (void)c; return mj_string(env, pin_strerror((pin_status_t)st)); }
MJ_FN(jstring, noDevicesHint)(JNIEnv *env, jclass c) { (void)c; return mj_string(env, pin_no_devices_hint()); }

MJ_FN(jint, setAppDirs)(JNIEnv *env, jclass c, jstring config, jstring runtime, jstring temp)
{
    (void)c;
    char a[PIN_PATH_MAX], b[PIN_PATH_MAX], t[PIN_PATH_MAX];
    mj_utf8(env, config, a, sizeof(a));
    mj_utf8(env, runtime, b, sizeof(b));
    mj_utf8(env, temp, t, sizeof(t));
    return (jint)pin_set_app_dirs(a, b, t);
}

MJ_FN(jint, setFirmwareDir)(JNIEnv *env, jclass c, jstring dir)
{
    (void)c;
    char d[PIN_PATH_MAX];
    mj_utf8(env, dir, d, sizeof(d));
    return (jint)pin_set_firmware_dir(d);
}

MJ_FN(jint, encodersProbe)(JNIEnv *env, jclass c) { (void)env; (void)c; return pin_encoders_probe(); }
MJ_FN(void, setCpuKeepBusy)(JNIEnv *env, jclass c, jint mode) { (void)env; (void)c; pin_set_cpu_keep_busy(mode); }
MJ_FN(void, setLogLevel)(JNIEnv *env, jclass c, jint level) { (void)env; (void)c; pin_set_log_level(level); }

MJ_FN(void, setReplayFile)(JNIEnv *env, jclass c, jstring path)
{
    (void)c;
    char p[PIN_PATH_MAX];
    mj_utf8(env, path, p, sizeof(p));
    pin_set_replay_file(p);
}

/* What the app needs to know about a USB device UsbManager found, all from the core's model
 * table: {display name, pin_device_fd_problem() text, the open problem of an unsupported
 * sibling, the open problem without USB permission}; "" where nothing applies. null for a
 * vid / pid the core does not know. */
MJ_FN(jobjectArray, usbModel)(JNIEnv *env, jclass c, jint vid, jint pid)
{
    (void)c;
    pin_device_info_t d;
    memset(&d, 0, sizeof(d));
    d.size = sizeof(d);
    if (pin_usb_model((uint16_t)vid, (uint16_t)pid, &d) != PIN_OK)
        return NULL;
    char name[PIN_NAME_MAX * 2], fd[PIN_TEXT_MAX * 2], unsup[PIN_TEXT_MAX * 2] = "", perm[PIN_TEXT_MAX * 2];
    pin_device_display_name(&d, name, sizeof(name));
    pin_device_fd_problem(&d, fd, sizeof(fd));
    if (d.state == PIN_DEV_UNSUPPORTED)
        pin_device_open_problem(&d, unsup, sizeof(unsup));
    pin_device_info_t np = d;
    np.state = PIN_DEV_NO_DRIVER;   /* what a missing permission maps to on Android */
    pin_device_open_problem(&np, perm, sizeof(perm));

    jobjectArray r = (*env)->NewObjectArray(env, 4, (*env)->FindClass(env, "java/lang/String"), NULL);
    if (!r)
        return NULL;
    const char *v[4] = { name, fd, unsup, perm };
    for (int i = 0; i < 4; i++) {
        jstring js = mj_string(env, v[i]);
        (*env)->SetObjectArrayElement(env, r, i, js);
        (*env)->DeleteLocalRef(env, js);
    }
    return r;
}

MJ_FN(jstring, settingsGet)(JNIEnv *env, jclass c, jstring key)
{
    (void)c;
    char k[256], v[PIN_PATH_MAX];
    mj_utf8(env, key, k, sizeof(k));
    if (pin_settings_get(k, v, sizeof(v)) != PIN_OK)
        v[0] = 0;
    return mj_string(env, v);
}

MJ_FN(jboolean, settingsSet)(JNIEnv *env, jclass c, jstring key, jstring value)
{
    (void)c;
    char k[256], v[PIN_PATH_MAX];
    mj_utf8(env, key, k, sizeof(k));
    mj_utf8(env, value, v, sizeof(v));
    return pin_settings_set(k, v) == PIN_OK;
}

/* ---- update check: the app downloads the VERSION file, the core decides ---------------- */

#define UPDATE_MAX_BYTES (64 * 1024)

MJ_FN(jstring, updateUrl)(JNIEnv *env, jclass c) { (void)c; return mj_string(env, pin_update_url()); }

MJ_FN(jboolean, updateDue)(JNIEnv *env, jclass c, jlong now)
{
    (void)env; (void)c;
    return pin_update_due((int64_t)now) ? JNI_TRUE : JNI_FALSE;
}

/* The downloaded file and the current time (seconds since 1970); true when the file was a valid
 * VERSION file and the check is recorded (pin_update_check_json). */
MJ_FN(jboolean, updateCheck)(JNIEnv *env, jclass c, jbyteArray json, jlong now)
{
    (void)c;
    jsize n = json ? (*env)->GetArrayLength(env, json) : 0;
    if (n <= 0 || n > UPDATE_MAX_BYTES)
        return JNI_FALSE;
    char *buf = malloc((size_t)n);
    if (!buf)
        return JNI_FALSE;
    (*env)->GetByteArrayRegion(env, json, 0, n, (jbyte *)buf);
    pin_update_info_t info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    pin_status_t st = pin_update_check_json(buf, (size_t)n, (int64_t)now, &info);
    free(buf);
    return st == PIN_OK ? JNI_TRUE : JNI_FALSE;
}

/* {the notice ("MarvinCapture 1.1 is available (you have 1.0)."), latest version, download
 * address or "", release notes (plain text, may contain newlines) or ""} when the last check found a newer version, else null. */
MJ_FN(jobjectArray, updateKnown)(JNIEnv *env, jclass c)
{
    (void)c;
    pin_update_info_t info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (!pin_update_known(&info))
        return NULL;
    char text[PIN_TEXT_MAX];
    pin_format_update(&info, text, sizeof(text));
    jobjectArray r = (*env)->NewObjectArray(env, 4, (*env)->FindClass(env, "java/lang/String"), NULL);
    if (!r)
        return NULL;
    const char *v[4] = { text, info.latest, info.download_url, info.notes };
    for (int i = 0; i < 4; i++) {
        jstring js = mj_string(env, v[i]);
        (*env)->SetObjectArrayElement(env, r, i, js);
        (*env)->DeleteLocalRef(env, js);
    }
    return r;
}

MJ_FN(jstring, stdName)(JNIEnv *env, jclass c, jint std) { (void)c; return mj_string(env, pin_std_name((pin_std_t)std)); }

/* ---- formats -------------------------------------------------------------------------- */

MJ_FN(jintArray, formats)(JNIEnv *env, jclass c, jint kind)
{
    (void)c;
    pin_format_info_t f[PIN_FMT_COUNT];
    for (int i = 0; i < PIN_FMT_COUNT; i++)
        f[i].size = sizeof(f[i]);
    int n = pin_formats((pin_kind_t)kind, f, PIN_FMT_COUNT);
    if (n < 0)
        n = 0;
    if (n > PIN_FMT_COUNT)
        n = PIN_FMT_COUNT;
    jint ids[PIN_FMT_COUNT];
    for (int i = 0; i < n; i++)
        ids[i] = (jint)f[i].format;
    jintArray r = (*env)->NewIntArray(env, n);
    if (r)
        (*env)->SetIntArrayRegion(env, r, 0, n, ids);
    return r;
}

static int format_info(jint f, pin_format_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->size = sizeof(*out);
    return pin_format_info((pin_format_t)f, out) == PIN_OK;
}

MJ_FN(jstring, formatLabel)(JNIEnv *env, jclass c, jint f)
{
    (void)c;
    pin_format_info_t i;
    return mj_string(env, format_info(f, &i) ? i.label : "");
}

/* Core.FMT_*: 1 title, 2 scene split, 4 multi-pass, 8 default of its kind; the kind << 8. */
MJ_FN(jint, formatFlags)(JNIEnv *env, jclass c, jint f)
{
    (void)env; (void)c;
    pin_format_info_t i;
    if (!format_info(f, &i))
        return 0;
    return (i.supports_title ? 1 : 0) | (i.supports_scene_split ? 2 : 0) |
           (i.supports_multi_pass ? 4 : 0) | (i.is_default ? 8 : 0) | ((jint)i.kind << 8);
}

MJ_FN(jstring, formatWarning)(JNIEnv *env, jclass c, jint f)
{
    (void)c;
    char w[1024];
    pin_format_warning((pin_format_t)f, w, sizeof(w));
    return mj_string(env, w);
}

static int format_options(jint f, pin_format_options_t *o)
{
    memset(o, 0, sizeof(*o));
    o->size = sizeof(*o);
    return pin_format_options((pin_format_t)f, o) == PIN_OK;
}

/* {has_quality, has_deinterlace, has_ffv1_compression, quality_min, quality_max,
 *  quality_default, deinterlace_default, ffv1_default} (Core.FO_*) */
MJ_FN(jintArray, formatOptionInts)(JNIEnv *env, jclass c, jint f)
{
    (void)c;
    pin_format_options_t o;
    if (!format_options(f, &o))
        return NULL;
    jint v[8] = { o.has_quality, o.has_deinterlace, o.has_ffv1_compression, o.quality_min,
                  o.quality_max, o.quality_default, o.deinterlace_default, o.ffv1_default };
    jintArray r = (*env)->NewIntArray(env, 8);
    if (r)
        (*env)->SetIntArrayRegion(env, r, 0, 8, v);
    return r;
}

/* The labels and help texts, in pin_format_options_t order (Core.FT_*). */
MJ_FN(jobjectArray, formatOptionTexts)(JNIEnv *env, jclass c, jint f)
{
    (void)c;
    pin_format_options_t o;
    if (!format_options(f, &o))
        return NULL;
    const char *v[] = {
        o.quality_label, o.quality_low_label, o.quality_high_label, o.quality_help,
        o.deinterlace_label, o.deinterlace_keep_label, o.deinterlace_double_label,
        o.deinterlace_keep_help, o.deinterlace_double_help,
        o.ffv1_label, o.ffv1_fast_label, o.ffv1_smaller_label, o.ffv1_smallest_label, o.ffv1_help,
        o.note,
    };
    int n = (int)(sizeof(v) / sizeof(v[0]));
    jobjectArray r = (*env)->NewObjectArray(env, n, (*env)->FindClass(env, "java/lang/String"), NULL);
    if (!r)
        return NULL;
    for (int i = 0; i < n; i++) {
        jstring js = mj_string(env, v[i]);
        (*env)->SetObjectArrayElement(env, r, i, js);
        (*env)->DeleteLocalRef(env, js);
    }
    return r;
}

MJ_FN(jstring, formatQualityHint)(JNIEnv *env, jclass c, jint f, jint q, jint deint, jboolean is60)
{
    (void)c;
    char h[256];
    pin_format_quality_hint((pin_format_t)f, q, deint, is60 ? 1 : 0, h, sizeof(h));
    return mj_string(env, h);
}

MJ_FN(jstring, formatBytes)(JNIEnv *env, jclass c, jlong bytes)
{
    (void)c;
    char b[64];
    pin_format_bytes((uint64_t)bytes, b, sizeof(b));
    return mj_string(env, b);
}

MJ_FN(jstring, formatRemaining)(JNIEnv *env, jclass c, jdouble seconds)
{
    (void)c;
    char b[64];
    pin_format_remaining(seconds, b, sizeof(b));
    return mj_string(env, b);
}

MJ_FN(jint, nextFileNumber)(JNIEnv *env, jclass c, jstring path)
{
    (void)c;
    char p[PIN_PATH_MAX];
    mj_utf8(env, path, p, sizeof(p));
    return (jint)pin_next_file_number(p);
}

MJ_FN(jboolean, stopReasonAbnormal)(JNIEnv *env, jclass c, jint r)
{
    (void)env; (void)c;
    return pin_stop_reason_abnormal((pin_stop_reason_t)r) != 0;
}

MJ_FN(jboolean, stateIsCapturing)(JNIEnv *env, jclass c, jint st)
{
    (void)env; (void)c;
    return pin_state_is_capturing((pin_state_t)st) != 0;
}

static void log_event(const pin_event_t *e)
{
    int prio = ANDROID_LOG_INFO;
    if (e->kind == PIN_EVT_LOG) {
        prio = e->a >= 3 ? ANDROID_LOG_ERROR : e->a == 2 ? ANDROID_LOG_WARN
             : e->a == 1 ? ANDROID_LOG_INFO : ANDROID_LOG_DEBUG;
        __android_log_print(prio, "marvin-core", "%s", e->text);
    } else if (e->kind == PIN_EVT_ERROR) {
        __android_log_print(ANDROID_LOG_ERROR, "marvin-core", "error %d: %s", e->a, e->text);
    } else {
        __android_log_print(ANDROID_LOG_DEBUG, "marvin-core", "event %d a=%d %s", (int)e->kind, e->a, e->text);
    }
}

/* The process-wide log lines (pin_poll_event(NULL)) go to logcat only. */
MJ_FN(void, drainProcessLog)(JNIEnv *env, jclass c)
{
    (void)env; (void)c;
    pin_event_t e;
    e.size = sizeof(e);
    while (pin_poll_event(NULL, &e) == 1) {
        log_event(&e);
        e.size = sizeof(e);
    }
}

/* ---- session --------------------------------------------------------------------------- */

MJ_FN(jlong, open)(JNIEnv *env, jclass c, jstring id, jintArray status)
{
    (void)c;
    char d[PIN_PATH_MAX];
    mj_utf8(env, id, d, sizeof(d));
    pin_session_t *s = NULL;
    jint st = (jint)pin_open(d, &s);
    if (status && (*env)->GetArrayLength(env, status) > 0)
        (*env)->SetIntArrayRegion(env, status, 0, 1, &st);
    return st == PIN_OK ? (jlong)(intptr_t)s : 0;
}

MJ_FN(void, close)(JNIEnv *env, jclass c, jlong s) { (void)env; (void)c; pin_close(S(s)); }
MJ_FN(jint, setInput)(JNIEnv *env, jclass c, jlong s, jint in) { (void)env; (void)c; return (jint)pin_set_input(S(s), (pin_input_t)in); }
MJ_FN(jint, setStandard)(JNIEnv *env, jclass c, jlong s, jint std) { (void)env; (void)c; return (jint)pin_set_standard(S(s), (pin_std_t)std); }
MJ_FN(jint, setControl)(JNIEnv *env, jclass c, jlong s, jint ctl, jint v) { (void)env; (void)c; return (jint)pin_set_control(S(s), (pin_control_t)ctl, v); }
MJ_FN(void, setAspect)(JNIEnv *env, jclass c, jlong s, jint a) { (void)env; (void)c; pin_set_aspect(S(s), (pin_aspect_t)a); }
MJ_FN(void, previewEnable)(JNIEnv *env, jclass c, jlong s, jboolean on) { (void)env; (void)c; pin_preview_enable(S(s), on ? 1 : 0); }
MJ_FN(void, monitorEnable)(JNIEnv *env, jclass c, jlong s, jboolean on) { (void)env; (void)c; pin_monitor_enable(S(s), on ? 1 : 0); }
/* cmd = pin_deck_cmd_t (Core.DECK_*). Non-blocking. */
MJ_FN(jint, deck)(JNIEnv *env, jclass c, jlong s, jint cmd)
{
    (void)env; (void)c;
    return (jint)pin_deck(S(s), (pin_deck_cmd_t)cmd);
}
MJ_FN(jint, captureStop)(JNIEnv *env, jclass c, jlong s) { (void)env; (void)c; return (jint)pin_capture_stop(S(s)); }

/* {min, max, step, def, value, enabled} (Core.CT_*), or null. */
MJ_FN(jintArray, getControl)(JNIEnv *env, jclass c, jlong s, jint ctl)
{
    (void)c;
    pin_control_info_t i;
    memset(&i, 0, sizeof(i));
    i.size = sizeof(i);
    if (pin_get_control(S(s), (pin_control_t)ctl, &i) != PIN_OK)
        return NULL;
    jint v[6] = { i.min, i.max, i.step, i.def, i.value, i.enabled };
    jintArray r = (*env)->NewIntArray(env, 6);
    if (r)
        (*env)->SetIntArrayRegion(env, r, 0, 6, v);
    return r;
}

MJ_FN(jstring, controlLabel)(JNIEnv *env, jclass c, jlong s, jint ctl)
{
    (void)c;
    pin_control_info_t i;
    memset(&i, 0, sizeof(i));
    i.size = sizeof(i);
    if (pin_get_control(S(s), (pin_control_t)ctl, &i) != PIN_OK)
        i.label[0] = 0;
    return mj_string(env, i.label);
}

/* Capture options from Java: path, title and the Core.OPT_* ints. One pass; deck commands only when
 * OPT_START_DECK is set ("rewind, play and capture"). */
static int build_opts(JNIEnv *env, jstring path, jstring title, jintArray opts, pin_capture_opts_t *o)
{
    memset(o, 0, sizeof(*o));
    o->size = sizeof(*o);
    pin_capture_opts_defaults(o);
    if (mj_utf8(env, path, o->path, sizeof(o->path)) != 0)
        return 0;
    mj_utf8(env, title, o->title, sizeof(o->title));
    jint v[13];
    if (!opts || (*env)->GetArrayLength(env, opts) < 13)
        return 0;
    (*env)->GetIntArrayRegion(env, opts, 0, 13, v);
    /* a negative format or format setting keeps the core's default */
    if (v[0] >= 0) o->format_analog = (pin_format_t)v[0];
    if (v[1] >= 0) o->format_dv = (pin_format_t)v[1];
    if (v[2] >= 0) o->format_hdv = (pin_format_t)v[2];
    o->aspect = (pin_aspect_t)v[3];
    o->scene_split = v[4];
    o->idle_stop_minutes = v[5];
    o->max_duration_minutes = v[6];
    o->first_number = (unsigned)v[7];
    if (v[8] >= 0) o->deinterlace = v[8];
    if (v[9] >= 0) o->video_quality = v[9];
    if (v[10] >= 0) o->ffv1_compression = v[10];
    o->passes = 1;
    o->start_deck = v[11] != 0;
    o->rewind_first = v[12] != 0;
    return 1;
}

/* nums: {free_bytes, minutes_left, fat32, collision, low_space}; strs: {first_path, message}. */
MJ_FN(jint, checkOutput)(JNIEnv *env, jclass c, jlong s, jstring path, jstring title, jintArray opts,
                         jlongArray nums, jobjectArray strs)
{
    (void)c;
    pin_capture_opts_t o;
    if (!build_opts(env, path, title, opts, &o))
        return PIN_ERR_ARG;
    pin_output_check_t chk;
    memset(&chk, 0, sizeof(chk));
    chk.size = sizeof(chk);
    pin_status_t st = pin_check_output(S(s), &o, &chk);
    jlong n[5] = { (jlong)chk.free_bytes, (jlong)chk.minutes_left, chk.fat32, chk.collision, chk.low_space };
    if (nums && (*env)->GetArrayLength(env, nums) >= 5)
        (*env)->SetLongArrayRegion(env, nums, 0, 5, n);
    if (strs && (*env)->GetArrayLength(env, strs) >= 2) {
        jstring a = mj_string(env, chk.first_path), b = mj_string(env, chk.message);
        (*env)->SetObjectArrayElement(env, strs, 0, a);
        (*env)->SetObjectArrayElement(env, strs, 1, b);
    }
    return (jint)st;
}

MJ_FN(jint, setOutputHint)(JNIEnv *env, jclass c, jlong s, jstring path, jstring title, jintArray opts)
{
    (void)c;
    pin_capture_opts_t o;
    if (!build_opts(env, path, title, opts, &o))
        return PIN_ERR_ARG;
    return (jint)pin_set_output_hint(S(s), &o);
}

MJ_FN(jint, captureStart)(JNIEnv *env, jclass c, jlong s, jstring path, jstring title, jintArray opts,
                          jboolean overwrite)
{
    (void)c;
    pin_capture_opts_t o;
    if (!build_opts(env, path, title, opts, &o))
        return PIN_ERR_ARG;
    return (jint)pin_capture_start(S(s), &o, overwrite ? 1 : 0);
}

/* Returns the event kind, or -1 when none is pending; a[0] = the event's a, text[0] its text.
 * Log lines are also written to logcat. */
MJ_FN(jint, pollEvent)(JNIEnv *env, jclass c, jlong s, jintArray a, jobjectArray text)
{
    (void)c;
    pin_event_t e;
    memset(&e, 0, sizeof(e));
    e.size = sizeof(e);
    if (pin_poll_event(S(s), &e) != 1)
        return -1;
    log_event(&e);
    jint av = e.a;
    if (a && (*env)->GetArrayLength(env, a) > 0)
        (*env)->SetIntArrayRegion(env, a, 0, 1, &av);
    if (text && (*env)->GetArrayLength(env, text) > 0) {
        jstring t = mj_string(env, e.text);
        (*env)->SetObjectArrayElement(env, text, 0, t);
        (*env)->DeleteLocalRef(env, t);
    }
    return (jint)e.kind;
}

/* The status snapshot plus everything derived from it by the core's text and enable rules.
 * Index order = Core.ST_* (ints), SL_* (longs), SD_* (doubles), SS_* (strings). */
#define N_INTS 30
#define N_LONGS 7
#define N_DOUBLES 8
#define N_STRS 20

MJ_FN(jint, getStatus)(JNIEnv *env, jclass c, jlong h, jintArray ints, jlongArray longs,
                       jdoubleArray dbls, jobjectArray strs)
{
    (void)c;
    pin_status_snapshot_t st;
    memset(&st, 0, sizeof(st));
    st.size = sizeof(st);
    pin_status_t r = pin_get_status(S(h), &st);
    if (r != PIN_OK)
        return (jint)r;

    int deck_available = st.input != PIN_INPUT_DV || st.camera_present != 0;
    char short_text[PIN_PATH_MAX + 128];
    int hub = pin_format_status_short(&st, 0, short_text, sizeof(short_text));

    jint iv[N_INTS] = {
        st.state, st.input, st.stream_kind, st.signal, st.is_60hz, st.dar_num, st.dar_den,
        st.deck, st.camera_present, st.progress_percent, st.disk_low, st.stop_reason,
        st.stop_no_video, st.pass, st.passes, st.last_error,
        pin_state_is_capturing(st.state),
        pin_manual_capture_allowed(st.state, deck_available, st.deck, st.signal),
        pin_capture_action_allowed(st.state, deck_available, PIN_CAPTURE_STOP),
        st.deck_busy, st.width, st.height, st.video_interlaced,
        pin_stop_reason_abnormal(st.stop_reason), hub,
        pin_deck_cmd_allowed(st.state, deck_available, st.deck, PIN_DECK_CMD_REW),
        pin_deck_cmd_allowed(st.state, deck_available, st.deck, PIN_DECK_CMD_PLAY),
        pin_deck_cmd_allowed(st.state, deck_available, st.deck, PIN_DECK_CMD_STOP),
        pin_deck_cmd_allowed(st.state, deck_available, st.deck, PIN_DECK_CMD_FF),
        pin_capture_action_allowed(st.state, deck_available, PIN_CAPTURE_START_AUTO),
    };
    jlong lv[N_LONGS] = {
        (jlong)st.frames, (jlong)st.frames_error, (jlong)st.frames_dropped, (jlong)st.bytes_written,
        (jlong)st.total_bytes_written, (jlong)st.disk_free_bytes, (jlong)st.write_dropped,
    };
    jdouble dv[N_DOUBLES] = {
        st.elapsed_s, st.idle_stop_remaining_s, st.duration_remaining_s,
        st.audio_peak_db[0], st.audio_peak_db[1], st.est_seconds_left,
        st.audio_rms_db[0], st.audio_rms_db[1],
    };
    if (ints && (*env)->GetArrayLength(env, ints) >= N_INTS)
        (*env)->SetIntArrayRegion(env, ints, 0, N_INTS, iv);
    if (longs && (*env)->GetArrayLength(env, longs) >= N_LONGS)
        (*env)->SetLongArrayRegion(env, longs, 0, N_LONGS, lv);
    if (dbls && (*env)->GetArrayLength(env, dbls) >= N_DOUBLES)
        (*env)->SetDoubleArrayRegion(env, dbls, 0, N_DOUBLES, dv);
    if (!strs || (*env)->GetArrayLength(env, strs) < N_STRS)
        return PIN_OK;

    char state[128], signal[128], frames[256], frames_detail[1024], sizes[128], free_text[256],
         storage_detail[1024], line[512], idle_left[64] = "", dur_left[64] = "";
    pin_format_state(&st, state, sizeof(state));
    pin_format_signal(&st, signal, sizeof(signal));
    pin_format_frames(&st, frames, sizeof(frames));
    pin_format_frames_detail(&st, frames_detail, sizeof(frames_detail));
    pin_format_sizes(&st, sizes, sizeof(sizes));
    pin_format_storage_free(&st, free_text, sizeof(free_text));
    pin_format_storage_detail(&st, storage_detail, sizeof(storage_detail));
    pin_format_status_line(&st, line, sizeof(line));
    if (st.idle_stop_remaining_s >= 0)
        pin_format_remaining(st.idle_stop_remaining_s, idle_left, sizeof(idle_left));
    if (st.duration_remaining_s >= 0)
        pin_format_remaining(st.duration_remaining_s, dur_left, sizeof(dur_left));

    char deck_tip[160];
    pin_format_deck_tip(st.deck, st.deck_busy, deck_tip, sizeof(deck_tip));
    const char *sv[N_STRS] = {
        short_text, state, signal, frames, frames_detail, sizes, free_text, storage_detail,
        st.detail, st.error_text, st.stop_text, st.current_file, st.video_label, st.timecode,
        line, pin_manual_capture_block_text(deck_available, st.deck, st.signal), idle_left, dur_left,
        pin_deck_state_name(st.deck), deck_tip,
    };
    for (int i = 0; i < N_STRS; i++) {
        jstring js = mj_string(env, sv[i]);
        (*env)->SetObjectArrayElement(env, strs, i, js);
        (*env)->DeleteLocalRef(env, js);
    }
    return PIN_OK;
}
