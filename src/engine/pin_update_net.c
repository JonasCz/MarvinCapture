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

/* pin_update_check(): downloads the published VERSION file and compares it with
 * this build's version. Windows uses WinHTTP; macOS and Linux load libcurl at run
 * time (dlopen), so the core has no build or package dependency on it and a system
 * without it simply never sees an update. Every failure is silent: the caller gets
 * a status and shows nothing. */

#include "pin_update.h"

#include "../core/pin_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#else
#include <dlfcn.h>
#endif

#ifndef PIN_APP_VERSION
#define PIN_APP_VERSION "0"
#endif
#define PIN_WIDEN_(s) L##s
#define PIN_WIDEN(s) PIN_WIDEN_(s)

#define UPDATE_URL "https://raw.githubusercontent.com/JonasCz/MarvinCapture/main/VERSION"
#define UPDATE_MAX_BYTES (64 * 1024)

typedef struct {
    char *data;
    size_t len;
    int overflow;
} body_t;

static void body_append(body_t *b, const void *p, size_t n)
{
    if (b->overflow || b->len + n > UPDATE_MAX_BYTES) {
        b->overflow = 1;
        return;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

static int fetch_file(const char *path, body_t *b)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        body_append(b, buf, n);
    fclose(f);
    return b->overflow ? -1 : 0;
}

#ifdef _WIN32

static int fetch_http(const char *url, uint32_t timeout_ms, body_t *b)
{
    wchar_t wurl[PIN_PATH_MAX];
    if (!MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, PIN_PATH_MAX))
        return -1;
    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256], path[PIN_PATH_MAX];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = PIN_PATH_MAX;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc))
        return -1;

    int rc = -1;
    HINTERNET ses = NULL, con = NULL, req = NULL;
    ses = WinHttpOpen(L"MarvinCapture/" PIN_WIDEN(PIN_APP_VERSION), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses)
        goto out;
    int t = (int)timeout_ms;
    WinHttpSetTimeouts(ses, t, t, t, t);
    con = WinHttpConnect(ses, host, uc.nPort, 0);
    if (!con)
        goto out;
    req = WinHttpOpenRequest(con, L"GET", path, NULL, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                             uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!req)
        goto out;
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, NULL))
        goto out;
    DWORD code = 0, size = sizeof(code);
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX) ||
        code != 200) {
        pin_logf(PIN_LOG_DEBUG, "update check: HTTP status %lu\n", (unsigned long)code);
        goto out;
    }
    for (;;) {
        char buf[4096];
        DWORD got = 0;
        if (!WinHttpReadData(req, buf, sizeof(buf), &got))
            goto out;
        if (got == 0)
            break;
        body_append(b, buf, got);
        if (b->overflow)
            goto out;
    }
    rc = 0;
out:
    if (rc != 0)
        pin_logf(PIN_LOG_DEBUG, "update check: download failed (error %lu)\n", (unsigned long)GetLastError());
    if (req)
        WinHttpCloseHandle(req);
    if (con)
        WinHttpCloseHandle(con);
    if (ses)
        WinHttpCloseHandle(ses);
    return rc;
}

#else /* libcurl, loaded at run time */

/* The few libcurl entry points and option numbers used (stable ABI since 7.x). */
typedef void CURL;
typedef CURL *(*curl_easy_init_fn)(void);
typedef int (*curl_easy_setopt_fn)(CURL *, int, ...);
typedef int (*curl_easy_perform_fn)(CURL *);
typedef void (*curl_easy_cleanup_fn)(CURL *);
typedef int (*curl_easy_getinfo_fn)(CURL *, int, ...);
#define CURLOPT_WRITEDATA 10001
#define CURLOPT_URL 10002
#define CURLOPT_USERAGENT 10018
#define CURLOPT_WRITEFUNCTION 20011
#define CURLOPT_FOLLOWLOCATION 52
#define CURLOPT_NOSIGNAL 99
#define CURLOPT_TIMEOUT_MS 155
#define CURLOPT_CONNECTTIMEOUT_MS 156
#define CURLINFO_RESPONSE_CODE 0x200002

