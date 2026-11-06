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
 * marvin-core public API.
 *
 * This is the only header a front-end (the WinUI app, a future macOS or
 * Linux GUI, MarvinCaptureCLI) includes. It is a flat C ABI on purpose: opaque
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

#define PIN_API_VERSION 3

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
PIN_API const char *pin_version_string(void);    /* "marvin-core 1.0" */
PIN_API const char *pin_app_version(void);       /* "1.0": the "version" of the top-level VERSION file */
PIN_API const char *pin_strerror(pin_status_t s);

/* Optional. Where the FPGA bitstreams live. Searched in order: this dir,
 * the settings key Paths/firmware_dir, firmware/ next to the core library,
 * firmware/ next to the executable. The build puts firmware/ next to the
 * library. */
PIN_API pin_status_t pin_set_firmware_dir(const char *utf8_dir);

/* Optional; for an app whose process has none of the usual directories (Android). Call it once,
 * before anything else. Each NULL or "" keeps the platform default for that one (and resets an
 * earlier call's choice):
 *   config_dir   where settings.ini lives (default: %APPDATA%\PinnacleOSS,
 *                ~/Library/Application Support/PinnacleOSS, $XDG_CONFIG_HOME/pinnacle-oss)
 *   runtime_dir  where the cross-process lock files go (default: $XDG_RUNTIME_DIR/pinnacle-oss,
 *                $TMPDIR/pinnacle-oss or /tmp/pinnacle-oss-<uid>; unused on Windows). On Android
 *                none of these exists: pass the app's cache directory, or pin_open() fails.
 *   temp_dir     where a format that needs a temporary file would write it (default: next to
 *                the output file; Android's media provider refuses a .tmp file in Movies/). No
 *                format needs one at present: HDV to MOV / MKV is written live.
 * Directories are created if missing. PIN_ERR_ARG (and nothing changed) if a path is longer
 * than 511 bytes. */
PIN_API pin_status_t pin_set_app_dirs(const char *config_dir, const char *runtime_dir,
                                      const char *temp_dir);

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
    /* Appended (a caller compiled without it passes the old size and gets
     * the old layout). Hubs between the computer's root hub port and the
     * device: 0 = plugged straight into the computer, >0 = behind that many
     * hubs (it shares their bandwidth: show pin_usb_hub_hint()), -1 =
     * unknown (backend can't tell, replay device). Advisory only: some
     * mainboards wire their own ports through a built-in hub. */
    int32_t hub_depth;
} pin_device_info_t;

/* Lists devices without opening them. Cheap enough to call on every OS
 * device-change notification. Fills up to max entries and returns how many
 * exist (which may exceed max). Each out[i].size must be set by the caller.
 * Includes the virtual replay device (id "replay:<basename>") whenever the
 * PIN_REPLAY environment variable or pin_set_replay_file() names a file. */
PIN_API int pin_enumerate(pin_device_info_t *out, int max);

/* Names a USB device found some other way, without enumerating or opening it (Android:
 * UsbManager lists the devices, the core cannot): fills name, vid, pid, tested, state
 * (PIN_DEV_READY for a model the core drives, PIN_DEV_UNSUPPORTED for a sibling it
 * recognises but does not drive), hub_depth -1, and an empty id, serial and owner_pid.
 * out->size must be set. PIN_ERR_NOT_FOUND for a vid / pid that is not in the model table. */
PIN_API pin_status_t pin_usb_model(uint16_t vid, uint16_t pid, pin_device_info_t *out);

/* One short paragraph for a device with hub_depth > 0: it shares the hub's
 * bandwidth, plug it straight into the computer, and the platform's tool to
 * check (USBTreeView / lsusb -t / System Information). Static, never NULL. */
PIN_API const char *pin_usb_hub_hint(void);

/* Performance recommendations about the computer itself (power saving that makes analog capture
 * lose frames), next to the per-device hub hint above. pin_perf_check() returns a bitmask of the
 * PIN_PERF_* items that apply right now (0 = nothing to recommend); pin_perf_text() is the
 * message for one bit, and pin_perf_fix() the shell commands that fix it (one per line, "" if
 * the front end should open the OS settings instead). Static strings, never NULL. */
#define PIN_PERF_CPU_LATENCY 0x1   /* Linux: /dev/cpu_dma_latency is not writable (never on Android,
                                      where nothing can be done about it) */
#define PIN_PERF_POWER_PLAN  0x2   /* Windows: power plan is Power saver or Balanced */
PIN_API int pin_perf_check(void);
PIN_API const char *pin_perf_text(int kind);
PIN_API const char *pin_perf_fix(int kind);

/* Process-wide: whether a capture keeps one CPU core busy at idle priority, so the CPUs stay
 * out of the deep idle states whose wake-up latency makes analog captures lose frames
 * (docs/analog.md, "Power saving and capture reliability"). It runs next to the USB read loop
 * and costs one core's worth of power and heat. Takes effect when the next read loop starts.
 *   AUTO  (default) Linux: only when the /dev/cpu_dma_latency request (the better remedy) is
 *         refused; Android: always, since an app cannot make that request
 *   ON    always (Linux, Android), even when the latency request was granted
 *   OFF   never
 * Windows and macOS: no effect. Other values are ignored. */
