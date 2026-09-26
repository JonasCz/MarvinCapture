/*
 * pin_stub_capture.c — pin_check_output / pin_capture_start / pin_capture_stop
 * and the per-tick capture simulation.
 *
 * The target file is real: pin_check_output does real filesystem checks
 * (directory exists, name collision) and, on Windows, a real free-space /
 * FAT32 query via GetDiskFreeSpaceExW / GetVolumeInformationW. During a
 * simulated capture we really create the file and really grow it — but
 * only up to PIN_STUB_MAX_REAL_BYTES of actual on-disk data. Past that we
 * keep incrementing the *reported* bytes_written counter (what the GUI
 * shows) without doing more real I/O, so a long fake capture doesn't write
 * gigabytes of junk to the test box. This is documented here rather than
 * hidden: a real core always writes what it reports.
 */
#include "pin_stub.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#define PIN_STUB_MAX_REAL_BYTES (256 * 1024)

/* Rough bytes/second for each format, used for the fake capture growth
 * rate and the "minutes left" estimate. Not exact, just plausible. */
static double bytes_per_second_for(pin_format_t f) {
    switch (f) {
    case PIN_FMT_ANALOG_AVI: return 27.0 * 1024 * 1024;      /* uncompressed 4:2:2 SD */
    case PIN_FMT_ANALOG_FFV1_MKV: return 9.0 * 1024 * 1024;  /* lossless intra, compresses some */
    case PIN_FMT_DV_RAW:
    case PIN_FMT_DV_AVI:
    case PIN_FMT_DV_MOV: return 3.6 * 1024 * 1024;           /* DV25 */
    case PIN_FMT_HDV_TS:
    case PIN_FMT_HDV_MOV:
    case PIN_FMT_HDV_MKV: return 2.3 * 1024 * 1024;          /* ~19 Mbps HDV */
    default: return 4.0 * 1024 * 1024;
    }
}

static pin_format_t format_for_kind(const pin_capture_opts_t *o, pin_kind_t kind) {
    switch (kind) {
    case PIN_KIND_ANALOG: return o->format_analog;
    case PIN_KIND_DV: return o->format_dv;
    case PIN_KIND_HDV: return o->format_hdv;
    default: return o->format_dv;
    }
}

static void build_first_path(const char *base, pin_format_t fmt, char *out, size_t cap) {
    pin_format_info_t fi; fi.size = sizeof(fi);
    pin_format_info(fmt, &fi);
    size_t blen = strlen(base);
    size_t elen = strlen(fi.extension);
    /* if the base already ends in ".ext" (case-insensitive), don't double it */
    if (blen > elen + 1 && base[blen - elen - 1] == '.') {
        size_t i = 0;
        int same = 1;
        for (; i < elen; i++) {
            char a = base[blen - elen + i], b = fi.extension[i];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { same = 0; break; }
        }
        if (same) { snprintf(out, cap, "%s", base); return; }
    }
    snprintf(out, cap, "%s.%s", base, fi.extension);
}

static void dirname_of(const char *path, char *out, size_t cap) {
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    const char *cut = slash > bslash ? slash : bslash;
    if (!cut) { snprintf(out, cap, "."); return; }
    size_t n = (size_t)(cut - path);
    if (n >= cap) n = cap - 1;
    memcpy(out, path, n);
    out[n] = 0;
}

