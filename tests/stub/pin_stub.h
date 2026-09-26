/*
 * pinnacle-oss-core STUB implementation — internal shared header.
 *
 * This is a hardware-free fake of the pin_api.h ABI (see
 * src/api/pin_api.h, which is frozen and NOT modified here). It exists so
 * the Windows GUI can be built and exercised end to end with zero hardware
 * attached. It is built as pinnacle-oss-core.dll, the exact name the real
 * core library will eventually use, so the GUI never has to change.
 *
 * Not shared with, or built alongside, anything under src (other than the
 * frozen ABI header). This stub is self-contained under tests/stub.
 */
#ifndef PIN_STUB_H
#define PIN_STUB_H

#define PIN_BUILDING_LIBRARY 1
#include "../../src/api/pin_api.h"
#include "pin_stub_abi2.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- time helper -------------------------------------------------------- */

double pin_stub_now(void); /* monotonic seconds, double */
void pin_stub_sleep_ms(int ms);

/* ---- event queue ---------------------------------------------------------
 * Bounded ring buffer. If full: drop the oldest PIN_EVT_LOG event if any
 * exists in the queue (logs are the least important); otherwise drop the
 * oldest event of any kind to make room. Never blocks pushers.
 */
#define PIN_STUB_EVT_CAP 256

typedef struct {
    pin_event_t items[PIN_STUB_EVT_CAP];
    int head, count;
    pthread_mutex_t lock;
} pin_evtq_t;

void pin_evtq_init(pin_evtq_t *q);
void pin_evtq_destroy(pin_evtq_t *q);
void pin_evtq_push(pin_evtq_t *q, pin_event_kind_t kind, int32_t a, const char *text);
int pin_evtq_pop(pin_evtq_t *q, pin_event_t *out);

/* ---- fake devices --------------------------------------------------------
 * Exactly two fake devices, as specified: one READY, one IN_USE by a fake
 * "other process" (pid 1234). Their conceptual state can change when this
 * process opens/closes the READY one (-> OPEN_HERE and back).
 */
typedef struct {
    char id[PIN_NAME_MAX];
    char name[PIN_NAME_MAX];
    char serial[PIN_NAME_MAX]; /* cached once opened */
    uint16_t vid, pid;
    pin_dev_state_t base_state;   /* READY or IN_USE, ignoring "opened here" */
    uint32_t owner_pid;
    int opened_here;              /* this process currently holds it */
} pin_stub_device_t;

#define PIN_STUB_DEVICE_COUNT 2
extern pin_stub_device_t g_pin_stub_devices[PIN_STUB_DEVICE_COUNT];
extern pthread_mutex_t g_pin_stub_devices_lock;

void pin_stub_devices_init(void);

/* ---- preview frame buffer (triple buffer) -------------------------------- */

#define PIN_STUB_PLANE_MAX (1920 * 1088)

typedef struct {
    uint64_t seq;
    int width, height;
    int chroma_shift_x, chroma_shift_y;
    int stride[3];
    pin_matrix_t matrix;
    int full_range;
    int dar_num, dar_den;
    int interlaced, top_field_first;
    uint8_t *plane[3]; /* heap allocated, sized for the frame */
    size_t plane_cap[3]; /* allocated bytes per plane */
} pin_stub_frame_t;

/* ---- deck fake state ------------------------------------------------------ */

typedef struct {
    pin_deck_state_t state;
    int busy;                 /* AV/C command "in flight" */
    double busy_until;        /* monotonic time when busy clears */
    pin_deck_state_t pending_state; /* state to apply once busy clears */
    double tape_percent;      /* 0..100, fractional internally */
    int no_tape;
    /* timecode, frames @ 25fps non-drop for simplicity */
    long long tc_frames;      /* absolute frame count, can't go below 0 */
} pin_stub_deck_t;

/* ---- capture fake state ---------------------------------------------------- */

