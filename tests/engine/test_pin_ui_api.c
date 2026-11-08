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

/* pin_api.h's ready-made texts and enable rules that read a status snapshot or a
 * device entry: the exact strings every GUI shows. Links src/api/pin_api.c. */

#include "../../src/api/pin_api.h"
#include "../../src/engine/pin_format_opts.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);             \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)
#define CHECK_STR(a, b, msg)                                                 \
    do {                                                                     \
        if (strcmp((a), (b)) != 0) {                                         \
            printf("FAIL %s:%d: %s (got \"%s\", want \"%s\")\n", __FILE__,   \
                   __LINE__, msg, (a), (b));                                 \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static pin_status_snapshot_t snap(pin_state_t state)
{
    pin_status_snapshot_t st;
    memset(&st, 0, sizeof(st));
    st.size = sizeof(st);
    st.state = state;
    st.idle_stop_remaining_s = st.duration_remaining_s = -1;
    st.est_seconds_left = -1;
    st.progress_percent = -1;
    st.camera_present = -1;
    return st;
}

static void test_state_and_short(void)
{
    char b[512];
    pin_status_snapshot_t st = snap(PIN_STATE_REWINDING);
    st.pass = 2;
    st.passes = 3;
    pin_format_state(&st, b, sizeof(b));
    CHECK_STR(b, "Rewinding (pass 2/3)", "rewinding");
    st.state = PIN_STATE_PREPARING;
    pin_format_state(&st, b, sizeof(b));
    CHECK_STR(b, "Preparing\xe2\x80\xa6", "preparing");
    st.state = PIN_STATE_STOPPING;
    pin_format_state(&st, b, sizeof(b));
    CHECK_STR(b, "Finishing the file\xe2\x80\xa6", "stopping");
    st.state = PIN_STATE_CLOSED;
    pin_format_state(&st, b, sizeof(b));
    CHECK_STR(b, "Closed", "closed");

    CHECK(!pin_state_is_capturing(PIN_STATE_READY) && pin_state_is_capturing(PIN_STATE_CAPTURING) &&
          pin_state_is_capturing(PIN_STATE_STOPPING) && pin_state_is_capturing(PIN_STATE_REWINDING) &&
          !pin_state_is_capturing(PIN_STATE_ERROR), "capturing states");

    /* status bar line */
    st = snap(PIN_STATE_READY);
    CHECK(pin_format_status_short(&st, 0, b, sizeof(b)) == 0, "no hub");
    CHECK_STR(b, "Ready", "ready");
    CHECK(pin_format_status_short(&st, 1, b, sizeof(b)) == 1, "hub flagged");
    CHECK_STR(b, "Ready (connection via USB hub detected, see readme)", "ready behind a hub");
    st.state = PIN_STATE_PREPARING;
    CHECK(pin_format_status_short(&st, 1, b, sizeof(b)) == 0, "hub text only when READY");
    CHECK_STR(b, "Preparing\xe2\x80\xa6", "preparing without detail");
    snprintf(st.detail, sizeof(st.detail), "Uploading FPGA firmware (42%%)");
    pin_format_status_short(&st, 0, b, sizeof(b));
    CHECK_STR(b, "Preparing: Uploading FPGA firmware (42%)", "preparing detail");

    st = snap(PIN_STATE_ERROR);
    st.last_error = PIN_ERR_USB;
    pin_format_status_short(&st, 0, b, sizeof(b));
    CHECK_STR(b, "Error: USB error", "error without text");
    snprintf(st.error_text, sizeof(st.error_text), "firmware rejected");
    pin_format_status_short(&st, 0, b, sizeof(b));
    CHECK_STR(b, "Error: firmware rejected", "error text");

    st = snap(PIN_STATE_CAPTURING);
    pin_format_status_short(&st, 0, b, sizeof(b));
    CHECK_STR(b, "Capturing", "capturing before the first file");
    snprintf(st.current_file, sizeof(st.current_file), "C:\\Videos\\tape-0001.avi");
    st.pass = 1;
    st.passes = 1;
    pin_format_status_short(&st, 0, b, sizeof(b));
    CHECK_STR(b, "tape-0001.avi", "file name only (backslashes)");
    snprintf(st.current_file, sizeof(st.current_file), "/home/u/tape-0001.dv");
    st.pass = 2;
    st.passes = 3;
    pin_format_status_short(&st, 0, b, sizeof(b));
    CHECK_STR(b, "tape-0001.dv  \xc2\xb7  pass 2/3", "file name and pass");
    st.state = PIN_STATE_STOPPING;
    pin_format_status_short(&st, 0, b, sizeof(b));
    CHECK_STR(b, "Finishing tape-0001.dv\xe2\x80\xa6", "stopping names the file being finished");
    st.current_file[0] = 0;
    pin_format_status_short(&st, 0, b, sizeof(b));
    CHECK_STR(b, "Finishing the file\xe2\x80\xa6", "stopping without a file shows the state");
    pin_format_status_line(&st, b, sizeof(b));
    CHECK_STR(b, "Finishing the file\xe2\x80\xa6", "status line while finishing, no \"(no signal)\"");

    st = snap(PIN_STATE_READY);
    snprintf(st.stop_text, sizeof(st.stop_text), "Capture stopped after capturing 5s, because ...");
    st.stop_reason = PIN_STOP_USER;
    pin_format_status_short(&st, 0, b, sizeof(b));
    CHECK_STR(b, "Ready", "the user's own stop is not shown");
    st.stop_reason = PIN_STOP_NO_SIGNAL;
    pin_format_status_short(&st, 1, b, sizeof(b));
    CHECK_STR(b, st.stop_text, "stop text wins over the hub text");
    st.stop_reason = PIN_STOP_USER;
    st.stop_no_video = 1;
    pin_format_status_short(&st, 0, b, sizeof(b));
    CHECK_STR(b, st.stop_text, "no video is shown after a user stop too");
    st.stop_text[0] = 0;
    pin_format_status_short(&st, 0, b, sizeof(b));
    CHECK_STR(b, "Ready", "no text, no line");
}

static void test_signal(void)
{
    char b[64];
    pin_status_snapshot_t st = snap(PIN_STATE_READY);
    st.input = PIN_INPUT_DV;
    pin_format_signal(&st, b, sizeof(b));
    CHECK_STR(b, "DV/HDV", "dv, nothing arriving");
    st.signal = 1;
    pin_format_signal(&st, b, sizeof(b));
    CHECK_STR(b, "DV/HDV", "no label yet");
    st.stream_kind = PIN_KIND_HDV;
    snprintf(st.video_label, sizeof(st.video_label), "1080i25");
    pin_format_signal(&st, b, sizeof(b));
    CHECK_STR(b, "HDV \xc2\xb7 1080i25", "hdv");
    st.stream_kind = PIN_KIND_DV;
    snprintf(st.video_label, sizeof(st.video_label), "PAL");
    pin_format_signal(&st, b, sizeof(b));
    CHECK_STR(b, "DV \xc2\xb7 PAL", "dv");
    st.signal = 0;
    pin_format_signal(&st, b, sizeof(b));
    CHECK_STR(b, "DV", "dv, no signal");

    st.input = PIN_INPUT_SVIDEO;
    st.signal = 1;
    st.detected_std = PIN_STD_NTSC_443;
    pin_format_signal(&st, b, sizeof(b));
    CHECK_STR(b, "S-Video \xc2\xb7 NTSC-4.43", "s-video ntsc 4.43");
    st.input = PIN_INPUT_COMPOSITE;
    st.detected_std = PIN_STD_SECAM;
    pin_format_signal(&st, b, sizeof(b));
    CHECK_STR(b, "Composite \xc2\xb7 SECAM", "composite secam");
    st.detected_std = PIN_STD_AUTO;
    snprintf(st.video_label, sizeof(st.video_label), "NTSC");
    pin_format_signal(&st, b, sizeof(b));
    CHECK_STR(b, "Composite \xc2\xb7 NTSC", "auto falls back to the label");
    st.signal = 0;
    pin_format_signal(&st, b, sizeof(b));
    CHECK_STR(b, "Composite", "analog, no signal");
}

static void test_counters(void)
{
    char b[512];
    pin_status_snapshot_t st = snap(PIN_STATE_READY);
    pin_format_frames(&st, b, sizeof(b));
    CHECK_STR(b, "Frames 0 \xc2\xb7 0 err \xc2\xb7 0 drop", "zero frames");
    st.frames = 1234;
    st.frames_error = 2;
    st.frames_dropped = 1;
    st.write_dropped = 1500;
    pin_format_frames(&st, b, sizeof(b));
    CHECK_STR(b, "Frames 1,234 \xc2\xb7 2 err \xc2\xb7 1,501 drop", "frames");

    pin_format_capture_summary(&st, b, sizeof(b));
    CHECK_STR(b, "Frames 1,234, with errors 2, dropped 1,501", "capture summary");
    st.lost_blocks = 4321;
    pin_format_capture_summary(&st, b, sizeof(b));
    CHECK_STR(b, "Frames 1,234, with errors 2, dropped 1,501, 4,321 lost blocks", "summary with lost blocks");
    st.lost_blocks = 0;
    st.state = PIN_STATE_CAPTURING;
    st.pass = 1;
    st.passes = 1;
    st.scene = 1;
    pin_format_status_line(&st, b, sizeof(b));
    CHECK(strstr(b, "  err 2  drops 1501") != NULL, "status line shows err and drops");
    st.state = PIN_STATE_READY;

    st.clip_frames = 10;
    st.clip_frames_error = 1;
    st.clip_frames_dropped = 3;
    pin_format_frames_detail(&st, b, sizeof(b));
    CHECK_STR(b, "Total (since the app started)\nFrames: 1,234\nWith errors: 2\nDropped: 1,501\n\n"
                 "Current clip\nFrames: 10\nWith errors: 1\nDropped: 3\n\n"
                 "A frame has an error if the camera damaged or concealed it,\n"
                 "data was missing, or (HDV) it depends on a damaged picture.", "detail, idle");
    st.state = PIN_STATE_CAPTURING;
    pin_format_frames_detail(&st, b, sizeof(b));
    CHECK(strstr(b, "Total (since capture start)\n") == b, "detail, capturing");

    st = snap(PIN_STATE_READY);
    pin_format_sizes(&st, b, sizeof(b));
    CHECK_STR(b, "0 B / 0 B", "sizes zero");
    st.total_bytes_written = 1610612736ull;
    st.clip_bytes_written = 300ull << 20;
    pin_format_sizes(&st, b, sizeof(b));
    CHECK_STR(b, "1.5 GB / 300.0 MB", "sizes");

    st = snap(PIN_STATE_READY);
    pin_format_storage_free(&st, b, sizeof(b));
    CHECK_STR(b, "", "no storage info");
    st.disk_free_bytes = 20ull << 30;
    pin_format_storage_free(&st, b, sizeof(b));
    CHECK_STR(b, "\xc2\xb7 20.0 GB free", "free only");
    st.est_seconds_left = 95 * 60; /* 1 h 35 min */
    pin_format_storage_free(&st, b, sizeof(b));
    CHECK_STR(b, "\xc2\xb7 20.0 GB free \xc2\xb7 1 h 35 min left", "free and time");
    st.disk_free_bytes = 0;
    pin_format_storage_free(&st, b, sizeof(b));
    CHECK_STR(b, " \xc2\xb7 1 h 35 min left", "time only keeps its leading space");

    st = snap(PIN_STATE_CAPTURING);
    st.total_bytes_written = 2048;
    st.clip_bytes_written = 1024;
    pin_format_storage_detail(&st, b, sizeof(b));
    CHECK_STR(b, "Written in this capture: 2.0 KB\nWritten in the current file: 1.0 KB", "detail minimal");
    st.disk_free_bytes = 1ull << 30;
    st.est_seconds_left = 7200;
    st.disk_low = 1;
    pin_format_storage_detail(&st, b, sizeof(b));
    CHECK_STR(b, "Written in this capture: 2.0 KB\nWritten in the current file: 1.0 KB\n"
                 "Free on the output volume: 1.0 GB\n2 h 00 min left\n\n"
                 "Low disk space: less than 1 hour or 50 GB left", "detail full");
}

static void test_deck_name(void)
{
    CHECK_STR(pin_deck_state_name(PIN_DECK_STOPPED), "Stopped", "stopped");
    CHECK_STR(pin_deck_state_name(PIN_DECK_PLAYING), "Playing", "playing");
    CHECK_STR(pin_deck_state_name(PIN_DECK_PAUSED), "Paused", "paused");
    CHECK_STR(pin_deck_state_name(PIN_DECK_FAST_FORWARD), "Fast forward", "ff");
    CHECK_STR(pin_deck_state_name(PIN_DECK_REWINDING), "Rewinding", "rew");
    CHECK_STR(pin_deck_state_name(PIN_DECK_RECORDING), "Camera recording", "rec");
    CHECK_STR(pin_deck_state_name(PIN_DECK_NO_TAPE), "No tape", "no tape");
    CHECK_STR(pin_deck_state_name(PIN_DECK_UNKNOWN), "\xe2\x80\x94", "unknown");
    CHECK_STR(pin_deck_state_name((pin_deck_state_t)99), "\xe2\x80\x94", "out of range");

    char tip[96];
    pin_format_deck_tip(PIN_DECK_PLAYING, 0, tip, sizeof(tip));
    CHECK_STR(tip, "Deck: Playing", "tip idle");
    pin_format_deck_tip(PIN_DECK_PLAYING, 1, tip, sizeof(tip));
    CHECK_STR(tip, "Deck: Playing \xe2\x80\x94 waiting for the deck to respond", "tip busy");

    CHECK(pin_deck_cmd_allowed(PIN_STATE_READY, 1, PIN_DECK_STOPPED, PIN_DECK_CMD_PLAY) == 1, "wrapper");
    CHECK(pin_capture_action_allowed(PIN_STATE_READY, 0, PIN_CAPTURE_START_AUTO) == 0, "wrapper");
}

static void test_devices(void)
{
    char b[256];
    pin_device_info_t d;
    memset(&d, 0, sizeof(d));
    d.size = sizeof(d);
    snprintf(d.name, sizeof(d.name), "Pinnacle Studio 500-USB");
    d.tested = 1;

    pin_device_display_name(&d, b, sizeof(b));
    CHECK_STR(b, "Pinnacle Studio 500-USB", "tested name");
    d.tested = 0;
    pin_device_display_name(&d, b, sizeof(b));
    CHECK_STR(b, "Pinnacle Studio 500-USB (untested)", "untested name");
    d.state = PIN_DEV_UNSUPPORTED;
    pin_device_display_name(&d, b, sizeof(b));
    CHECK_STR(b, "Pinnacle Studio 500-USB", "unsupported is not flagged untested");

    const struct { pin_dev_state_t s; const char *text; } badge[] = {
        { PIN_DEV_READY, "Ready" }, { PIN_DEV_OPEN_HERE, "Ready" },
        { PIN_DEV_PREPARING, "Preparing\xe2\x80\xa6" }, { PIN_DEV_IN_USE, "In use" },
        { PIN_DEV_NO_DRIVER, "Driver missing" }, { PIN_DEV_UNSUPPORTED, "Unsupported" },
        { (pin_dev_state_t)77, "Unknown" },
    };
    for (size_t i = 0; i < sizeof(badge) / sizeof(badge[0]); i++) {
        d.state = badge[i].s;
        pin_device_status_text(&d, 0, b, sizeof(b));
        CHECK_STR(b, badge[i].text, "badge");
    }
    d.state = PIN_DEV_IN_USE;
    pin_device_status_text(&d, 1, b, sizeof(b));
    CHECK_STR(b, "Capturing", "capturing here");

    d.state = PIN_DEV_READY;
    CHECK(pin_device_unavailable_reason(&d, b, sizeof(b)) == 0 && !b[0], "ready: usable");
    d.state = PIN_DEV_OPEN_HERE;
    CHECK(pin_device_unavailable_reason(&d, b, sizeof(b)) == 0 && !b[0], "open here: usable");
    CHECK(pin_device_open_problem(&d, b, sizeof(b)) == 0 && !b[0], "open here: no problem");
    d.state = PIN_DEV_IN_USE;
    d.owner_pid = 4321;
    CHECK(pin_device_unavailable_reason(&d, b, sizeof(b)) == 1, "in use");
    CHECK_STR(b, "In use by another window (pid 4321)", "in use reason");
    CHECK(pin_device_open_problem(&d, b, sizeof(b)) == 1, "in use problem");
    CHECK_STR(b, "This device is in use by another program (process 4321).", "in use problem text");
    d.owner_pid = 0;
    pin_device_unavailable_reason(&d, b, sizeof(b));
    CHECK_STR(b, "In use by another program", "in use, owner unknown");
    pin_device_open_problem(&d, b, sizeof(b));
    CHECK_STR(b, "This device is in use by another program.", "in use problem, owner unknown");
    d.state = PIN_DEV_PREPARING;
    d.owner_pid = 12;
    pin_device_unavailable_reason(&d, b, sizeof(b));
    CHECK_STR(b, "Being prepared by another window (pid 12)", "preparing reason");
    d.owner_pid = 0;
    pin_device_unavailable_reason(&d, b, sizeof(b));
    CHECK_STR(b, "Being prepared by another program", "preparing reason, owner unknown");
    pin_device_open_problem(&d, b, sizeof(b));
    CHECK_STR(b, "Another program is preparing this device.", "preparing problem");
    d.state = PIN_DEV_UNSUPPORTED;
    pin_device_unavailable_reason(&d, b, sizeof(b));
    CHECK_STR(b, "This model isn't supported yet", "unsupported reason");
    pin_device_open_problem(&d, b, sizeof(b));
    CHECK_STR(b, "This model is recognised but not supported yet.", "unsupported problem");
    d.state = PIN_DEV_NO_DRIVER;
    CHECK(pin_device_unavailable_reason(&d, b, sizeof(b)) == 1 && b[0], "no driver reason");
    CHECK(pin_device_open_problem(&d, b, sizeof(b)) == 1 && b[0], "no driver problem");
#if defined(_WIN32)
    CHECK(strstr(b, "Zadig") != NULL, "windows: Zadig");
#else
    CHECK(strstr(b, "Zadig") == NULL, "no Zadig outside Windows");
#endif
    CHECK(strncmp(pin_no_devices_hint(), "No devices found.", 17) == 0, "no-devices hint");
}

/* The name field's and the aspect drop-downs' help texts: they name the formats (no probe ran
 * here, so the H.264 / HEVC formats are never listed as storing an aspect). */
static void test_format_help(void)
{
    char b[512];
    pin_format_title_help(PIN_INPUT_DV, PIN_FMT_DV_AVI, PIN_FMT_HDV_TS, b, sizeof(b));
    CHECK_STR(b, "File name for DV and HDV (scenes and passes get numbered suffixes). Also stored as the "
                 "title in DV AVI and raw .m2t (HDV) files.", "title: DV AVI, raw HDV");
    pin_format_title_help(PIN_INPUT_DV, PIN_FMT_DV_RAW, PIN_FMT_HDV_TS, b, sizeof(b));
    CHECK_STR(b, "File name for DV and HDV (scenes and passes get numbered suffixes). Also stored as the "
                 "title in raw .m2t (HDV) files; raw .dv files have no title.", "title: both raw");
    pin_format_title_help(PIN_INPUT_DV, PIN_FMT_DV_MOV, PIN_FMT_HDV_MKV, b, sizeof(b));
    CHECK_STR(b, "File name for DV and HDV (scenes and passes get numbered suffixes). Also stored as the "
                 "title in DV MOV and HDV MKV files.", "title: both store it");
    pin_format_title_help(PIN_INPUT_DV, PIN_FMT_DV_RAW, PIN_FMT_HDV_MOV, b, sizeof(b));
    CHECK_STR(b, "File name for DV and HDV (scenes and passes get numbered suffixes). Also stored as the "
                 "title in HDV MOV files; raw .dv files have no title.", "title: raw DV, HDV MOV");
    pin_format_title_help(PIN_INPUT_SVIDEO, PIN_FMT_ANALOG_FFV1_MKV, PIN_FMT_HDV_TS, b, sizeof(b));
    CHECK_STR(b, "File name (the extension follows the format). Also stored as the title in FFV1 MKV files.",
              "title: analog (the HDV format ignored)");
    pin_format_title_help(PIN_INPUT_DV, PIN_FMT_COUNT, PIN_FMT_COUNT, b, sizeof(b));
    CHECK_STR(b, "File name for DV and HDV (scenes and passes get numbered suffixes).", "title: no list yet");
    pin_format_title_help(PIN_INPUT_COMPOSITE, PIN_FMT_DV_AVI, PIN_FMT_COUNT, b, sizeof(b));
    CHECK_STR(b, "File name (the extension follows the format).", "title: a DV format in the analog field");

    pin_format_aspect_help(PIN_FMT_DV_RAW, b, sizeof(b));
    CHECK(strncmp(b, "Raw .dv files have no aspect flag of their own: players take the aspect from the video "
                     "itself", 88) == 0, "aspect: raw DV, why");
    CHECK(strstr(b, " DV AVI and DV MOV files store one.") != NULL, "aspect: raw DV, which formats store one");
    CHECK(strstr(b, "H.264") == NULL, "aspect: no encoder probed, no H.264");
    pin_format_aspect_help(PIN_FMT_HDV_TS, b, sizeof(b));
    CHECK(strncmp(b, "Raw .m2t (HDV) files have no aspect flag of their own", 53) == 0 &&
          strstr(b, " HDV MOV and HDV MKV files store one.") != NULL, "aspect: raw HDV");
    pin_format_aspect_help(PIN_FMT_DV_AVI, b, sizeof(b));
    CHECK_STR(b, "Overrides the aspect ratio for the preview and the DV AVI files. Auto uses what the tape "
                 "reports.", "aspect: DV AVI");
    pin_format_aspect_help(PIN_FMT_HDV_MKV, b, sizeof(b));
    CHECK_STR(b, "Overrides the aspect ratio for the preview and the HDV MKV files. Auto uses what the stream "
                 "reports (16:9 for HDV).", "aspect: HDV MKV");
    pin_format_aspect_help(PIN_FMT_ANALOG_AVI, b, sizeof(b));
    CHECK_STR(b, "4:3, or 16:9 for anamorphic widescreen tapes (Auto is 4:3). Affects the preview and the "
                 "uncompressed AVI files.", "aspect: analog");
    pin_format_aspect_help(PIN_FMT_COUNT, b, sizeof(b));
    CHECK_STR(b, "", "aspect: unknown format");

    /* scene split: forced for HDV MOV / MKV (checked and disabled in a GUI), else what it does */
    pin_format_info_t sfi;
    memset(&sfi, 0, sizeof(sfi));
    sfi.size = sizeof(sfi);
    CHECK(pin_format_info(PIN_FMT_HDV_MOV, &sfi) == PIN_OK && sfi.scene_split_forced == 1 &&
          sfi.supports_scene_split == 1, "scene split forced: HDV MOV");
    CHECK(pin_format_info(PIN_FMT_HDV_MKV, &sfi) == PIN_OK && sfi.scene_split_forced == 1, "forced: HDV MKV");
    CHECK(pin_format_info(PIN_FMT_HDV_TS, &sfi) == PIN_OK && sfi.scene_split_forced == 0, "not forced: raw HDV");
    CHECK(pin_format_info(PIN_FMT_DV_AVI, &sfi) == PIN_OK && sfi.scene_split_forced == 0, "not forced: DV AVI");
    pin_format_scene_split_help(PIN_FMT_HDV_MKV, b, sizeof(b));
    CHECK(strncmp(b, "Always on for HDV MKV files:", 28) == 0, "scene split help: forced");
    pin_format_scene_split_help(PIN_FMT_DV_RAW, b, sizeof(b));
    CHECK_STR(b, "A new numbered file for each recording on the tape: where the timecode or the recording "
                 "date jumps.", "scene split help: DV");
    pin_format_scene_split_help(PIN_FMT_HDV_TS, b, sizeof(b));
    CHECK(strstr(b, "camera's clock") != NULL, "scene split help: HDV names the clock");
    pin_format_scene_split_help(PIN_FMT_ANALOG_AVI, b, sizeof(b));
    CHECK_STR(b, "Uncompressed AVI files are not split into scenes.", "scene split help: analog");
    pin_format_scene_split_help(PIN_FMT_COUNT, b, sizeof(b));
    CHECK_STR(b, "", "scene split help: unknown format");
    /* the format drop-down's note per kind: HDV says MOV / MKV are not truly lossless */
    char kb[1024];
    pin_format_kind_help(PIN_KIND_HDV, kb, sizeof(kb));
    CHECK(strstr(kb, "Note the MOV and MKV for capturing HDV are not truly lossless") != NULL &&
          strstr(kb, "DaVinci Resolve") != NULL && strstr(kb, ".m2t raw HDV stream") != NULL,
          "format help: HDV");
    CHECK(strncmp(kb, "MOV needs a clean finish", 24) == 0 && strstr(kb, "MKV and the raw .m2t stay readable") != NULL,
          "format help: HDV MOV needs a clean finish");
    pin_format_kind_help(PIN_KIND_DV, kb, sizeof(kb));
    CHECK(strncmp(kb, "AVI and MOV need a clean finish", 31) == 0 &&
          strstr(kb, "MKV and the raw .dv stay readable") != NULL, "format help: DV AVI and MOV need a clean finish");
    pin_format_kind_help(PIN_KIND_ANALOG, kb, sizeof(kb));
    CHECK(strncmp(kb, "AVI needs a clean finish", 24) == 0 && strstr(kb, "MKV stays readable") != NULL &&
          strstr(kb, "MOV") == NULL && strstr(kb, "raw") == NULL, "format help: analog AVI needs a clean finish");
    pin_format_kind_help((pin_kind_t)99, kb, sizeof(kb));
    CHECK_STR(kb, "", "format help: unknown kind has none");
    pin_format_kind_help(PIN_KIND_HDV, kb, 40);
    CHECK(strlen(kb) == 39, "format help: cut to the buffer");

    /* the format table and the rule the script and the session use agree */
    for (int f = 0; f < PIN_FMT_COUNT; f++) {
        memset(&sfi, 0, sizeof(sfi));
        sfi.size = sizeof(sfi);
        CHECK(pin_format_info((pin_format_t)f, &sfi) == PIN_OK &&
              sfi.scene_split_forced == pin_fmt_always_splits((pin_format_t)f) &&
              (!sfi.scene_split_forced || sfi.supports_scene_split), "scene_split_forced: one rule");
    }
    /* an older caller's struct size (before scene_split_forced) gets that layout */
    memset(&sfi, 0x55, sizeof(sfi));
    sfi.size = (uint32_t)offsetof(pin_format_info_t, scene_split_forced);
    CHECK(pin_format_info(PIN_FMT_HDV_MOV, &sfi) == PIN_OK && sfi.scene_split_forced == 0x55555555 &&
          sfi.supports_aspect == 1, "format info: old struct size");
    pin_format_aspect_help(PIN_FMT_DV_RAW, b, 20);
    CHECK(strlen(b) == 19, "aspect: cut to the buffer");
    pin_format_title_help(PIN_INPUT_DV, PIN_FMT_DV_RAW, PIN_FMT_HDV_TS, b, 30);
    CHECK(strlen(b) == 29, "title: cut to the buffer");
    pin_format_title_help(PIN_INPUT_DV, PIN_FMT_DV_RAW, PIN_FMT_HDV_TS, NULL, 0);   /* no crash */
}

int main(void)
{
    test_state_and_short();
    test_signal();
    test_counters();
    test_deck_name();
    test_devices();
    test_format_help();
    if (g_failures) {
        printf("%d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_pin_ui_api: OK\n");
    return 0;
}
