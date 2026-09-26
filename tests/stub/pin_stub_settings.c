/*
 * pin_stub_settings.c — INI-backed settings, re-read on every get and
 * written atomically (write to a temp file, then rename over the target)
 * on every set, at %LOCALAPPDATA%\PinnacleOSS\stub_settings.ini, matching
 * the documented behaviour of the real core so multiple stub-backed
 * windows don't stomp on each other's settings.
 *
 * Format: one "section.key=value" pair per line (a flattened INI; the
 * "section." prefix is just part of the key string pin_settings_get/set
 * already use, so there is no need for real [section] headers here).
 */
#include "pin_stub.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#if defined(_WIN32)
#include <windows.h>
#endif

static char g_ini_path[PIN_PATH_MAX];
static pthread_mutex_t g_settings_lock = PTHREAD_MUTEX_INITIALIZER;

static void build_ini_path(void) {
    if (g_ini_path[0]) return;
#if defined(_WIN32)
    const char *local = getenv("LOCALAPPDATA");
    if (!local) local = ".";
    char dir[PIN_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s\\PinnacleOSS", local);
    CreateDirectoryA(dir, NULL); /* ignore ERROR_ALREADY_EXISTS */
    snprintf(g_ini_path, sizeof(g_ini_path), "%s\\stub_settings.ini", dir);
#else
    const char *home = getenv("HOME");
    if (!home) home = ".";
    snprintf(g_ini_path, sizeof(g_ini_path), "%s/.pinnacle-oss-stub-settings.ini", home);
#endif
}

void pin_stub_settings_init(void) { build_ini_path(); }

static pin_status_t ini_find(const char *key, char *out, size_t cap) {
    FILE *fp = fopen(g_ini_path, "rb");
    if (!fp) return PIN_ERR_NOT_FOUND;
    char line[PIN_TEXT_MAX + PIN_NAME_MAX];
    size_t keylen = strlen(key);
    pin_status_t rc = PIN_ERR_NOT_FOUND;
    while (fgets(line, sizeof(line), fp)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
        if (strncmp(line, key, keylen) == 0 && line[keylen] == '=') {
            if (out && cap) {
                strncpy(out, line + keylen + 1, cap - 1);
                out[cap - 1] = 0;
            }
            rc = PIN_OK;
            break;
        }
    }
    fclose(fp);
    return rc;
}

pin_status_t pin_settings_get(const char *key, char *out, size_t cap) {
    if (!key || !out || cap == 0) return PIN_ERR_ARG;
    build_ini_path();
    pthread_mutex_lock(&g_settings_lock);
    out[0] = 0;
    pin_status_t rc = ini_find(key, out, cap);
    pthread_mutex_unlock(&g_settings_lock);
    return rc;
}

pin_status_t pin_settings_set(const char *key, const char *value) {
    if (!key || !value) return PIN_ERR_ARG;
    build_ini_path();
    pthread_mutex_lock(&g_settings_lock);

    size_t keylen = strlen(key);
    char tmp_path[PIN_PATH_MAX];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", g_ini_path);

    FILE *in = fopen(g_ini_path, "rb");
    FILE *out = fopen(tmp_path, "wb");
    if (!out) {
        if (in) fclose(in);
        pthread_mutex_unlock(&g_settings_lock);
        return PIN_ERR_IO;
    }

    int written = 0;
    char line[PIN_TEXT_MAX + PIN_NAME_MAX];
    if (in) {
        while (fgets(line, sizeof(line), in)) {
            size_t n = strlen(line);
            while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
            if (n == 0) continue;
            if (strncmp(line, key, keylen) == 0 && line[keylen] == '=') {
                fprintf(out, "%s=%s\n", key, value);
                written = 1;
            } else {
                fprintf(out, "%s\n", line);
            }
        }
        fclose(in);
    }
    if (!written) fprintf(out, "%s=%s\n", key, value);
    fclose(out);

#if defined(_WIN32)
    /* MoveFileExW with REPLACE_EXISTING is the atomic rename on Windows;
     * remove+rename as a portable fallback if that ever isn't available. */
    if (!MoveFileExA(tmp_path, g_ini_path, MOVEFILE_REPLACE_EXISTING)) {
        remove(g_ini_path);
        rename(tmp_path, g_ini_path);
    }
#else
    rename(tmp_path, g_ini_path);
#endif

    pthread_mutex_unlock(&g_settings_lock);
    return PIN_OK;
}