typedef struct {
    int active;
    pin_capture_opts_t opts;
    int overwrite;
    FILE *fp;
    char path[PIN_PATH_MAX];
    double start_time;
    double last_grow_time;
    uint64_t frames, bytes_written;
    int pass, passes;
    int scene;
    double stopping_since;
    int stopping;
} pin_stub_capture_t;

/* ---- the session ------------------------------------------------------------ */

struct pin_session {
    pthread_mutex_t lock;      /* guards the fields below (not the queues/preview) */
    pthread_cond_t cond;       /* generic "something changed", used by worker sleep */

    int device_index;          /* index into g_pin_stub_devices */
    char device_id[PIN_NAME_MAX];

    volatile int closing;
    pthread_t worker_thread;
    pthread_t preview_thread;
    int worker_started, preview_started;

    pin_state_t state;
    pin_status_t last_error;
    char error_text[PIN_TEXT_MAX];

    pin_input_t input;
    pin_kind_t stream_kind;    /* what is "arriving" */
    int prepare_pending;       /* PREPARING -> READY timer active */
    double prepare_until;

    int signal;
    int is_60hz;
    int width, height;
    int dar_num, dar_den;
    pin_std_t requested_std;   /* what pin_set_standard was told */
    pin_std_t detected_std;    /* effective, used for hue enable etc. */

    pin_aspect_t aspect;

    int32_t controls[PIN_CTL_COUNT];

    pin_stub_deck_t deck;
    pin_stub_capture_t cap;

    double opened_at;
    double hdv_toggle_at;      /* next time DV/HDV alternation flips */
    int hdv_locked;            /* PIN_STUB_HDV=1 */
    int is_dv_alternate_hdv;   /* current alternation flag for usb:1-4 */

    /* preview */
    pthread_mutex_t preview_lock;
    pthread_cond_t preview_cond;
    pin_stub_frame_t frame[3];
    int frame_front;           /* index most recently completed */
    int preview_enabled;
    uint64_t preview_seq;
    int locked_index;          /* index currently locked out via pin_preview_lock, -1 if none */

    /* audio monitor */
    int monitor_enabled;
    int16_t *ring;
    int ring_cap;   /* frames */
    int ring_head, ring_count;
    pthread_mutex_t ring_lock;
    double audio_phase;

    pin_evtq_t events;

    /* pin_set_output_hint(): where the next capture would go, for the
     * disk-free / time-left estimate before a capture starts. */
    pin_capture_opts_t hint;
    int has_hint;
    double disk_cache_time;       /* pin_stub_now() of the last free-space query */
    uint64_t disk_cache_free;
};

/* Free bytes at the active capture's (or the hinted) output folder and the
 * estimated seconds left at that format's data rate. Caller holds s->lock. */
void pin_stub_estimate_disk(pin_session_t *s, uint64_t *free_bytes, double *seconds_left);

/* ---- shared helper: push a state-changing event + update state under lock */
void pin_stub_set_state(pin_session_t *s, pin_state_t st);

/* worker/preview thread entry points, defined in pin_stub_session.c /
 * pin_stub_preview.c respectively */
void *pin_stub_worker_main(void *arg);
void *pin_stub_preview_main(void *arg);

/* called by the worker thread each tick to advance deck + capture + audio */
void pin_stub_deck_tick(pin_session_t *s, double now);
void pin_stub_capture_tick(pin_session_t *s, double now);

/* settings backend (ini-backed) */
void pin_stub_settings_init(void);

/* Shared geometry table for a stream kind, used by both the status
 * snapshot and the preview frame generator so they always agree.
 * aspect_override: PIN_ASPECT_AUTO uses the kind's natural DAR. */
void pin_stub_kind_geometry(pin_kind_t kind, int is_60hz, pin_aspect_t aspect_override,
                             int *w, int *h, int *dar_num, int *dar_den,
                             int *chroma_shift_x, int *chroma_shift_y, pin_matrix_t *matrix);

#ifdef __cplusplus
}
#endif

#endif /* PIN_STUB_H */