static size_t curl_write(char *p, size_t size, size_t n, void *ud)
{
    body_t *b = ud;
    body_append(b, p, size * n);
    return b->overflow ? 0 : size * n;   /* 0 aborts the transfer */
}

static int fetch_http(const char *url, uint32_t timeout_ms, body_t *b)
{
    static const char *const names[] = {
#ifdef __APPLE__
        "/usr/lib/libcurl.4.dylib", "libcurl.4.dylib", "libcurl.dylib",
#else
        "libcurl.so.4", "libcurl-gnutls.so.4", "libcurl-nss.so.4", "libcurl.so",
#endif
    };
    void *lib = NULL;
    for (size_t i = 0; !lib && i < sizeof(names) / sizeof(names[0]); i++)
        lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        pin_logf(PIN_LOG_DEBUG, "update check: libcurl not found\n");
        return -1;
    }
    curl_easy_init_fn init = (curl_easy_init_fn)dlsym(lib, "curl_easy_init");
    curl_easy_setopt_fn setopt = (curl_easy_setopt_fn)dlsym(lib, "curl_easy_setopt");
    curl_easy_perform_fn perform = (curl_easy_perform_fn)dlsym(lib, "curl_easy_perform");
    curl_easy_cleanup_fn cleanup = (curl_easy_cleanup_fn)dlsym(lib, "curl_easy_cleanup");
    curl_easy_getinfo_fn getinfo = (curl_easy_getinfo_fn)dlsym(lib, "curl_easy_getinfo");
    int rc = -1;
    CURL *h = (init && setopt && perform && cleanup && getinfo) ? init() : NULL;
    if (h) {
        setopt(h, CURLOPT_URL, url);
        setopt(h, CURLOPT_USERAGENT, "MarvinCapture/" PIN_APP_VERSION);
        setopt(h, CURLOPT_NOSIGNAL, 1L);
        setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
        setopt(h, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
        setopt(h, CURLOPT_CONNECTTIMEOUT_MS, (long)timeout_ms);
        setopt(h, CURLOPT_WRITEFUNCTION, curl_write);
        setopt(h, CURLOPT_WRITEDATA, (void *)b);
        int res = perform(h);
        long code = 0;
        getinfo(h, CURLINFO_RESPONSE_CODE, &code);
        if (res == 0 && code == 200 && !b->overflow)
            rc = 0;
        else
            pin_logf(PIN_LOG_DEBUG, "update check: download failed (curl %d, HTTP %ld)\n", res, code);
        cleanup(h);
    }
    /* libcurl stays loaded: unloading it is not safe while its global state lives. */
    return rc;
}

#endif

pin_status_t pin_update_net_check(pin_update_info_t *info, uint32_t timeout_ms)
{
    if (!info || info->size < sizeof(*info))
        return info ? PIN_ERR_ABI : PIN_ERR_ARG;
    uint32_t size = info->size;
    memset(info, 0, sizeof(*info));
    info->size = size;
    snprintf(info->current, sizeof(info->current), "%s", PIN_APP_VERSION);
    if (timeout_ms == 0)
        timeout_ms = 5000;

    const char *url = getenv("MARVIN_UPDATE_URL");
    if (!url)
        url = UPDATE_URL;
    if (!url[0])
        return PIN_ERR_STATE;   /* turned off */

    body_t b = { malloc(UPDATE_MAX_BYTES), 0, 0 };
    if (!b.data)
        return PIN_ERR_NOMEM;
    int http = strncmp(url, "https://", 8) == 0 || strncmp(url, "http://", 7) == 0;
    int rc = http ? fetch_http(url, timeout_ms, &b) : fetch_file(url, &b);
    pin_status_t st = PIN_ERR_IO;
    if (rc == 0) {
        if (pin_update_parse(b.data, b.len, info) == 0) {
            info->available = pin_update_compare(info->latest, info->current) > 0;
            st = PIN_OK;
            pin_logf(PIN_LOG_DEBUG, "update check: published %s, this is %s\n", info->latest, info->current);
        } else {
            pin_logf(PIN_LOG_DEBUG, "update check: the published VERSION file is not valid\n");
        }
    }
    free(b.data);
    return st;
}
