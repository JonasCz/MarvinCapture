/*
 * pinnacle-oss-core STUB — transitional ABI helpers.
 *
 * The engine workstream is appending fields to three public structs
 * (appended only, so the old prefix is unchanged):
 *
 *   pin_capture_opts_t     + int rewind_first;
 *   pin_status_snapshot_t  + uint64_t disk_free_bytes; double est_seconds_left; int disk_low;
 *   pin_launch_t           (grows with the embedded pin_capture_opts_t)
 *
 * and adding pin_set_output_hint(). src/api/pin_api.h is frozen for this
 * workstream and may or may not carry the new fields yet, so the stub
 * computes every new offset from the last field the header is known to
 * have and accepts both the "v1" size (header as it was) and the "v2" size
 * (with the appended fields). This works unchanged before and after the
 * header update. Offsets follow normal C layout rules for the appended
 * types (x86-64 / MinGW), matching the C# mirrors in the GUI.
 */
#ifndef PIN_STUB_ABI2_H
#define PIN_STUB_ABI2_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "../../src/api/pin_api.h"

#define PIN_ALIGN8(x) (((x) + 7u) & ~(size_t)7u)

/* ---- pin_capture_opts_t ------------------------------------------------ */
#define PIN_OPTS_V1_SIZE     (offsetof(pin_capture_opts_t, keep_raw) + sizeof(int))
#define PIN_OPTS_REWIND_OFF  PIN_OPTS_V1_SIZE
#define PIN_OPTS_V2_SIZE     (PIN_OPTS_V1_SIZE + sizeof(int))

static inline int pin_opts_size_ok(uint32_t sz) {
    return sz == PIN_OPTS_V1_SIZE || sz == PIN_OPTS_V2_SIZE;
}

/* rewind_first, or 0 for a v1 caller. */
static inline int pin_opts_rewind_first(const pin_capture_opts_t *o) {
    int v = 0;
    if (o->size >= PIN_OPTS_V2_SIZE) memcpy(&v, (const char *)o + PIN_OPTS_REWIND_OFF, sizeof v);
    return v;
}

/* ---- pin_status_snapshot_t --------------------------------------------- */
#define PIN_ST_V1_SIZE        (offsetof(pin_status_snapshot_t, audio_rms_db) + 2 * sizeof(float))
#define PIN_ST_DISKFREE_OFF   PIN_ALIGN8(PIN_ST_V1_SIZE)
#define PIN_ST_ESTSEC_OFF     (PIN_ST_DISKFREE_OFF + sizeof(uint64_t))
#define PIN_ST_DISKLOW_OFF    (PIN_ST_ESTSEC_OFF + sizeof(double))
#define PIN_ST_V2_SIZE        PIN_ALIGN8(PIN_ST_DISKLOW_OFF + sizeof(int))

static inline int pin_status_size_ok(uint32_t sz) {
    return sz == PIN_ST_V1_SIZE || sz == PIN_ST_V2_SIZE;
}

static inline void pin_status_set_disk(pin_status_snapshot_t *st, uint64_t free_bytes,
                                       double est_seconds_left, int disk_low) {
    if (st->size < PIN_ST_V2_SIZE) return;
    memcpy((char *)st + PIN_ST_DISKFREE_OFF, &free_bytes, sizeof free_bytes);
    memcpy((char *)st + PIN_ST_ESTSEC_OFF, &est_seconds_left, sizeof est_seconds_left);
    memcpy((char *)st + PIN_ST_DISKLOW_OFF, &disk_low, sizeof disk_low);
}

/* ---- pin_launch_t -------------------------------------------------------
 * Everything after the embedded capture opts is 4-byte fields, so the v2
 * layout is the v1 layout with the tail shifted by sizeof(int). */
#define PIN_LAUNCH_CAPTURE_OFF  offsetof(pin_launch_t, capture)
#define PIN_LAUNCH_TAIL_OFF     offsetof(pin_launch_t, capture_fields)   /* in the native header */
#define PIN_LAUNCH_TAIL_LEN     (sizeof(pin_launch_t) - PIN_LAUNCH_TAIL_OFF)
#define PIN_LAUNCH_V1_SIZE      (PIN_LAUNCH_CAPTURE_OFF + PIN_OPTS_V1_SIZE + PIN_LAUNCH_TAIL_LEN)
#define PIN_LAUNCH_V2_SIZE      (PIN_LAUNCH_V1_SIZE + sizeof(int))

/* Does the compiled header already have the v2 layout? */
#define PIN_NATIVE_IS_V2        (sizeof(pin_capture_opts_t) == PIN_OPTS_V2_SIZE)

/* Native (header) launch struct -> caller's v2 bytes, when the header is still v1. */
static inline void pin_launch_native_to_v2(const pin_launch_t *native, void *dst_v2) {
    char *d = (char *)dst_v2;
    int zero = 0;
    uint32_t v2 = (uint32_t)PIN_LAUNCH_V2_SIZE, ov2 = (uint32_t)PIN_OPTS_V2_SIZE;
    memcpy(d, native, PIN_LAUNCH_CAPTURE_OFF + PIN_OPTS_V1_SIZE);
    memcpy(d + PIN_LAUNCH_CAPTURE_OFF + PIN_OPTS_V1_SIZE, &zero, sizeof zero); /* rewind_first */
    memcpy(d + PIN_LAUNCH_CAPTURE_OFF + PIN_OPTS_V2_SIZE,
           (const char *)native + PIN_LAUNCH_TAIL_OFF, PIN_LAUNCH_TAIL_LEN);
    memcpy(d, &v2, sizeof v2);                                   /* launch.size */
    memcpy(d + PIN_LAUNCH_CAPTURE_OFF, &ov2, sizeof ov2);        /* launch.capture.size */
}

/* Caller's v2 bytes -> native (header v1) launch struct. */
static inline void pin_launch_v2_to_native(const void *src_v2, pin_launch_t *native) {
    const char *s = (const char *)src_v2;
    memcpy(native, s, PIN_LAUNCH_CAPTURE_OFF + PIN_OPTS_V1_SIZE);
    memcpy((char *)native + PIN_LAUNCH_TAIL_OFF, s + PIN_LAUNCH_CAPTURE_OFF + PIN_OPTS_V2_SIZE,
           PIN_LAUNCH_TAIL_LEN);
    native->size = (uint32_t)sizeof(pin_launch_t);
    native->capture.size = (uint32_t)sizeof(pin_capture_opts_t);
}

/* New entry point being added by the engine workstream. Declared here so
 * the stub exports it even while the frozen header doesn't declare it yet
 * (a later identical prototype in pin_api.h is compatible). */
PIN_API pin_status_t pin_set_output_hint(pin_session_t *s, const pin_capture_opts_t *o);

#endif /* PIN_STUB_ABI2_H */
