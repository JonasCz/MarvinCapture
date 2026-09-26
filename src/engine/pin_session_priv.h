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
 * Full definition of `struct pin_session` (pin_api.h only forward-declares
 * pin_session_t as opaque). Shared between pin_session.c (which owns the
 * worker thread and every state transition) and pin_preview.c (which needs
 * to reach into the same struct for its own mutex/fields without going
 * through the public pin_preview_* API from inside the engine). Nothing
 * outside src/engine and src/api/pin_api.c includes this header.
 */

#ifndef PIN_SESSION_PRIV_H
#define PIN_SESSION_PRIV_H

#include "../api/pin_api.h"
#include "../core/pinnacle_device.h"
#include "../core/pinnacle_lock.h"
#include "../core/pinnacle_1394.h"
#include "../core/pinnacle_analog.h"
#include "../core/pinnacle_stream.h"
#include "../core/dv_reassembler.h"
#include "../sinks/pin_sink.h"
#include "../sinks/pin_writer.h"
#include "pin_deck.h"
#include "pin_scene.h"
#include "pin_naming.h"
#include "pin_preview.h"
#include "pin_hdv_audio.h"
#include "pin_audio_resample.h"

#include <pthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- worker command mailbox ---------------------------------------------
 * One command in flight at a time (the API is documented as queuing to the
 * worker and returning at once); a second command while one is running
 * simply overwrites the mailbox -- fine for this session's command set,
 * where every command is either idempotent-ish (deck) or only valid in a
 * state that already implies the previous one finished (set_input/capture).
 */
typedef enum {
    PIN_CMD_NONE = 0,
    PIN_CMD_SET_INPUT,
    PIN_CMD_CAPTURE_START,
    PIN_CMD_CAPTURE_STOP,
    PIN_CMD_DECK,
    PIN_CMD_RUN_ACTIONS,
    PIN_CMD_CLOSE,
} pin_cmd_kind_t;

typedef struct {
    pin_cmd_kind_t kind;
    pin_input_t input;
    pin_capture_opts_t capture;
    int overwrite;
    pin_deck_cmd_t deck_cmd;
    pin_launch_t launch;
    int pending;
} pin_cmd_t;

/* Small FIFO of not-yet-committed raw units (DV frames / HDV pictures) so a
 * confirmed scene cut can be back-dated to its true first frame -- see
 * pin_scene.h and the plan's "debounce FIFO (hold back debounce_frames
 * units before committing to a file so the cut can be back-dated)". Sized
 * generously (32) since even at HDV's largest picture this is a few MB. */
#define PIN_SCENE_FIFO_CAP 32

typedef struct {
    uint8_t *data;
    size_t len;
    long frame_index;
} pin_scene_fifo_item_t;

struct pin_session {
    /* PIN_PATH_MAX, not PIN_NAME_MAX: a "replay:<file>" id embeds a whole
     * file path, which can exceed PIN_NAME_MAX (64) easily. This is purely
     * an internal buffer size -- the frozen pin_device_info_t.id field
     * really is PIN_NAME_MAX, so a long replay path shown by pin_enumerate()
     * is still truncated there (see pin_api.c's pin_enumerate()); only the
     * id string passed directly to pin_open(), which the ABI leaves
     * unbounded (a plain `const char *`), needs to survive intact here. */
    char device_id[PIN_PATH_MAX];
    pinnacle_device_t dev;
    pinnacle_lock_t *lock;

    pthread_t worker_thread;
    int worker_started;
    volatile int worker_stop;     /* ask the whole session to shut down */
    volatile int loop_stop;       /* ask just the current stream/analog loop to return */

    pthread_mutex_t mtx;          /* guards everything below except *_atomic-documented fields */

    pin_cmd_t cmd;
    pthread_cond_t cmd_posted;    /* worker wakes when a command lands */
    pthread_cond_t cmd_idle;      /* signalled when cmd.kind goes back to NONE */