typedef enum {
    PIN_KEEP_BUSY_AUTO = 0,
    PIN_KEEP_BUSY_ON,
    PIN_KEEP_BUSY_OFF,
} pin_keep_busy_t;
PIN_API void pin_set_cpu_keep_busy(int mode);

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
    PIN_STATE_STOPPING,     /* finalising files (may take a moment) */
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
 * pin_set_replay_file() with it first and then opening it by name.
 *
 * "fd:<n>" opens the device behind file descriptor n, a USB device the caller
 * opened itself: on Android UsbDeviceConnection.getFileDescriptor() (an app
 * cannot enumerate USB devices there), in Termux the descriptor termux-usb
 * passes. Nothing is enumerated; the model comes from the device. The caller
 * keeps the descriptor (and the UsbDeviceConnection) open until pin_close()
 * returns; the core never closes it. The device has no port id (hub_depth -1).
 * PIN_ERR_ARG for a malformed "fd:" id, a descriptor that is not open, or a
 * platform that cannot do this (Linux / Android only); a descriptor that is no
 * supported USB device fails at pin_set_input(). The MovieBox Deluxe cannot be
 * opened this way yet (see pin_device_fd_problem()). */
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
    PIN_FMT_ANALOG_FFV1_MKV,    /* FFV1 level 3 + FLAC in Matroska */
    PIN_FMT_DV_RAW,             /* .dv */
    PIN_FMT_DV_AVI,             /* DV type-2 AVI */
    PIN_FMT_DV_MOV,             /* DV in QuickTime */
    PIN_FMT_HDV_TS,             /* .m2t (default) or .ts */
    PIN_FMT_HDV_MOV,
    PIN_FMT_HDV_MKV,
    /* Appended. Lossy, with the operating system's own (normally hardware) encoder: Android
     * MediaCodec, macOS VideoToolbox, Windows Media Foundation; video_quality and deinterlace
     * apply (pin_format_options()). FLAC audio, Matroska, written live (a crash leaves a
     * playable file). Listed by pin_formats() only where the encoder opens on this machine;
     * never on Linux. DV input is decoded first (its scene split and passes work as usual). */
    PIN_FMT_ANALOG_H264_MKV,
    PIN_FMT_ANALOG_HEVC_MKV,
    PIN_FMT_DV_H264_MKV,
    PIN_FMT_DV_HEVC_MKV,
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
    int supports_title;         /* the file stores pin_capture_opts_t.title (raw .dv and .m2t / .ts
                                   do not; the capture ignores it there) */
    int supports_scene_split;
    int supports_multi_pass;
    int is_default;             /* the default format for its kind */
    /* Appended. A caller compiled before it sets size (of out[0] for pin_formats()) to its own
     * sizeof and gets that layout. 1 if the file stores a display aspect: pin_capture_opts_t's
     * aspect (analog, DV) or aspect_hdv (HDV) then applies. 0 for raw DV (.dv) and the raw
     * transport stream (.m2t / .ts), which carry no aspect flag of their own: players take the
     * aspect from the video itself (DV's 16:9 flag, the MPEG-2 sequence header), so a GUI
     * disables its aspect control for them (pin_format_aspect_help() says why). */
    int supports_aspect;
    /* Appended. 1 if the format always splits into scenes, whatever pin_capture_opts_t's
     * scene_split says: HDV to MOV / MKV (its files take the stream's timing apart and must not
     * run across another recording, see docs/hdv.md). A GUI shows its scene split option
     * checked and disabled for it; pin_format_scene_split_help() says why. */
    int scene_split_forced;
} pin_format_info_t;

/* Formats for a kind, in display order. Returns the count. out[0].size sets the size of every
 * entry (see supports_aspect). Only the formats this platform offers: the H.264 / HEVC formats
 * appear only where their encoder opened in pin_encoders_probe() (which the first call runs if
 * nobody did: see there). pin_format_info() answers for every format.
 *
 * PIN_FMT_HDV_MOV / PIN_FMT_HDV_MKV are written live: every GOP is decoded to check it;
 * undamaged GOPs are copied, damaged ones (tape dropouts: a packet lost or flagged, or anything
 * the decoder had to conceal) are encoded again from the decoded, concealed pictures as MPEG-2
 * of the same size and bit rate; the files' timestamps start at 0 and run without gaps, and
 * they always split into scenes (scene_split_forced). If the machine cannot keep up with the
 * checking or the repairs, the capture ends with an error. See docs/hdv.md ("MOV and MKV"). */
PIN_API int pin_formats(pin_kind_t kind, pin_format_info_t *out, int max);
PIN_API pin_status_t pin_format_info(pin_format_t f, pin_format_info_t *out);

/* Finds out once per process which of the H.264 / HEVC formats this machine can write: opens
 * each encoder with a small test configuration (720 x 576, 25 fps) and closes it again, on a
 * thread of its own (Media Foundation must not run on a GUI's single-threaded COM apartment).
 * Blocking: typically 50-300 ms per encoder, up to about a second on a phone that needs every
 * fallback (pixel format, profile), and nothing at all on Linux. The result is cached, so later
 * calls (and pin_formats()) return at once. A GUI calls it from a worker thread at start-up,
 * before it first lists the formats; otherwise the first pin_formats() pays for it. Returns a
 * bitmask of PIN_ENC_* for the encoders that opened. */
#define PIN_ENC_H264 0x1
#define PIN_ENC_HEVC 0x2
PIN_API int pin_encoders_probe(void);

/* A one-paragraph warning to show when the user picks format f on this platform, or "" when
 * there is none: the H.264 / HEVC formats (lossy; for DV input a re-encode of the DV, which is
 * already compressed), and on Android uncompressed analog AVI, whose data rate phone storage
 * may not sustain. "" for every other format. 512 bytes are enough. */
PIN_API void pin_format_warning(pin_format_t f, char *out, size_t cap);

/* Help texts (tooltips) for a GUI's "name and title" field and its aspect drop-downs, naming the
 * formats they apply to; a GUI shows them and asks again when a format selection changes.
 * Cheap, never runs the encoder probe. 512 bytes are enough.
 *
 * pin_format_title_help(): the name field, which is the files' name and also their title.
 * input PIN_INPUT_DV: the DV input's field, which DV and HDV share; format is the DV format,
 * format_hdv the HDV format. An analog input: the analog field; format is the analog format
 * (format_hdv is ignored). Names the formats that store the title and those that have none:
 * "File name for DV and HDV (scenes and passes get numbered suffixes). Also stored as the title
 * in DV AVI files; raw .m2t (HDV) files have no title." A format that is not of the kind
 * expected there (PIN_FMT_COUNT, say, while a GUI's list is still empty) is left out.
 *
 * pin_format_aspect_help(): the aspect drop-down of format f's kind. If f stores an aspect
 * (pin_format_info_t.supports_aspect): what the choice does, for which files. If not: why the
 * drop-down is disabled (the file has no aspect flag; players take it from the video) and which
 * formats of that kind offered here store one. "" for an unknown format. */
PIN_API void pin_format_title_help(pin_input_t input, pin_format_t format, pin_format_t format_hdv,
                                   char *out, size_t cap);
PIN_API void pin_format_aspect_help(pin_format_t f, char *out, size_t cap);

/* The help text (tooltip) of a GUI's "split into scenes" option for format f: what it does, or,
 * for a format that always splits (pin_format_info_t.scene_split_forced), why it is checked and
 * disabled; for a format without scene split, that its files are not split. "" for an unknown
 * format. 512 bytes are enough. */
PIN_API void pin_format_scene_split_help(pin_format_t f, char *out, size_t cap);

/* The help text (tooltip) of a GUI's format drop-down for a kind, whatever is selected in it, or
 * "" for none. HDV: that MOV and MKV are not truly lossless (damaged GOPs are re-encoded with the
 * damage concealed, which editors take better, but the damage can no longer be seen as such)
 * and that the raw .m2t is. 1024 bytes are enough. */