pin_status_t pin_check_output(pin_session_t *s, const pin_capture_opts_t *o,
                               pin_output_check_t *out) {
    if (!s || !o || !out) return PIN_ERR_ARG;
    if (!pin_opts_size_ok(o->size) || out->size != sizeof(*out)) return PIN_ERR_ABI;
    uint32_t caller_size = out->size;
    memset(out, 0, sizeof(*out));
    out->size = caller_size;

    pthread_mutex_lock(&s->lock);
    pin_kind_t kind = s->stream_kind;
    pthread_mutex_unlock(&s->lock);

    pin_format_t fmt = format_for_kind(o, kind);
    build_first_path(o->path, fmt, out->first_path, sizeof(out->first_path));

    char dir[PIN_PATH_MAX];
    dirname_of(out->first_path, dir, sizeof(dir));
    struct stat st;
    int dir_ok = (stat(dir, &st) == 0);

    struct stat fst;
    out->collision = (stat(out->first_path, &fst) == 0);

    out->free_bytes = 0;
    out->fat32 = 0;
#if defined(_WIN32)
    {
        wchar_t wdir[PIN_PATH_MAX];
        int n = MultiByteToWideChar(CP_UTF8, 0, dir, -1, wdir, PIN_PATH_MAX);
        if (n > 0) {
            ULARGE_INTEGER freeAvail, total, totalFree;
            if (GetDiskFreeSpaceExW(wdir, &freeAvail, &total, &totalFree)) {
                out->free_bytes = freeAvail.QuadPart;
            }
            wchar_t root[8] = L"C:\\";
            if (n >= 2 && wdir[1] == L':') { root[0] = wdir[0]; }
            wchar_t fsname[64] = {0};
            if (GetVolumeInformationW(root, NULL, 0, NULL, NULL, NULL, fsname, 64)) {
                if (wcsstr(fsname, L"FAT32")) out->fat32 = 1;
            }
        }
    }
#endif
    if (out->free_bytes == 0) {
        /* Win32 query unavailable/failed: a plausible fallback so the GUI
         * still has something sane to show. */
        out->free_bytes = (uint64_t)200 * 1024 * 1024 * 1024;
    }

    double bps = bytes_per_second_for(fmt);
    out->minutes_left = (uint64_t)((double)out->free_bytes / bps / 60.0);

    out->message[0] = 0;
    if (!dir_ok) {
        snprintf(out->message, sizeof(out->message), "Output folder does not exist: %s", dir);
    } else if (out->collision) {
        snprintf(out->message, sizeof(out->message), "%s already exists", out->first_path);
    } else if (out->fat32 && (fmt == PIN_FMT_DV_RAW || fmt == PIN_FMT_HDV_TS)) {
        snprintf(out->message, sizeof(out->message),
                 "FAT32 volume: files over 4 GiB will fail for this format");
    } else if (out->minutes_left < 5) {
        snprintf(out->message, sizeof(out->message), "Low disk space: about %llu minute(s) left",
                 (unsigned long long)out->minutes_left);
    }

    return PIN_OK;
}

pin_status_t pin_capture_start(pin_session_t *s, const pin_capture_opts_t *o, int overwrite) {
    if (!s || !o) return PIN_ERR_ARG;
    if (!pin_opts_size_ok(o->size)) return PIN_ERR_ABI;

    pthread_mutex_lock(&s->lock);
    if (s->state != PIN_STATE_READY) {
        pthread_mutex_unlock(&s->lock);
        return PIN_ERR_STATE;
    }
    pin_kind_t kind = s->stream_kind;
    pthread_mutex_unlock(&s->lock);

    pin_format_t fmt = format_for_kind(o, kind);
    char first_path[PIN_PATH_MAX];
    build_first_path(o->path, fmt, first_path, sizeof(first_path));

    struct stat fst;
    if (!overwrite && stat(first_path, &fst) == 0) return PIN_ERR_EXISTS;

    FILE *fp = fopen(first_path, "wb");
    if (!fp) return PIN_ERR_IO;

    pthread_mutex_lock(&s->lock);
    s->cap.active = 1;
    s->cap.opts = *o;
    s->cap.overwrite = overwrite;
    s->cap.fp = fp;
    strncpy(s->cap.path, first_path, sizeof(s->cap.path) - 1);
    s->cap.start_time = pin_stub_now();
    s->cap.last_grow_time = s->cap.start_time;
    s->cap.frames = 0;
    s->cap.bytes_written = 0;
    s->cap.pass = 1;
    s->cap.passes = o->passes >= 1 ? o->passes : 1;
    s->cap.scene = 1;
    s->cap.stopping = 0;
    pin_stub_set_state(s, PIN_STATE_CAPTURING);
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);

    pin_evtq_push(&s->events, PIN_EVT_FILE_OPENED, 0, first_path);
    pin_evtq_push(&s->events, PIN_EVT_SCENE, 1, "");
    pin_evtq_push(&s->events, PIN_EVT_PASS, 1, "");

    if (pin_opts_rewind_first(o)) {
        /* "Play and capture" rewinds to the start of the tape first. The stub
         * rewinds instantly; the real engine goes through REWINDING. */
        pthread_mutex_lock(&s->lock);
        s->deck.tc_frames = 0;
        s->deck.tape_percent = 0;
        pthread_mutex_unlock(&s->lock);
        pin_evtq_push(&s->events, PIN_EVT_LOG, 1, "Rewound to the start of the tape");
    }
    if (o->start_deck) {
        pthread_mutex_lock(&s->lock);
        s->deck.state = PIN_DECK_PLAYING;
        pthread_mutex_unlock(&s->lock);
        pin_evtq_push(&s->events, PIN_EVT_DECK, (int32_t)PIN_DECK_PLAYING, "");
    }
    return PIN_OK;
}