    /* status */
    pin_state_t state;
    pin_status_t last_error;
    char error_text[PIN_PATH_MAX]; /* full text for the error event; the status snapshot gets a truncated copy */
    pin_input_t input;
    pin_kind_t stream_kind;
    int signal, is_60hz, width, height, dar_num, dar_den;
    pin_std_t detected_std;
    pin_deck_state_t deck;
    int deck_busy;
    char timecode[16];
    char rec_datetime[32];
    int tape_percent;
    int pass, passes, scene;
    char current_file[PIN_PATH_MAX];
    double elapsed_s, idle_s;
    uint64_t frames, frames_dropped, frames_damaged, lost_blocks, ts_errors;
    uint64_t bytes_written, writer_backlog, writer_backlog_max;
    float audio_peak_db[2], audio_rms_db[2];
    double audio_meter_t;   /* monotonic seconds of the last metered block; 0 = none yet */

    pin_std_t requested_std;
    pin_aspect_t aspect_override;

    /* capture pipeline (only valid while CAPTURING) */
    pin_capture_opts_t capture_opts;
    pinnacle_analog_t analog;
    dv_reassembler_t reasm;
    pin_writer_t *writer;
    pin_sink_t *sink;
    pin_format_t active_format;
    unsigned scene_index;         /* 1-based */
    unsigned pass_index;          /* 1-based */
    uint64_t unit_index;
    long scene_frame_counter;
    pin_scene_detector_t scene_det;
    int scene_det_ready;
    unsigned opts_scene_split_window;
    int hdv_await_gop;       /* HDV capture: nothing written until a GOP (sequence header) arrives */
    int stream_kind_known;   /* first frame/GOP seen: DV-vs-HDV (or analog) is settled */
    int capture_want_start;  /* CAPTURE_START arrived before stream_kind_known, or is
                                 waiting on rewind_before_capture; deferred */
    int rewind_before_capture; /* "Play and capture" with rewind_first: REWINDING now,
                                   start_capture_now() once BOT (deck stopped) is seen */
    pin_capture_opts_t output_hint; /* pin_set_output_hint(): format/path while READY, for
                                        est_seconds_left before a capture actually starts */
    int have_output_hint;
    pin_scene_fifo_item_t scene_fifo[PIN_SCENE_FIFO_CAP];
    unsigned scene_fifo_head, scene_fifo_count; /* ring, oldest at head */
    double capture_start_s, last_data_s;
    char naming_base[PIN_PATH_MAX]; /* extension-stripped */
    char naming_ext[16];

    /* DV/HDV link + deck, valid once PREPARING for DV has completed */
    pinnacle_1394_t link;
    pin_deck_async_t deck_async;
    double last_transport_poll_s;

    /* replay (virtual device) */
    int is_replay;
    char replay_path[PIN_PATH_MAX];

    /* preview */
    pin_preview_t *preview;

    /* audio monitor ring (48 kHz s16 stereo) */
    int16_t *mon_buf;
    size_t mon_cap_frames;     /* capacity, frames (stereo pairs) */
    size_t mon_head;           /* next write position */
    size_t mon_fill;           /* frames currently valid */
    int mon_enabled;
    pthread_mutex_t mon_mtx;

    /* DV/HDV audio feeding the meters + monitor ring above (analog feeds
     * them directly from analog_audio_cb(); see dv_on_unit() for DV/HDV).
     * DV: dv_audio_extract() is cheap enough to run inline on whatever
     * thread dv_on_unit() runs on (the USB read loop, or a replay thread),
     * so it only needs a resampler's persistent state, not a thread of its
     * own. HDV needs libavcodec's mp2 decoder, which is not cheap enough
     * for that thread, hence pin_hdv_audio_t's own thread + queue. */
    pin_resampler_t dv_audio_rs;
    pin_hdv_audio_t *hdv_audio;

    /* events */
#define PIN_EVQ_CAP 256
    pin_event_t evq[PIN_EVQ_CAP];
    unsigned evq_head, evq_count;
    pthread_mutex_t evq_mtx;

    /* action sequencer */
    pin_launch_t launch;
    int actions_running;
    int action_index;
};

/* Pushes an event to s's queue (s may be NULL: process-wide, handled by
 * pin_log's sink in pin_api.c). Drops the oldest LOG event first if full,
 * else the oldest event of any kind. */
void pin_session_push_event(pin_session_t *s, pin_event_kind_t kind, int32_t a, const char *text);

/* Locked accessors used by pin_api.c and pin_preview.c. */
void pin_session_lock(pin_session_t *s);
void pin_session_unlock(pin_session_t *s);

double pin_session_now(void);

#ifdef __cplusplus
}
#endif

#endif /* PIN_SESSION_PRIV_H */
