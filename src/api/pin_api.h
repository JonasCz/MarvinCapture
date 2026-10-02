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
 * pinnacle-oss-core public API.
 *
 * This is the only header a front-end (the WinUI app, a future macOS or
 * Linux GUI, pinctl) includes. It is a flat C ABI on purpose: opaque
 * handles, fixed-size POD structs, UTF-8 strings in fixed buffers, and no
 * callbacks into the caller. Everything that happens asynchronously is
 * *polled*:
 *
 *   - pin_get_status()      a snapshot of the session, cheap, call at ~10 Hz
 *   - pin_poll_event()      a FIFO of discrete happenings (state changes,
 *                           files opened/closed, log lines, errors)
 *   - pin_preview_wait()    blocks a caller-owned render thread until a new
 *                           preview frame exists, then lock/unlock it
 *
 * That keeps managed runtimes (C#, Swift) out of the business of being
 * called back on engine threads, and it keeps per-GUI code down to widgets
 * and a renderer: option lists, what is enabled for which format, file
 * naming, status text and settings all come from here.
 *
 * Threading: every function may be called from any thread. Functions that
 * take a session serialise on it internally; the ones documented as
 * "non-blocking" only take a short lock and never touch USB. Commands that
 * drive hardware (prepare, input switch, deck, capture) are queued to the
 * session's worker thread and return at once; their outcome shows up in
 * the status snapshot and as events.
 *
 * Struct versioning: structs the caller fills or receives start with a
 * `size` field set to sizeof() as the caller compiled it. The library
 * rejects a size it does not know (PIN_ERR_ABI) rather than read past the
 * end. New fields are only ever appended.
 */

#ifndef PIN_API_H
#define PIN_API_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(PIN_BUILDING_LIBRARY)
#    define PIN_API __declspec(dllexport)
#  else
#    define PIN_API __declspec(dllimport)
#  endif
#else
#  define PIN_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define PIN_API_VERSION 2

#define PIN_PATH_MAX 1024 /* bytes of UTF-8, including the terminator */
#define PIN_NAME_MAX 64
#define PIN_TEXT_MAX 256

/* ---- basics ---------------------------------------------------------- */

typedef enum {
    PIN_OK = 0,
    PIN_ERR_ABI,            /* struct size / API version mismatch */
    PIN_ERR_ARG,            /* bad argument */
    PIN_ERR_STATE,          /* not allowed in the current session state */
    PIN_ERR_NOT_FOUND,      /* device gone / no such id */
    PIN_ERR_BUSY,           /* device in use by another process */
    PIN_ERR_NO_DRIVER,      /* Windows: device not bound to WinUSB */
    PIN_ERR_USB,
    PIN_ERR_FIRMWARE,       /* FPGA bitstream missing, unreadable or rejected; the error event says which */
    PIN_ERR_NOT_READY,      /* device needs a power cycle (replug) */
    PIN_ERR_NO_CAMERA,      /* DV: no camera / deck on the 1394 bus */
    PIN_ERR_DECK,           /* DV: AV/C command rejected */
    PIN_ERR_IO,             /* file system */
    PIN_ERR_EXISTS,         /* output would overwrite an existing file */
    PIN_ERR_DISK_FULL,
    PIN_ERR_CODEC,          /* muxer / encoder failure */
    PIN_ERR_NOMEM,
    PIN_ERR_INTERNAL,
} pin_status_t;

PIN_API uint32_t pin_api_version(void);          /* == PIN_API_VERSION */
PIN_API const char *pin_version_string(void);    /* "pinnacle-oss-core 0.x (git ...)" */
PIN_API const char *pin_strerror(pin_status_t s);

/* Optional. Where the FPGA bitstreams live. Searched in order: this dir,
 * the settings key Paths/firmware_dir, firmware/ next to the core library,
 * firmware/ next to the executable. The build puts firmware/ next to the
 * library. */
PIN_API pin_status_t pin_set_firmware_dir(const char *utf8_dir);

/* ---- devices ---------------------------------------------------------- */

typedef enum {
    PIN_DEV_READY = 0,      /* present, free */
    PIN_DEV_PREPARING,      /* another process is loading firmware on it */
    PIN_DEV_IN_USE,         /* another process has it open */
    PIN_DEV_OPEN_HERE,      /* this process has it open */
    PIN_DEV_NO_DRIVER,      /* Windows: needs WinUSB (Zadig) */
    PIN_DEV_UNSUPPORTED,    /* a sibling model we recognise but don't drive yet */
} pin_dev_state_t;

typedef struct {
    uint32_t size;
    char id[PIN_NAME_MAX];      /* stable while plugged in: USB port path, e.g. "usb:1-4.2";
                                   "replay:<file>" for the virtual device */
    char name[PIN_NAME_MAX];    /* "Pinnacle Studio 500-USB" */
    char serial[PIN_NAME_MAX];  /* 1394 GUID as hex once known (cached by the core), else "" */
    uint16_t vid, pid;
    pin_dev_state_t state;
    uint32_t owner_pid;         /* PREPARING / IN_USE: the other process, else 0 */
    uint32_t tested;            /* 1 = model verified on real hardware, 0 = supported but untested */
} pin_device_info_t;

/* Lists devices without opening them. Cheap enough to call on every OS
 * device-change notification. Fills up to max entries and returns how many
 * exist (which may exceed max). Each out[i].size must be set by the caller.
 * Includes the virtual replay device (id "replay:<basename>") whenever the
 * PIN_REPLAY environment variable or pin_set_replay_file() names a file. */
PIN_API int pin_enumerate(pin_device_info_t *out, int max);

/* Sets (or, with NULL/"", clears) the process-wide replay source: a file
 * pin_enumerate() lists as a virtual "replay:<basename>" device and
 * pin_open() can open by that id (or by the full path). Lets a GUI's "Open
 * a capture file..." replace the old PIN_REPLAY environment variable /
 * settings-key mechanism. Not persisted; set again after every restart. */
PIN_API void pin_set_replay_file(const char *path);

/* Blocks until the device set, or any known device's cross-process lock
 * state (Ready/Preparing/In use -- see pin_device_info_t.state), may have
 * changed, so a GUI needs no polling loop. Returns 1 if something may have
 * changed (re-call pin_enumerate() to see what), 0 on timeout, <0 on error.
 * Several hotplug events in quick succession (Windows in particular fires
 * more than one per physical plug) are coalesced into a single return.
 * Safe to call from one thread at a time per process; a second concurrent
 * call is not supported. */
PIN_API int pin_devices_wait(int timeout_ms);

/* Makes a pending/future pin_devices_wait() return 0 (timeout) at once;
 * for clean shutdown of a thread blocked in it. Safe to call any time,
 * including with no wait outstanding. */
PIN_API void pin_devices_wake(void);

/* ---- sessions ---------------------------------------------------------- */

typedef struct pin_session pin_session_t;

typedef enum {
    PIN_STATE_CLOSED = 0,
    PIN_STATE_PREPARING,    /* firmware upload / bring-up / input switch */
    PIN_STATE_READY,        /* idle, preview running if a signal is present */
    PIN_STATE_CAPTURING,
    PIN_STATE_STOPPING,     /* finalising files (may take a moment for remuxes) */
    PIN_STATE_REWINDING,    /* multi-pass: rewinding to the start of the tape */
    PIN_STATE_ERROR,        /* see last_error; close and reopen, or pin_prepare() again */
} pin_state_t;

typedef enum {
    PIN_INPUT_DV = 0,       /* DV or HDV over 1394, auto-detected */
    PIN_INPUT_SVIDEO,
    PIN_INPUT_COMPOSITE,
} pin_input_t;

/* Takes the device (cross-process lock), starts the worker thread. Does no
 * slow hardware work; call pin_set_input() to bring the device up.
 *
 * device_id may be: NULL/"first" (the first ready device, or the PIN_REPLAY
 * env var / pin_set_replay_file() source if set); a pin_device_info_t.id
 * exactly as pin_enumerate() reports it (e.g. "usb:1-8"); a device's serial
 * (pin_device_info_t.serial, 16 hex chars, matched case-insensitively) when
 * no id matches it literally; "replay:<basename>" for the configured replay
 * file; or a path to an existing file, which is equivalent to calling
 * pin_set_replay_file() with it first and then opening it by name. */
PIN_API pin_status_t pin_open(const char *device_id, pin_session_t **out);

/* Stops any capture (finalising files), releases the device. Blocks until
 * done. Safe with NULL. */
PIN_API void pin_close(pin_session_t *s);

/* Selects the input and (re)prepares the device for it: loads the right
 * FPGA design when switching between DV and analog. Non-blocking; state
 * goes PREPARING -> READY or ERROR. Not allowed while capturing. */
PIN_API pin_status_t pin_set_input(pin_session_t *s, pin_input_t input);

/* ---- analog controls (live, also during capture) ----------------------- */

typedef enum {
    PIN_STD_AUTO = 0,       /* 50/60 Hz from the decoder's detector: PAL or NTSC */
    PIN_STD_PAL, PIN_STD_NTSC, PIN_STD_PAL_M, PIN_STD_PAL_N, PIN_STD_PAL_60,
    PIN_STD_NTSC_443, PIN_STD_NTSC_J, PIN_STD_SECAM,
    PIN_STD_COUNT
} pin_std_t;

typedef enum {
    PIN_CTL_BRIGHTNESS = 0,
    PIN_CTL_CONTRAST,
    PIN_CTL_SATURATION,
    PIN_CTL_HUE,            /* NTSC only; see pin_control_info_t.enabled */
    PIN_CTL_SHARPNESS,
    PIN_CTL_AUDIO_GAIN,     /* line-in gain, in dB * 10 */
    PIN_CTL_COUNT
} pin_control_t;

typedef struct {
    uint32_t size;
    char label[PIN_NAME_MAX];   /* "Brightness", ... (English; GUIs may localise by id) */
    int32_t min, max, step, def;
    int32_t value;              /* current */
    int enabled;                /* e.g. hue is disabled for PAL/SECAM */
} pin_control_info_t;

PIN_API const char *pin_std_name(pin_std_t std);
PIN_API pin_status_t pin_set_standard(pin_session_t *s, pin_std_t std);
PIN_API pin_status_t pin_get_control(pin_session_t *s, pin_control_t c, pin_control_info_t *out);
PIN_API pin_status_t pin_set_control(pin_session_t *s, pin_control_t c, int32_t value);

/* ---- deck control (DV input) ------------------------------------------ */

typedef enum {
    PIN_DECK_UNKNOWN = 0,
    PIN_DECK_STOPPED,
    PIN_DECK_PLAYING,
    PIN_DECK_PAUSED,
    PIN_DECK_FAST_FORWARD,
    PIN_DECK_REWINDING,
    PIN_DECK_RECORDING,     /* the camera is recording (camera mode) */
    PIN_DECK_NO_TAPE,
} pin_deck_state_t;

typedef enum {
    PIN_DECK_CMD_PLAY = 0,
    PIN_DECK_CMD_PAUSE,
    PIN_DECK_CMD_STOP,      /* while capturing: stops the capture first, then the deck */
    PIN_DECK_CMD_FF,
    PIN_DECK_CMD_REW,
} pin_deck_cmd_t;

/* Non-blocking. While capturing only PIN_DECK_CMD_STOP is accepted. */
PIN_API pin_status_t pin_deck(pin_session_t *s, pin_deck_cmd_t cmd);

/* ---- output formats ----------------------------------------------------- */

typedef enum {
    PIN_FMT_ANALOG_AVI = 0,     /* uncompressed YUY2 + PCM, OpenDML */
    PIN_FMT_ANALOG_FFV1_MKV,    /* FFV1 level 3 + PCM in Matroska */
    PIN_FMT_DV_RAW,             /* .dv */
    PIN_FMT_DV_AVI,             /* DV type-2 AVI */
    PIN_FMT_DV_MOV,             /* DV in QuickTime */
    PIN_FMT_HDV_TS,             /* .ts (m2t) */
    PIN_FMT_HDV_MOV,
    PIN_FMT_HDV_MKV,
    PIN_FMT_COUNT
} pin_format_t;

typedef enum {
    PIN_KIND_ANALOG = 0,
    PIN_KIND_DV,
    PIN_KIND_HDV,
} pin_kind_t;

typedef struct {
    uint32_t size;
    pin_format_t format;
    pin_kind_t kind;
    char label[PIN_NAME_MAX];   /* "DV in AVI (type 2)" */
    char extension[16];         /* "avi" (no dot) */
    int supports_title;
    int supports_scene_split;
    int supports_multi_pass;
    int is_default;             /* the default format for its kind */
} pin_format_info_t;

/* Formats for a kind, in display order. Returns the count. */
PIN_API int pin_formats(pin_kind_t kind, pin_format_info_t *out, int max);
PIN_API pin_status_t pin_format_info(pin_format_t f, pin_format_info_t *out);

/* ---- capture ------------------------------------------------------------ */

typedef enum {
    PIN_ASPECT_AUTO = 0,    /* DV: from the stream; HDV: 16:9; analog: 4:3 */
    PIN_ASPECT_4_3,
    PIN_ASPECT_16_9,
} pin_aspect_t;

typedef struct {
    uint32_t size;
    char path[PIN_PATH_MAX];    /* base output path; extension optional */
    pin_format_t format_analog; /* used when the input is analog */
    pin_format_t format_dv;     /* used when the stream turns out to be DV */
    pin_format_t format_hdv;    /* ... or HDV */
    char title[PIN_TEXT_MAX];   /* metadata title; ignored if unsupported */
    pin_aspect_t aspect;
    int scene_split;            /* DV/HDV: new numbered file per recording */
    int idle_stop_minutes;      /* stop (or end the pass) after this long without data / signal; 0 = never */
    int passes;                 /* DV/HDV: capture the tape this many times (>= 1) */
    int start_deck;             /* DV/HDV: send PLAY first ("Play and capture") */
    int keep_raw;               /* also keep the raw .dv/.ts next to a rewrapped file */
    int rewind_first;           /* DV/HDV: with start_deck, REWIND to the start of the tape,
                                    wait for BOT, then PLAY, then capture ("Play and capture"
                                    now rewinds first). Appended field, ABI-compatible. */
    unsigned first_number;      /* 0 = legacy naming (plain "base.ext"; "-NNNN" only with
                                    scene_split, from 1). >= 1: every file is numbered
                                    "base-NNNN.ext", starting at this number and counting up
                                    per scene; the caller picks an unused number. Appended. */
    int max_duration_minutes;   /* stop after this long of capture time, signal/data or not;
                                    0 = never. DV/HDV: applies across all passes (cuts a pass
                                    short rather than rewinding for the next one). Appended
                                    field, ABI-compatible. */
} pin_capture_opts_t;

PIN_API void pin_capture_opts_defaults(pin_capture_opts_t *o);

typedef struct {
    uint32_t size;
    uint64_t free_bytes;
    uint64_t minutes_left;      /* at the expected data rate of the chosen format */
    int fat32;                  /* files over 4 GiB would fail */
    int collision;              /* some output name already exists */
    char first_path[PIN_PATH_MAX]; /* the first file the capture would write */
    char message[PIN_TEXT_MAX];    /* human-readable summary of any problem, else "" */
    int low_space;              /* free_bytes is known and below 25 GiB: the GUI asks before capturing
                                   (not part of message, which has its own prompt). Appended field. */
} pin_output_check_t;

/* Checks free space, file system limits and name collisions before a
 * capture. input tells it which format/data rate applies. */
PIN_API pin_status_t pin_check_output(pin_session_t *s, const pin_capture_opts_t *o,
                                      pin_output_check_t *out);

/* Non-blocking. Lets the core track the target volume/format while READY
 * (before a capture actually starts), so pin_get_status()'s disk_free_bytes
 * / est_seconds_left / disk_low are meaningful in the GUI's output panel
 * ahead of time. Safe to call repeatedly (e.g. every time the user edits
 * the output path or format); has no effect once CAPTURING (the real
 * capture's own opts take over then). */
PIN_API pin_status_t pin_set_output_hint(pin_session_t *s, const pin_capture_opts_t *o);

/* Non-blocking. Refuses with PIN_ERR_EXISTS if a target file exists unless
 * overwrite is set (the GUI asks the user first). */
PIN_API pin_status_t pin_capture_start(pin_session_t *s, const pin_capture_opts_t *o, int overwrite);

/* Non-blocking. State goes STOPPING -> READY. Also stops the deck if the
 * capture started it. */
PIN_API pin_status_t pin_capture_stop(pin_session_t *s);

/* ---- status --------------------------------------------------------------- */

typedef struct {
    uint32_t size;
    pin_state_t state;
    pin_status_t last_error;
    char error_text[PIN_TEXT_MAX];

    pin_input_t input;
    pin_kind_t stream_kind;     /* what is actually arriving (DV vs HDV detected) */
    int signal;                 /* analog: decoder locked; DV: data arriving */
    int is_60hz;
    int width, height;
    int dar_num, dar_den;       /* display aspect of what is arriving */
    pin_std_t detected_std;

    pin_deck_state_t deck;
    int deck_busy;              /* an AV/C command is in flight / being retried */
    char timecode[16];          /* "HH:MM:SS:FF" (";" before FF for drop-frame), or "" */
    char rec_datetime[32];      /* "YYYY-MM-DD HH:MM:SS" from the tape, or "" */
    int tape_percent;           /* 0..100, or -1 when the deck doesn't report it */

    /* capture */
    int pass, passes;
    int scene;                  /* 1-based scene / file number */
    char current_file[PIN_PATH_MAX];
    double elapsed_s;
    uint64_t frames;
    uint64_t frames_dropped;    /* not in the output (analog: the previous frame repeated) */
    uint64_t frames_damaged;    /* analog: the dropped frames that arrived short; DV: sequences zero-padded */
    uint64_t lost_blocks;       /* DV/HDV: CIP data blocks lost in transit */
    uint64_t ts_errors;         /* HDV: continuity errors */
    uint64_t bytes_written;
    uint64_t writer_backlog;    /* bytes queued for the disk */
    uint64_t writer_backlog_max;
    double idle_s;              /* seconds since data / signal was last seen */

    float audio_peak_db[2];     /* dBFS, -inf as -144; loudest sample since the previous status read (the latest block if none new) */
    float audio_rms_db[2];

    /* Appended fields, ABI-compatible (PIN_API_VERSION unchanged). Disk
     * space on the output volume: while CAPTURING, from the live data rate;
     * else estimated from pin_set_output_hint()'s format at its nominal
     * rate, or 0/0 if no hint was ever given. */
    uint64_t disk_free_bytes;
    double est_seconds_left;
    int disk_low;               /* free space/time is getting low (< 1 hour or < 50 GB) */

    /* Appended fields, ABI-compatible.
     * detail: one line saying what the session is doing or waiting for, in
     * plain English -- the current bring-up step while PREPARING ("Uploading
     * FPGA firmware (42%)"), and the reason there is no picture while READY
     * without a signal ("No camera found ..."). Empty when there is nothing
     * to say. Shown in the preview pane and the status bar by every GUI.
     * progress_percent: 0..100 for the current PREPARING step if it has a
     * measurable length, else -1.
     * camera_present: DV / HDV input only -- 1 a camera answered on the 1394
     * bus, 0 none did (the deck controls have nothing to talk to), -1 not
     * applicable (analog input) or not known yet. */
    char detail[PIN_TEXT_MAX];
    int progress_percent;
    int camera_present;

    /* Appended fields, ABI-compatible. Video frames and audio blocks that
     * arrived but never reached the file because the disk writer's queue
     * was full (or the file had already failed). Always 0 in a good capture. */
    uint64_t write_dropped;

    /* Appended fields, ABI-compatible. Seconds until the capture stops by
     * itself, -1 when that limit is off or no capture is running:
     * idle_stop_remaining_s: the no-signal timeout (idle_stop_minutes); counts
     *   down from the full timeout while data / signal arrives (it restarts
     *   with every frame), so show it only while `signal` is 0.
     * duration_remaining_s: the total capture time limit (max_duration_minutes),
     *   over all passes. */
    double idle_stop_remaining_s;
    double duration_remaining_s;

    /* Appended fields, ABI-compatible. Frame error statistics. `frames`,
     * `frames_dropped` and frames_error are the TOTAL: since the capture
     * started, or since the session started (or the input was switched) while
     * not capturing. The clip_* ones are the same for the current output file:
     * they restart whenever a file is opened (capture start, scene split, new
     * pass); while not capturing they equal the total.
     * A frame is "with error" if any of these hit it:
     *   DV:     a DIF block missing/garbled (a lost sequence is zero-padded by
     *           the reassembler), a video block with a non-zero STA error /
     *           concealment status, an audio block carrying the error fill, a
     *           partially muted audio channel pair (Sony deck quirk), a camera
     *           that re-encoded the whole frame (STA 14, Samsung quirk);
     *   HDV:    transport_error_indicator, a continuity gap, a missing PES /
     *           picture header; and any B / P picture that depends on a
     *           damaged I / P picture of the same GOP;
     *   analog: a repeated (dropped) frame.
     * frames_dropped: DV frames with >= 90 % of their blocks missing, analog
     * repeated frames; HDV has no frame-level drop notion (always 0).
     * err_*_blocks: running totals behind the verdicts (DV: video blocks with
     * STA != 0, audio blocks bad or muted, blocks missing; HDV: damaged
     * pictures in err_video_blocks, TEI + continuity gaps + sync losses in
     * err_missing_blocks). */
    uint64_t frames_error;
    uint64_t clip_frames;
    uint64_t clip_frames_error;
    uint64_t clip_frames_dropped;
    uint64_t err_video_blocks;
    uint64_t err_audio_blocks;
    uint64_t err_missing_blocks;

    /* Appended fields, ABI-compatible. Sizes and the time-left estimate.
     * bytes_written above is the CURRENT file; clip_bytes_written is the same
     * value under a clearer name and total_bytes_written adds the files this
     * capture already finished (0 before the first capture; stays at the last
     * capture's total until the next one starts).
     * est_seconds_left (and disk_free_bytes) refer to the output volume;
     * hours left = est_seconds_left / 3600. It uses est_bytes_per_hour:
     * est_rate_source 0 = built-in nominal rate (DV and HDV 13 GB/h, analog AVI
     * computed from the picture size, FFV1 30 GB/h), 1 = FFV1 rate learned from
     * an earlier capture (settings key core.ffv1_bytes_per_hour, the average of
     * the last 10 minutes of that capture), 2 = measured on the running
     * capture over the last 10 minutes (once >= 10 s of data exist). */
    uint64_t clip_bytes_written;
    uint64_t total_bytes_written;
    double est_bytes_per_hour;
    int est_rate_source;
} pin_status_snapshot_t;

/* Non-blocking. */
PIN_API pin_status_t pin_get_status(pin_session_t *s, pin_status_snapshot_t *out);

/* Ready-made text so every GUI shows the same thing. */
PIN_API void pin_format_status_line(const pin_status_snapshot_t *st, char *out, size_t cap);
/* Seconds as "5m30s" / "1h02m10s" / "45s" (rounded up), for the stop countdowns
 * (idle_stop_remaining_s, duration_remaining_s). */
PIN_API void pin_format_remaining(double seconds, char *out, size_t cap);
PIN_API void pin_format_window_title(const pin_status_snapshot_t *st, const char *device_name,
                                     char *out, size_t cap);

/* ---- events ----------------------------------------------------------- */

typedef enum {
    PIN_EVT_STATE = 0,      /* state changed: a = new pin_state_t */
    PIN_EVT_FILE_OPENED,    /* text = path */
    PIN_EVT_FILE_CLOSED,    /* text = path, a = pin_status_t of the finalise */
    PIN_EVT_SCENE,          /* a new scene started: a = scene number */
    PIN_EVT_PASS,           /* a new pass started: a = pass number */
    PIN_EVT_DECK,           /* a = pin_deck_state_t */
    PIN_EVT_INPUT_FORMAT,   /* signal / stream format changed; re-read the status */
    PIN_EVT_LOG,            /* a = level (0 debug .. 3 error), text = message */
    PIN_EVT_ERROR,          /* a = pin_status_t, text = message */
    PIN_EVT_DONE,           /* the action list (pin_run_actions) finished: a = pin_status_t */
    PIN_EVT_DEVICES,        /* device list may have changed (this process's own view) */
} pin_event_kind_t;

typedef struct {
    uint32_t size;
    pin_event_kind_t kind;
    int32_t a;
    char text[PIN_PATH_MAX];
} pin_event_t;

/* Non-blocking. Returns 1 and fills *out if an event was pending, else 0.
 * The queue is bounded; if the caller falls behind, the oldest log events
 * are dropped first. s may be NULL for process-wide log events. */
PIN_API int pin_poll_event(pin_session_t *s, pin_event_t *out);

/* Minimum level for PIN_EVT_LOG (default 1 = info). */
PIN_API void pin_set_log_level(int level);

/* ---- preview ------------------------------------------------------------ */

typedef enum {
    PIN_MATRIX_BT601 = 0,
    PIN_MATRIX_BT709,
} pin_matrix_t;

typedef struct {
    uint32_t size;
    uint64_t seq;               /* increments per new frame */
    int width, height;          /* luma plane */
    int chroma_shift_x;         /* chroma width = width >> shift (1 for 4:2:2/4:2:0, 2 for 4:1:1) */
    int chroma_shift_y;         /* chroma height = height >> shift (1 for 4:2:0) */
    const uint8_t *plane[3];    /* Y, Cb, Cr */
    int stride[3];
    pin_matrix_t matrix;
    int full_range;             /* 0 = limited (16-235) */
    int dar_num, dar_den;       /* display aspect ratio, aspect override applied */
    int interlaced;
    int top_field_first;
} pin_frame_t;

/* The preview decodes on its own low-priority thread and never slows a
 * capture: if the renderer falls behind, frames are skipped. */

/* Blocks up to timeout_ms for a frame newer than after_seq. Returns 1 if
 * one is available, 0 on timeout, -1 if the session is closing. */
PIN_API int pin_preview_wait(pin_session_t *s, uint64_t after_seq, int timeout_ms);

/* Borrows the newest frame. Planes stay valid until pin_preview_unlock();
 * hold them only long enough to upload. Returns PIN_ERR_STATE if none. */
PIN_API pin_status_t pin_preview_lock(pin_session_t *s, pin_frame_t *out);
PIN_API void pin_preview_unlock(pin_session_t *s);

/* Stop decoding while the preview isn't visible (window minimised etc.). */
PIN_API void pin_preview_enable(pin_session_t *s, int enabled);

/* Aspect override for the preview and new captures. */
PIN_API void pin_set_aspect(pin_session_t *s, pin_aspect_t aspect);

/* Letterbox helper: the largest rectangle with display aspect dar inside a
 * w x h surface, centred. */
PIN_API void pin_fit_rect(int dar_num, int dar_den, int w, int h,
                          int *x, int *y, int *rw, int *rh);

/* Y'CbCr -> R'G'B' as a row-major 3x4 matrix (the fourth column is the
 * offset) that expects 0..1 normalised samples, for the preview shader. */
PIN_API void pin_yuv_to_rgb_matrix(pin_matrix_t m, int full_range, float out[12]);

/* ---- audio monitoring --------------------------------------------------- */

/* When enabled the core keeps a short ring of 48 kHz 16-bit stereo PCM of
 * what is being captured / previewed -- analog line-in, DV (extracted from
 * the DIF audio blocks) and HDV (the TS's MPEG-1 Layer II audio PID
 * decoded), all resampled to 48 kHz if the source wasn't already. The
 * GUI's audio output pulls from it, and can be a dumb pump: the core
 * itself bounds the ring's latency so a GUI that reads a little slowly
 * doesn't build up an ever-growing delay -- once the buffered amount
 * exceeds ~200 ms, the oldest frames are dropped down to ~80 ms before
 * the next pin_monitor_read() copies out of it. */
PIN_API void pin_monitor_enable(pin_session_t *s, int enabled);

/* Reads up to max_frames interleaved stereo frames; returns frames read. */
PIN_API int pin_monitor_read(pin_session_t *s, int16_t *out, int max_frames);

/* Non-blocking. Frames currently buffered (before the latency bound above
 * would trim them on the next read) -- lets a GUI decide how much to pull. */
PIN_API int pin_monitor_available(pin_session_t *s);

/* ---- settings ----------------------------------------------------------- */

/* A small INI in the platform's config directory, shared by all front-ends.
 * Keys are "section.key". The file is re-read on every get and written
 * atomically on every set, so concurrent windows don't lose updates. */
PIN_API pin_status_t pin_settings_get(const char *key, char *out, size_t cap);
PIN_API pin_status_t pin_settings_set(const char *key, const char *value);

/* ---- launch options / scripted actions ----------------------------------- */

typedef enum {
    PIN_ACT_NONE = 0,
    PIN_ACT_REWIND,         /* rewind to the start of the tape and wait */
    PIN_ACT_PLAY,
    PIN_ACT_STOP,
    PIN_ACT_CAPTURE,        /* capture with the launch options until it ends */
    PIN_ACT_WAIT_EOT,       /* wait until the deck stops */
} pin_action_t;

#define PIN_MAX_ACTIONS 16

typedef struct {
    uint32_t size;
    char device[PIN_NAME_MAX];  /* id, "first", or "" */
    int has_input;          pin_input_t input;
    int has_std;            pin_std_t std;
    int has_capture_opts;   pin_capture_opts_t capture;   /* only the fields given on the command line are applied over the saved settings */
    uint32_t capture_fields;    /* bitmask of which capture fields were given, see PIN_OPT_* */
    int action_count;
    pin_action_t actions[PIN_MAX_ACTIONS];
    int exit_when_done;
} pin_launch_t;

#define PIN_OPT_PATH     (1u << 0)
#define PIN_OPT_FORMAT   (1u << 1)
#define PIN_OPT_TITLE    (1u << 2)
#define PIN_OPT_SPLIT    (1u << 3)
#define PIN_OPT_PASSES   (1u << 4)
#define PIN_OPT_IDLE     (1u << 5)
#define PIN_OPT_ASPECT   (1u << 6)

/* Parses a GUI command line (UTF-8 argv, argv[0] skipped), including
 * --preset file.ini. Returns PIN_ERR_ARG with a message in err on bad
 * input. pin_launch_help() is the matching --help text. */
PIN_API pin_status_t pin_launch_parse(int argc, const char *const *argv, pin_launch_t *out,
                                      char *err, size_t err_cap);
PIN_API const char *pin_launch_help(void);

/* Runs the launch actions in order on the worker. Non-blocking; progress
 * shows up in the status and events, PIN_EVT_DONE when finished. */
PIN_API pin_status_t pin_run_actions(pin_session_t *s, const pin_launch_t *launch);

#ifdef __cplusplus
}
#endif

#endif /* PIN_API_H */