pin_status_t pin_capture_stop(pin_session_t *s) {
    if (!s) return PIN_ERR_ARG;
    pthread_mutex_lock(&s->lock);
    if (!s->cap.active) {
        pthread_mutex_unlock(&s->lock);
        return PIN_ERR_STATE;
    }
    s->cap.stopping = 1;
    s->cap.stopping_since = pin_stub_now();
    pin_stub_set_state(s, PIN_STATE_STOPPING);
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    return PIN_OK;
}

/* Called by the worker thread each tick; s->lock NOT held on entry. */
void pin_stub_capture_tick(pin_session_t *s, double now) {
    pthread_mutex_lock(&s->lock);
    if (!s->cap.active) { pthread_mutex_unlock(&s->lock); return; }

    if (!s->cap.stopping) {
        double dt = now - s->cap.last_grow_time;
        if (dt > 0) {
            s->cap.last_grow_time = now;
            double fps = 25.0;
            s->cap.frames = (uint64_t)(fps * (now - s->cap.start_time));

            pin_format_t fmt = format_for_kind(&s->cap.opts, s->stream_kind);
            double bps = bytes_per_second_for(fmt);
            uint64_t grow = (uint64_t)(bps * dt);
            s->cap.bytes_written += grow;

            if (s->cap.fp) {
                long real_pos = ftell(s->cap.fp);
                if (real_pos >= 0 && (uint64_t)real_pos < PIN_STUB_MAX_REAL_BYTES) {
                    uint64_t room = PIN_STUB_MAX_REAL_BYTES - (uint64_t)real_pos;
                    uint64_t write_n = grow < room ? grow : room;
                    static const char junk[4096] = {0};
                    while (write_n > 0) {
                        size_t chunk = write_n < sizeof(junk) ? (size_t)write_n : sizeof(junk);
                        fwrite(junk, 1, chunk, s->cap.fp);
                        write_n -= chunk;
                    }
                    fflush(s->cap.fp);
                }
            }

            /* fake scene splits: a new "scene" every ~20s of capture, only
             * if the caller asked for scene_split (DV/HDV only). */
            if (s->cap.opts.scene_split) {
                int expected_scene = 1 + (int)((now - s->cap.start_time) / 20.0);
                if (expected_scene != s->cap.scene) {
                    s->cap.scene = expected_scene;
                    pthread_mutex_unlock(&s->lock);
                    pin_evtq_push(&s->events, PIN_EVT_SCENE, expected_scene, "");
                    pthread_mutex_lock(&s->lock);
                }
            }

            /* idle-stop: simulate no configured idle timeout by default;
             * honour idle_stop_minutes as a hard cap on elapsed time for
             * this stub (a real core watches actual signal/data idle). */
            if (s->cap.opts.idle_stop_minutes > 0 &&
                (now - s->cap.start_time) > s->cap.opts.idle_stop_minutes * 60.0) {
                s->cap.stopping = 1;
                s->cap.stopping_since = now;
                pin_stub_set_state(s, PIN_STATE_STOPPING);
            }
        }
    } else if (now - s->cap.stopping_since >= 0.5) {
        /* finalize delay elapsed */
        if (s->cap.fp) { fclose(s->cap.fp); s->cap.fp = NULL; }
        char closed_path[PIN_PATH_MAX];
        strncpy(closed_path, s->cap.path, sizeof(closed_path) - 1);
        closed_path[sizeof(closed_path) - 1] = 0;
        s->cap.active = 0;
        pin_stub_set_state(s, PIN_STATE_READY);
        pthread_mutex_unlock(&s->lock);
        pin_evtq_push(&s->events, PIN_EVT_FILE_CLOSED, (int32_t)PIN_OK, closed_path);
        if (s->cap.opts.start_deck) {
            pthread_mutex_lock(&s->lock);
            s->deck.state = PIN_DECK_STOPPED;
            pthread_mutex_unlock(&s->lock);
            pin_evtq_push(&s->events, PIN_EVT_DECK, (int32_t)PIN_DECK_STOPPED, "");
        }
        return;
    }
    pthread_mutex_unlock(&s->lock);
}