PIN_API void pin_format_kind_help(pin_kind_t kind, char *out, size_t cap);

/* The format settings of pin_capture_opts_t and which format each applies to. */
typedef enum {
    PIN_DEINTERLACE_KEEP_FIELDS = 0,    /* each interlaced frame is encoded as one progressive picture */
    PIN_DEINTERLACE_DOUBLE_RATE = 1,    /* bwdif: every field becomes a frame, 25i -> 50p, 29.97i -> 59.94p */
} pin_deinterlace_t;

typedef enum {
    PIN_FFV1_FAST = 0,          /* Golomb-Rice coder (FFmpeg's default): least CPU, largest files */
    PIN_FFV1_SMALLER,           /* range coder: a few percent smaller, more CPU */
    PIN_FFV1_SMALLEST,          /* range coder and the large context model: smallest, most CPU */
    PIN_FFV1_LEVEL_COUNT
} pin_ffv1_compression_t;

#define PIN_OPT_HELP_MAX 512

/* What pin_format_options() reports: which of the format settings apply to a format and how a
 * GUI presents them, with ready-made labels and help texts (UTF-8, English, like the other
 * ready-made texts). A GUI shows one control per has_* flag and nothing else. */
typedef struct {
    uint32_t size;
    int has_quality;            /* video_quality applies (H.264 / HEVC) */
    int has_deinterlace;        /* deinterlace applies (H.264 / HEVC) */
    int has_ffv1_compression;   /* ffv1_compression applies (FFV1) */

    /* video_quality: a slider or spin box from quality_min to quality_max in steps of 1,
     * quality_default preselected. Lower is better (bigger files), like x264's CRF; the encoder
     * gets the closest it supports (constant quality or a bit rate derived from the value). */
    int quality_min, quality_max, quality_default;
    int quality_lower_is_better;            /* 1 */
    char quality_label[PIN_NAME_MAX];       /* "Quality" */
    char quality_low_label[PIN_NAME_MAX];   /* the quality_min end of the slider: "Best" */
    char quality_high_label[PIN_NAME_MAX];  /* the quality_max end: "Smallest" */
    char quality_help[PIN_OPT_HELP_MAX];    /* what the number means, the default, lossy */

    /* deinterlace: two choices (radio buttons or a drop-down), in pin_deinterlace_t order. */
    int deinterlace_default;                        /* PIN_DEINTERLACE_DOUBLE_RATE */
    char deinterlace_label[PIN_NAME_MAX];           /* "Interlaced video" */
    char deinterlace_keep_label[PIN_NAME_MAX];      /* "Keep fields (25/30 fps)" */
    char deinterlace_double_label[PIN_NAME_MAX];    /* "Deinterlace (50/60 fps)" */
    char deinterlace_keep_help[PIN_OPT_HELP_MAX];   /* the warning: interlaced material loses quality */
    char deinterlace_double_help[PIN_OPT_HELP_MAX];

    /* ffv1_compression: PIN_FFV1_LEVEL_COUNT choices in pin_ffv1_compression_t order. */
    int ffv1_default;                               /* PIN_FFV1_FAST */
    char ffv1_label[PIN_NAME_MAX];                  /* "Compression" */
    char ffv1_fast_label[PIN_NAME_MAX];             /* "Fast" */
    char ffv1_smaller_label[PIN_NAME_MAX];          /* "Smaller" */
    char ffv1_smallest_label[PIN_NAME_MAX];         /* "Smallest" */
    char ffv1_help[PIN_OPT_HELP_MAX];               /* smaller files cost more CPU; all lossless */

    char note[PIN_OPT_HELP_MAX];    /* one line for the whole settings panel of this format, or "":
                                       H.264 / HEVC: lossy, made by the hardware encoder */
} pin_format_options_t;

/* Fills *out (out->size set by the caller) for format f. Settings that do not apply have their
 * has_* flag 0 (the texts and ranges are filled all the same). PIN_ERR_ARG for an unknown format,
 * PIN_ERR_ABI for a size the library does not know. Pure, cheap. */
PIN_API pin_status_t pin_format_options(pin_format_t f, pin_format_options_t *out);

/* The data rate one quality setting comes to, for a line under the quality control: "About 2.9
 * GB per hour (6.4 Mbit/s)". f is an H.264 / HEVC format (else ""), is_60hz the video standard
 * (0 = PAL / SECAM 25 fps, 1 = NTSC / PAL-M 29.97 fps), deinterlace a pin_deinterlace_t. An
 * estimate from the same mapping the encoders' bit rates come from (pin_status_snapshot_t's
 * est_bytes_per_hour before a capture has measured its own rate). 128 bytes are enough. */
PIN_API void pin_format_quality_hint(pin_format_t f, int video_quality, int deinterlace,
                                     int is_60hz, char *out, size_t cap);

/* ---- capture ------------------------------------------------------------ */

typedef enum {
    PIN_ASPECT_AUTO = 0,    /* DV: from the stream; HDV: 16:9; analog: 4:3 */
    PIN_ASPECT_4_3,
    PIN_ASPECT_16_9,
} pin_aspect_t;

typedef struct {
    uint32_t size;
    char path[PIN_PATH_MAX];    /* base output path; extension optional. "-" = standard output:
                                   DV raw DIF, HDV MPEG-TS, analog NUT (rawvideo YUY2 + PCM s16le);
                                   the format_* fields, scene_split, keep_raw and passes > 1 are
                                   ignored, naming, collision and disk checks do not apply, and the
                                   capture ends with PIN_STOP_PIPE_SLOW / PIN_STOP_PIPE_CLOSED if the
                                   reader cannot keep up / exits. */
    pin_format_t format_analog; /* used when the input is analog */
    pin_format_t format_dv;     /* used when the stream turns out to be DV */
    pin_format_t format_hdv;    /* ... or HDV */
    char title[PIN_TEXT_MAX];   /* metadata title, in every file whose format stores one
                                   (pin_format_info_t.supports_title); ignored by the others, so
                                   a tape that switches between DV and HDV gets it where it fits */
    pin_aspect_t aspect;        /* display aspect of analog and DV files (aspect_hdv: HDV files);
                                   ignored by formats that store none (supports_aspect) */
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
                                    0 = never. DV/HDV: per pass, counting only the capture time
                                    of the current pass (not the rewind between passes); when it
                                    expires the pass ends like a no-signal timeout (rewind and
                                    next pass, or end of the capture in the last pass).
                                    Appended field, ABI-compatible. */
    char ts_extension[8];       /* extension of a raw HDV transport stream file (PIN_FMT_HDV_TS),
                                    without the dot: "" = "m2t", or "ts" to keep the name the user
                                    gave. Appended field, ABI-compatible. */
    /* Appended fields, ABI-compatible: the format settings (pin_format_options() says which
     * apply to which format; the others are ignored). A caller compiled before them passes the
     * old size and gets the defaults. */
    int deinterlace;            /* H.264 / HEVC: a pin_deinterlace_t (default DOUBLE_RATE) */
    int video_quality;          /* H.264 / HEVC: 0 (best) .. 51 (smallest), like a CRF (default 18) */
    int ffv1_compression;       /* PIN_FMT_ANALOG_FFV1_MKV: a pin_ffv1_compression_t (default FAST) */
    /* Appended field, ABI-compatible: the display aspect of HDV files (default AUTO), as `aspect`
     * is that of DV files, so each file of a tape that switches between DV and HDV gets its own
     * kind's, whichever kind the capture started on. A caller compiled before it passes the old
     * size and gets aspect_hdv = aspect. */
    pin_aspect_t aspect_hdv;
} pin_capture_opts_t;