/* ==== disk estimate / output hint (transitional ABI, see pin_stub_abi2.h) ==== */

void pin_stub_estimate_disk(pin_session_t *s, uint64_t *free_bytes, double *seconds_left) {
    const pin_capture_opts_t *o = s->cap.active ? &s->cap.opts : (s->has_hint ? &s->hint : NULL);
    *free_bytes = 0;
    *seconds_left = -1;
    if (!o || !o->path[0]) return;

    double now = pin_stub_now();
    if (now - s->disk_cache_time > 2.0 || s->disk_cache_time == 0) {
        char dir[PIN_PATH_MAX];
        pin_format_t fmt0 = format_for_kind(o, s->stream_kind);
        char first[PIN_PATH_MAX];
        build_first_path(o->path, fmt0, first, sizeof(first));
        dirname_of(first, dir, sizeof(dir));
        uint64_t fb = 0;
#if defined(_WIN32)
        wchar_t wdir[PIN_PATH_MAX];
        if (MultiByteToWideChar(CP_UTF8, 0, dir, -1, wdir, PIN_PATH_MAX) > 0) {
            ULARGE_INTEGER avail, total, tfree;
            if (GetDiskFreeSpaceExW(wdir, &avail, &total, &tfree)) fb = avail.QuadPart;
        }
#endif
        if (fb == 0) fb = (uint64_t)200 * 1024 * 1024 * 1024;
        s->disk_cache_free = fb;
        s->disk_cache_time = now;
    }
    *free_bytes = s->disk_cache_free;
    double bps = bytes_per_second_for(format_for_kind(o, s->stream_kind));
    if (bps > 0) *seconds_left = (double)*free_bytes / bps;
}

pin_status_t pin_set_output_hint(pin_session_t *s, const pin_capture_opts_t *o) {
    if (!s || !o) return PIN_ERR_ARG;
    if (!pin_opts_size_ok(o->size)) return PIN_ERR_ABI;
    pthread_mutex_lock(&s->lock);
    memset(&s->hint, 0, sizeof(s->hint));
    memcpy(&s->hint, o, o->size < sizeof(s->hint) ? o->size : sizeof(s->hint));
    s->has_hint = 1;
    s->disk_cache_time = 0; /* re-query for the new folder */
    pthread_mutex_unlock(&s->lock);
    return PIN_OK;
}