/* Fills *o with the defaults. A caller compiled against an older pin_api.h sets o->size to its
 * sizeof() first; only that many bytes are written then. Any other size (0 included) means
 * the full struct of this header. */
PIN_API void pin_capture_opts_defaults(pin_capture_opts_t *o);

/* Multi-pass needs a way to notice the end of a pass: the no-signal timeout
 * (idle_stop_minutes > 0) or the per-pass time limit (max_duration_minutes > 0).
 * Without either, a second pass could never start, so more than one pass is not
 * allowed. Returns 1 if passes > 1 makes sense with these limits, else 0. GUIs
 * use it to disable their "passes" entry. */
PIN_API int pin_capture_passes_allowed(int idle_stop_minutes, int max_duration_minutes);

/* Makes opts consistent: passes < 1 becomes 1 and, when pin_capture_passes_allowed()
 * says no, passes is forced to 1. Returns 1 if passes was changed. Called by
 * pin_capture_start() (not for replay sessions, whose end-of-file is the end of
 * the pass), so command-line presets and other front ends are covered too. */
PIN_API int pin_capture_opts_normalize(pin_capture_opts_t *o);

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
 * capture. input tells it which format/data rate applies. PIN_ERR_CODEC, with the
 * reason in message, when the format for the current input is an H.264 / HEVC one whose
 * encoder this machine does not have (pin_capture_start() refuses it the same way). */
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
 * overwrite is set (the GUI asks the user first), and with PIN_ERR_CODEC when the
 * format for the current input needs an encoder this machine does not have (see
 * pin_check_output() for the message). An encoder that fails later, while capturing,
 * ends the capture like a write error (PIN_STOP_ERROR, "the video encoder failed"). */
PIN_API pin_status_t pin_capture_start(pin_session_t *s, const pin_capture_opts_t *o, int overwrite);

/* Non-blocking. State goes STOPPING -> READY. Also stops the deck if the
 * capture started it (start_deck). */
PIN_API pin_status_t pin_capture_stop(pin_session_t *s);

typedef enum {
    PIN_STOP_DECK_AS_STARTED = 0, /* send deck Stop only if the capture was started with start_deck */
    PIN_STOP_DECK_NO = 1,         /* never: "stop capture & continue tape" */
    PIN_STOP_DECK_YES = 2         /* always, even for a manual capture: "stop capture & stop tape" */
} pin_stop_deck_t;

/* Like pin_capture_stop(), but the caller decides whether the deck gets a Stop
 * too, regardless of how the capture was started. Needs a camera on the bus
 * for YES (silently nothing without one). */
PIN_API pin_status_t pin_capture_stop_ex(pin_session_t *s, pin_stop_deck_t stop_deck);

/* ---- status --------------------------------------------------------------- */

/* Why a capture ended (pin_status_snapshot_t.stop_reason, PIN_EVT_CAPTURE_ENDED).
 * pin_stop_reason_abnormal() splits them: an abnormal end (something failed or
 * was disconnected) deserves a dialog; a normal one (the user, a limit that
 * was set, the end of the tape) a line in the status bar. */
typedef enum {
    PIN_STOP_NONE = 0,          /* no capture has ended in this session yet */
    PIN_STOP_USER,              /* pin_capture_stop[_ex], deck Stop, input switch, close */
    PIN_STOP_NO_SIGNAL,         /* idle_stop_minutes without data / signal, in the last pass */
    PIN_STOP_TIME_LIMIT,        /* max_duration_minutes reached, in the last pass */
    PIN_STOP_END_OF_TAPE,       /* replay device: the end of the source file, in the last pass */
    PIN_STOP_DEVICE_LOST,       /* the capture device stopped answering on USB (unplugged);
                                   the session goes to PIN_STATE_ERROR */
    PIN_STOP_CAMERA_LOST,       /* DV / HDV: the FireWire bus was reset during the capture (cable
                                   moved, camera switched off): frames were lost, so it stops */
    PIN_STOP_DISK_FULL,         /* the output drive got too full to go on; stopped while there
                                   was still room to finish the file */
    PIN_STOP_WRITE_ERROR,       /* writing or opening an output file failed */
    PIN_STOP_ERROR,             /* any other error (see last_error / error_text) */
    PIN_STOP_PIPE_SLOW,         /* output to stdout ("-"): the program reading it could not keep up
                                   and the queue in front of the pipe filled. Appended. */
    PIN_STOP_PIPE_CLOSED,       /* output to stdout ("-"): the reading program exited. Appended. */
    PIN_STOP_DISK_SLOW,         /* the output drive could not keep up and the queue in front of
                                   it filled: the file ends before the first lost unit. Appended. */
} pin_stop_reason_t;

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
     * duration_remaining_s: the capture time limit of the current pass
     *   (max_duration_minutes), restarting with each pass. */
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
     *           damaged I / P picture of the same GOP. Into HDV MOV / MKV
     *           instead the files' own count: every frame re-encoded (a
     *           damaged GOP, decoded with concealment and encoded again,
     *           including frames that repeat a neighbour for a lost picture)
     *           or played with its audio replaced by silence; it lags the
     *           picture count by a GOP or a repair;
     *   analog: a repeated (dropped) frame.
     * frames_dropped: DV frames with >= 90 % of their blocks missing, analog
     * repeated frames; HDV MOV / MKV frames that repeat a neighbour because
     * their picture was lost (also "with error"); raw HDV 0.
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

    /* Appended fields, ABI-compatible. Video format of what is arriving, for
     * every kind (width / height above): the frame rate as a fraction
     * (analog and DV: 25/1 or 30000/1001; HDV: from the stream's sequence
     * header, 0/0 until the first one is seen), whether it is interlaced
     * (analog and DV always; HDV as its sequence extension says, so 720p and
     * 1080p are progressive), and video_label, a short ready-made name so
     * every GUI shows the same: "PAL", "NTSC", "1080i25", "1080i29.97",
     * "720p50", "720p59.94" (HDV 720p is untested, no hardware). Empty while
     * there is no signal or the format is not known yet. */
    int video_fps_num, video_fps_den;
    int video_interlaced;
    char video_label[16];

    /* Appended fields, ABI-compatible. How the last capture of this session
     * ended (PIN_STOP_NONE until one has; cleared when a capture starts):
     * stop_captured_s is the length of what was captured (all passes, from
     * the frame count), stop_text the same ready-made sentence as the
     * PIN_EVT_CAPTURE_ENDED event. */
    pin_stop_reason_t stop_reason;
    double stop_captured_s;
    char stop_text[PIN_TEXT_MAX];

    /* Appended. 1 if the last capture ended without a single video frame having been
     * written (e.g. an empty tape): whatever the reason, no output file was left
     * behind (a file is created by its first frame, never before). When the end was
     * not abnormal (pin_stop_reason_abnormal), stop_text then says so instead of
     * "Capture stopped after capturing 0s ..." and PIN_EVT_NO_VIDEO was sent. Cleared
     * when a capture starts. */
    int stop_no_video;
} pin_status_snapshot_t;

/* Non-blocking. */
PIN_API pin_status_t pin_get_status(pin_session_t *s, pin_status_snapshot_t *out);

/* Ready-made text so every GUI shows the same thing. */
PIN_API void pin_format_status_line(const pin_status_snapshot_t *st, char *out, size_t cap);
/* Seconds as "5m30s" / "1h02m10s" / "45s" (rounded up), for the stop countdowns
 * (idle_stop_remaining_s, duration_remaining_s). */
PIN_API void pin_format_remaining(double seconds, char *out, size_t cap);
/* 1 if a capture that ended this way failed or lost something (device or
 * camera gone, disk full, write error, any error): show a dialog. 0 for the
 * user's own stop and for the limits and end of tape the user asked for. */
PIN_API int pin_stop_reason_abnormal(pin_stop_reason_t reason);

/* What a taskbar / dock progress indicator should show for a status snapshot,
 * so every GUI picks the same mode. idle_total_s / duration_total_s are the
 * running capture's no-signal timeout and per-pass time limit in seconds (0 =
 * off; the snapshot only carries what is left). *fraction (0..1, may be NULL)
 * is set for PIN_PROGRESS_NORMAL / PAUSED / ERROR, else 0.
 *   ERROR          session in the error state
 *   PREPARING      NORMAL with progress_percent, or INDETERMINATE if unmeasurable
 *   CAPTURING      no signal with a timeout: NORMAL, remaining / timeout (falls);
 *                  else a per-pass limit: NORMAL, elapsed / limit (rises);
 *                  else no signal: PAUSED (waiting for the signal);
 *                  else INDETERMINATE (running, end unknown)
 *                  disk_low (while writing, not an error): PAUSED (yellow) instead, with
 *                  the value of a bar mode kept (countdown / limit), else full
 *   REWINDING      PAUSED (between passes)
 *   STOPPING       INDETERMINATE
 *   otherwise      NONE */
typedef enum {
    PIN_PROGRESS_NONE = 0,
    PIN_PROGRESS_INDETERMINATE,
    PIN_PROGRESS_NORMAL,
    PIN_PROGRESS_PAUSED,
    PIN_PROGRESS_ERROR
} pin_progress_mode_t;
PIN_API pin_progress_mode_t pin_status_progress(const pin_status_snapshot_t *st, double idle_total_s,
                                                double duration_total_s, double *fraction);
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
    PIN_EVT_DONE,           /* pin_script_run finished: a = exit code (0 ok, see pin_script_run), text = reason */
    PIN_EVT_DEVICES,        /* device list may have changed (this process's own view) */
    PIN_EVT_CAPTURE_ENDED,  /* a capture ended (after its files were closed): a = pin_stop_reason_t,
                               text = "Capture stopped after capturing 12m30s, because ..." */
    PIN_EVT_STEP,           /* pin_script_run: a step starts: a = step index, text = its description */
    PIN_EVT_NO_VIDEO,       /* a capture ended that was not abnormal but received no video at all (sent
                               right after PIN_EVT_CAPTURE_ENDED): a = pin_stop_reason_t, text =
                               "No video received; nothing was captured to PATH." No file was
                               left. Appended. */
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
    double present_time;        /* pin_clock_now() time to show it at (jitter buffer) */
} pin_frame_t;

/* The preview decodes on its own low-priority thread and never slows a
 * capture: if the renderer falls behind, frames are skipped.
 *
 * Pacing: each decoded frame gets a present_time on a smoothed grid at the
 * source frame rate, a few ms (more for a jittery source) after it was
 * decoded, so USB / decode jitter doesn't reach the screen. A renderer
 * that cares calls pin_preview_lock_due() once per display refresh with
 * the time of the refresh it just saw, and presents whatever it gets; a
 * frame then appears on the first refresh at or after its present_time.
 * Audio monitoring (pin_monitor_read()) is held back by the same delay. */

/* Blocks up to timeout_ms for a frame newer than after_seq. Returns 1 if
 * one is available, 0 on timeout, -1 if the session is closing. */
PIN_API int pin_preview_wait(pin_session_t *s, uint64_t after_seq, int timeout_ms);

/* Borrows the newest frame. Planes stay valid until pin_preview_unlock();
 * hold them only long enough to upload. Returns PIN_ERR_STATE if none. */
PIN_API pin_status_t pin_preview_lock(pin_session_t *s, pin_frame_t *out);
PIN_API void pin_preview_unlock(pin_session_t *s);

/* Like pin_preview_lock(), but borrows the newest frame whose present_time
 * is <= now, dropping older ones. PIN_ERR_STATE if none is due yet. */
PIN_API pin_status_t pin_preview_lock_due(pin_session_t *s, double now, pin_frame_t *out);

/* The earliest present_time of the frames waiting to be shown; returns 0
 * (and leaves *t alone) if there are none. Lets a renderer sleep instead of
 * waking every refresh. */
PIN_API int pin_preview_next_time(pin_session_t *s, double *t);

/* Seconds on the monotonic clock present_time is on. On Windows it is
 * QueryPerformanceCounter, so DXGI / DWM timestamps compare directly. On
 * macOS it is mach_absolute_time (CLOCK_UPTIME_RAW, which does not count
 * system sleep), the clock of CACurrentMediaTime() and CADisplayLink /
 * CVDisplayLink timestamps. Elsewhere CLOCK_MONOTONIC. */
PIN_API double pin_clock_now(void);

/* Stop decoding while the preview isn't visible (window minimised etc.). */
PIN_API void pin_preview_enable(pin_session_t *s, int enabled);

/* Aspect override for the preview of whatever arrives (and what an analog capture's AUTO aspect
 * resolves to). The files take theirs from pin_capture_opts_t (aspect, aspect_hdv). */
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
 * the next pin_monitor_read() copies out of it. The newest few ms (the
 * preview's jitter delay, see pin_preview_lock_due()) are held back on
 * top of that, so sound and picture stay in step. */
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

/* Per-device settings: builds the key "dev_<ID>.<key>" for a unit so each
 * device keeps its own independent set (GUI options, output paths, ...).
 * <ID> is the unit's GUID (pin_device_info_t.serial) when known, else the
 * device id (sanitised; only stable while plugged into the same port). Pass
 * the serial and id fields of pin_device_info_t; either may be "" but not both.
 * key NULL/"" returns just the prefix "dev_<ID>." to append keys to. Use the
 * result with pin_settings_get/set. App-global settings (window geometry, last
 * used device) keep their plain keys. */
PIN_API pin_status_t pin_device_settings_key(const char *serial, const char *id, const char *key,
                                            char *out, size_t cap);

/* ---- command line: settings + actions (docs/cli.md) --------------------------- */

/* The command-line language shared by every front end (MarvinCaptureCLI, the
 * GUI): settings and actions, processed left to right, e.g.
 *     --rew --wait --play --capture tape01.avi --wait --rew --wait
 * The core parses it (pin_script_parse), checks it as far as it can without a
 * device (unknown options, bad values, malformed timecodes, transport actions
 * on an analog input, ...), and runs it as a sequence of steps on a helper
 * thread (pin_script_run). A "step" is one action: --rew --ff --play --pause
 * --stop --capture PATH --wait-any|--wait-all|--wait CONDS. Settings (--input, --std, --format,
 * ...) are not steps; they change what the steps after them do. */

typedef struct pin_script pin_script_t;

/* Parses argv (UTF-8, WITHOUT the program name). On success *out is a script to
 * free with pin_script_free(). A usage error returns PIN_ERR_ARG with a one-line
 * message naming the offending argument in err. -h / --help anywhere, or no
 * arguments at all, give a script with pin_script_help_requested() set (the
 * rest of the line is not looked at then). */
PIN_API pin_status_t pin_script_parse(int argc, const char *const *argv, pin_script_t **out,
                                      char *err, size_t err_cap);
PIN_API void pin_script_free(pin_script_t *sc);   /* safe with NULL; a running script has its own copy */

/* The full help text (docs/cli.md), static storage. */
PIN_API const char *pin_script_help(void);

PIN_API int pin_script_help_requested(const pin_script_t *sc);
/* -d / --device value (an id, a serial, "fd:<n>", or a file to replay; pass it to
 * pin_open()), or NULL when none was given (= the first device). */
PIN_API const char *pin_script_device(const pin_script_t *sc);
/* --debug was given: the front end sets the log level to 0 and prints the status
 * as one plain line per second. */
PIN_API int pin_script_debug(const pin_script_t *sc);
/* --exit-when-done was given: a GUI closes itself when the script ends (PIN_EVT_DONE),
 * with the exit code from the event. The CLI always exits and ignores it. */
PIN_API int pin_script_exit_when_done(const pin_script_t *sc);
/* Number of steps (actions). */
PIN_API int pin_script_step_count(const pin_script_t *sc);
/* Printable description of step i, e.g. "rew", "wait-any idle=00:01:00,nosignal=00:01:00",
 * "capture tape01.avi". PIN_ERR_ARG for an index out of range. */
PIN_API pin_status_t pin_script_step_text(const pin_script_t *sc, int index, char *out, size_t cap);
/* 1 if running the script needs an open session (it has at least one step). */
PIN_API int pin_script_needs_session(const pin_script_t *sc);

/* The settings given before the first action (all of them if the script has no
 * actions), for a front end that shows them in its own controls. Set size to
 * sizeof() before the call. Controls are in the units of pin_set_control(). */
typedef struct {
    uint32_t size;
    int has_input;          pin_input_t input;
    int has_std;            pin_std_t std;
    int has_format_analog;  pin_format_t format_analog;
    int has_format_dv;      pin_format_t format_dv;
    int has_format_hdv;     pin_format_t format_hdv;
    int has_aspect;         pin_aspect_t aspect;
    int has_split;          int split;
    int has_title;          char title[PIN_TEXT_MAX];
    int has_keep_raw;       int keep_raw;
    uint32_t controls_set;  /* bit (1 << pin_control_t) per control that was given */
    int32_t control_brightness, control_contrast, control_saturation;
    int32_t control_hue, control_sharpness, control_audio_gain;
    /* Appended: --deinterlace, --quality, --ffv1-compression (pin_capture_opts_t's fields). A
     * caller compiled before them passes the old size and gets the old layout. */
    int has_deinterlace;    int deinterlace;
    int has_video_quality;  int video_quality;
    int has_ffv1_compression; int ffv1_compression;
} pin_script_settings_t;

PIN_API pin_status_t pin_script_settings(const pin_script_t *sc, pin_script_settings_t *out);

/* Runs the script on a helper thread (non-blocking; the script is copied). The
 * session must be open: the runner applies the input (pin_set_input, if the
 * script names one or the session was never brought up; then waits for READY),
 * the analog standard and controls, and performs the steps in order.
 *   - PIN_EVT_STEP (a = step index, text = pin_script_step_text) as each step starts.
 *   - PIN_EVT_DONE at the end: a = the exit code (0 ok, 1 usage-type error found
 *     at run time, e.g. an existing file without --overwrite or --capture - which is
 *     not supported yet, 2 device or bring-up error, 3 deck error, 4 a capture ended
 *     abnormally, 5 a capture got no video at all (no file was left; the script
 *     still ran to its end, any more severe code wins), 130 cancelled);
 *     text = a one-line reason when a != 0.
 * Capture: a manual capture with the current settings; it is closed (and
 * finalised) by the next transport action, the next --capture or the end of the
 * script. Returns PIN_ERR_STATE if a script is already running in this session. */
PIN_API pin_status_t pin_script_run(pin_session_t *s, const pin_script_t *sc);

/* Ctrl-C: finalises an open capture, stops the tape if the script moved it, and
 * finishes with PIN_EVT_DONE a = 130. Non-blocking. PIN_ERR_STATE if no script is
 * running. */
PIN_API pin_status_t pin_script_cancel(pin_session_t *s);

/* ---- ready-made texts and enable rules (the same in every GUI) ---------------------
 *
 * Everything below is logic a front end would otherwise have to copy: the strings
 * (UTF-8, English; "·" is U+00B7, "…" is U+2026) and which buttons are enabled when.
 * A front end keeps only widgets, layout and bindings. Text functions write a
 * NUL-terminated string into out (truncated to cap); 512 bytes hold any of
 * them, including the multi-line ones, except pin_format_status_short(), which can carry a
 * file name (PIN_PATH_MAX + 128). Numbers use '.' as the
 * decimal separator and ',' as the thousands separator whatever the system locale. */

/* "123 B", "1.5 GB": B KB MB GB TB in 1024 steps, one decimal above bytes. */
PIN_API void pin_format_bytes(uint64_t bytes, char *out, size_t cap);

/* "2 h 05 min left" / "45 min left": whole minutes, truncated, never negative. */
PIN_API void pin_format_time_left(double seconds, char *out, size_t cap);

/* The number to put in pin_capture_opts_t.first_number for a capture to `path` (the
 * output directory + base name, the extension optional): one higher than the highest
 * "<name>-<digits>.<ext>" file already in that directory (any extension, name matched
 * case-insensitively), so no format's earlier takes are overwritten. A trailing ".ext" of
 * the name is dropped if it is the extension of any output format. Returns 1 if the
 * directory doesn't exist or can't be read, or the name is empty. An empty directory
 * part means ".". */
PIN_API uint32_t pin_next_file_number(const char *path);

/* Deck state as shown in the status bar: "Stopped", "Playing", "Paused", "Fast forward",
 * "Rewinding", "Camera recording", "No tape", else "—" (an em dash). Static, never NULL. */
PIN_API const char *pin_deck_state_name(pin_deck_state_t s);

/* Tooltip of the deck status: "Deck: Playing", or while the deck has a command pending
 * (busy != 0; status snapshot deck_busy) "Deck: Playing — waiting for the deck to respond"
 * (em dash). 80 bytes are enough. */
PIN_API void pin_format_deck_tip(pin_deck_state_t s, int busy, char *out, size_t cap);

/* Whether the deck button for cmd is enabled. Inputs are what the status snapshot
 * has (state, deck; deck_available = the input is DV/HDV with camera_present != 0,
 * i.e. false only when no camera answered on the bus). All commands need state READY
 * (never while capturing or finalising), a camera, and a deck that has a tape; then each
 * is disabled when the deck is already doing what it asks: PLAY while playing or
 * recording, PAUSE while paused, STOP while stopped, FF while fast-forwarding, REW
 * while rewinding. Cheap: call it per button per status tick. */
PIN_API int pin_deck_cmd_allowed(pin_state_t state, int deck_available, pin_deck_state_t deck,
                                 pin_deck_cmd_t cmd);

/* The capture buttons. A button that starts a capture turns into the matching stop
 * button while one runs, so a GUI asks for the start action when state is READY or
 * the stop action when pin_state_is_capturing() (the capture's own states):
 *   START_MANUAL  analog "Capture", DV/HDV "Manual capture": READY
 *   START_AUTO    DV/HDV "Automatic rewind & capture" (drives the deck): READY and a camera
 *   STOP          stop the capture, leave the tape: CAPTURING or REWINDING (not while
 *                 STOPPING, the file is already being finalised)
 *   STOP_TAPE     stop the capture and the tape: STOP, and a camera */
typedef enum {
    PIN_CAPTURE_START_MANUAL = 0,
    PIN_CAPTURE_START_AUTO,
    PIN_CAPTURE_STOP,
    PIN_CAPTURE_STOP_TAPE,
} pin_capture_action_t;

PIN_API int pin_capture_action_allowed(pin_state_t state, int deck_available,
                                       pin_capture_action_t action);

/* DV/HDV "Manual capture" (START_MANUAL, but it records what is already arriving and
 * never issues deck commands): enabled only in state READY with an incoming video signal
 * (signal = the status snapshot's signal != 0); the deck state does not matter. While a
 * capture runs the button is the Stop button and pin_capture_action_allowed(STOP) applies
 * instead. pin_manual_capture_block_text() is the tooltip for the disabled button:
 * "No incoming video signal", or "" when the signal does not block it. Static, never NULL. */
PIN_API int pin_manual_capture_allowed(pin_state_t state, int deck_available, pin_deck_state_t deck,
                                       int signal);
PIN_API const char *pin_manual_capture_block_text(int deck_available, pin_deck_state_t deck,
                                                  int signal);

/* 1 for CAPTURING, STOPPING and REWINDING: a capture is running (or ending), so the
 * options are locked and the stop buttons show instead of the start buttons. */
PIN_API int pin_state_is_capturing(pin_state_t state);

/* Session state: "Closed", "Preparing…", "Ready", "Capturing", "Finalizing…",
 * "Rewinding (pass 2/3)", "Error". */
PIN_API void pin_format_state(const pin_status_snapshot_t *st, char *out, size_t cap);

/* The status bar's first line: while capturing the file name (no directory) and, for
 * several passes, "  ·  pass 2/3" (two spaces each side of the dot); while READY
 * after a capture that ended on its own or without video, the core's stop_text; "Error: "
 * + the error text; "Preparing: " + detail; else pin_format_state(). behind_hub = the open
 * device sits behind a USB hub (pin_device_info_t.hub_depth > 0): READY then reads
 * "Ready (connection via USB hub detected, see readme)" and the function returns 1 (the
 * tooltip should then be pin_usb_hub_hint()); else 0. */
PIN_API int pin_format_status_short(const pin_status_snapshot_t *st, int behind_hub,
                                    char *out, size_t cap);

/* What is arriving: "S-Video", "Composite", "HDV", "DV" or "DV/HDV" for the input, and with a
 * signal " · " + the format: DV/HDV the snapshot's video_label ("PAL", "1080i25"), analog
 * the decoder's standard (PAL, NTSC, PAL-M, PAL-N, PAL-60, NTSC-4.43, NTSC-J, SECAM). */
PIN_API void pin_format_signal(const pin_status_snapshot_t *st, char *out, size_t cap);

/* "Frames 1,234 · 2 err · 0 drop" (dropped = frames_dropped + write_dropped),
 * and its multi-line tooltip: totals (since capture start while capturing, else since
 * the app started), the current clip, and what counts as an error. */
PIN_API void pin_format_frames(const pin_status_snapshot_t *st, char *out, size_t cap);
PIN_API void pin_format_frames_detail(const pin_status_snapshot_t *st, char *out, size_t cap);

/* "1.5 GB / 300.0 MB": everything this capture wrote / the current file. */
PIN_API void pin_format_sizes(const pin_status_snapshot_t *st, char *out, size_t cap);

/* "· 20.0 GB free · 1 h 35 min left": the free space (only when known, disk_free_bytes
 * > 0) and the time left (only when est_seconds_left >= 0), else "". */
PIN_API void pin_format_storage_free(const pin_status_snapshot_t *st, char *out, size_t cap);

/* Multi-line tooltip for the storage item: bytes written (capture, file), free space,
 * time left, and a warning when disk_low. */
PIN_API void pin_format_storage_detail(const pin_status_snapshot_t *st, char *out, size_t cap);

/* Device list. "Ready" = READY or OPEN_HERE. */

/* The name, plus " (untested)" for a model nobody verified on hardware (tested == 0,
 * not UNSUPPORTED). */
PIN_API void pin_device_display_name(const pin_device_info_t *d, char *out, size_t cap);

/* Badge text: "Capturing" if capturing_here (this window records from it), else "Ready",
 * "Preparing…", "In use", "Driver missing", "Unsupported" or "Unknown". */
PIN_API void pin_device_status_text(const pin_device_info_t *d, int capturing_here,
                                    char *out, size_t cap);

/* Tooltip for a device that can't be picked ("In use by another window (pid 123)", ...).
 * Returns 0 and "" for one that can (READY, OPEN_HERE), else 1. PIN_DEV_NO_DRIVER is
 * also what a failed permission check on macOS / Linux reports, so its text depends on
 * the platform (Windows: install WinUSB with Zadig; Linux: a udev rule; macOS: something
 * else holds the device). */
PIN_API int pin_device_unavailable_reason(const pin_device_info_t *d, char *out, size_t cap);

/* The sentence for a failed attempt to open such a device ("This device is in use by
 * another program (process 123)."), platform-specific like the above. Same return values. */
PIN_API int pin_device_open_problem(const pin_device_info_t *d, char *out, size_t cap);

/* The hint shown while pin_enumerate() finds nothing; platform-specific. Static. */
PIN_API const char *pin_no_devices_hint(void);

/* For a device opened from a file descriptor (pin_open("fd:<n>"), Android): returns 1 and a
 * sentence for the user when its model cannot be driven that way yet, else 0 and "". d is
 * typically filled by pin_usb_model(). Today that is the MovieBox Deluxe, which reappears as
 * a new USB device after the core loads its USB controller firmware. */
PIN_API int pin_device_fd_problem(const pin_device_info_t *d, char *out, size_t cap);

/* ---- update check ---------------------------------------------------------
 * The published VERSION file (JSON: "version", "release_notes", "download_url") is
 * fetched from the project's GitHub repository and compared with pin_app_version().
 * The environment variable MARVIN_UPDATE_URL replaces the address (http(s) URL or a
 * local file path; empty turns the check off). */

#define PIN_UPDATE_NOTES_MAX 2048

typedef struct {
    uint32_t size;
    int32_t available;                  /* 1: `latest` is newer than `current` */
    char current[PIN_NAME_MAX];         /* pin_app_version() */
    char latest[PIN_NAME_MAX];          /* the published version */
    char download_url[PIN_PATH_MAX];    /* where to get it (a web page) */
    char notes[PIN_UPDATE_NOTES_MAX];   /* release notes, plain text, may contain '
' */
} pin_update_info_t;

/* Blocking (network): call it off the UI thread. timeout_ms bounds the whole fetch
 * (0 = 5000). PIN_OK when the published file was read and parsed, whether or not it
 * is newer (see `available`); any other status means the check could not be done and
 * should be ignored silently (offline, repository not reachable, malformed file). */
PIN_API pin_status_t pin_update_check(pin_update_info_t *info, uint32_t timeout_ms);

/* The one-line notice for an available update: "MarvinCapture 1.1 is available (you
 * have 1.0)." "" when info->available is 0. */
PIN_API void pin_format_update(const pin_update_info_t *info, char *out, size_t cap);

/* The same check driven by the front end, for platforms where the core cannot download the file
 * (Android: no libcurl; the app fetches it with the platform's own HTTP client). All of the
 * decisions stay here: when to look, what the file says, what to remember.
 *
 *   1. if (pin_update_due(now)) { url = pin_update_url(); download it, give up silently on any
 *      failure; then pin_update_check_json(...) }.
 *   2. whenever the user can see the notice (options panel): pin_update_known().
 *
 * `now` is the current time in seconds since 1970 (UTC). The check runs at most once a week:
 * the time of the last successful one is kept in the settings (key update.last_check), so a
 * failed download is tried again at the next start. */

/* The address of the published VERSION file (http(s) URL). "" = the check is turned off
 * (MARVIN_UPDATE_URL set empty). Static string, never NULL. */
PIN_API const char *pin_update_url(void);

/* 1 when the check is on and none succeeded in the last 7 days (or ever). */
PIN_API int pin_update_due(int64_t now);

/* Takes the downloaded VERSION file (len bytes), compares it with pin_app_version() and fills
 * *info (size set by the caller). On PIN_OK it remembers `now` as the last check and the
 * published version, download address and release notes (for pin_update_known(), so the notice
 * stays up between checks). PIN_ERR_IO when the file is not a valid VERSION file: nothing is remembered, so the
 * next start tries again. */
PIN_API pin_status_t pin_update_check_json(const char *json, size_t len, int64_t now,
                                           pin_update_info_t *info);

/* The same, for platforms where the core downloads the file itself (Windows, macOS, Linux; the
 * CLI): blocking (network), call it off the UI thread. When pin_update_due() it fetches the file
 * (timeout_ms bounds the fetch, 0 = 5000), evaluates it and remembers the result like
 * pin_update_check_json(); otherwise, or when the fetch fails (silently; tried again at the next
 * start), it uses what was remembered, like pin_update_known(). Returns 1 and fills *info
 * (size set by the caller) when a newer version is known, else 0. */
PIN_API int pin_update_refresh(pin_update_info_t *info, uint32_t timeout_ms);

/* 1 and *info filled (current, latest, download_url, notes, available = 1) when the
 * last check found a version newer than this one, else 0. Compared against the running version
 * each time, so the notice goes away by itself after the app was updated. No network. */
PIN_API int pin_update_known(pin_update_info_t *info);

#ifdef __cplusplus
}
#endif

#endif /* PIN_API_H */
