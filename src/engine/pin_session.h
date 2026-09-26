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
 * Session engine: one worker thread + command queue per open device, the
 * state machine (PREPARING/READY/CAPTURING/STOPPING/REWINDING/ERROR), the
 * capture pipeline (dv_reassembler / pinnacle_analog_capture_loop feeding a
 * pin_writer_t -> pin_sink_t), deck control and the scene-split debounce
 * FIFO. src/api/pin_api.c is a thin wrapper around exactly these functions
 * (same names/signatures as pin_api.h, "pin_" -> "pin_session_"), so this
 * header is the real contract; pin_api.h is its frozen public mirror.
 *
 * Threading and locking: see pin_session_priv.h. Every function here may be
 * called from any thread; all of them take s->mtx for a short section and
 * (for anything that touches hardware) post to the worker's command
 * mailbox rather than doing the work inline.
 */

#ifndef PIN_SESSION_H
#define PIN_SESSION_H

#include "../api/pin_api.h"

#ifdef __cplusplus
extern "C" {
#endif

pin_status_t pin_session_open(const char *device_id, pin_session_t **out);
void pin_session_close(pin_session_t *s);

pin_status_t pin_session_set_input(pin_session_t *s, pin_input_t input);
pin_status_t pin_session_set_standard(pin_session_t *s, pin_std_t std);
pin_status_t pin_session_get_control(pin_session_t *s, pin_control_t c, pin_control_info_t *out);
pin_status_t pin_session_set_control(pin_session_t *s, pin_control_t c, int32_t value);

pin_status_t pin_session_deck(pin_session_t *s, pin_deck_cmd_t cmd);

pin_status_t pin_session_check_output(pin_session_t *s, const pin_capture_opts_t *o,
                                       pin_output_check_t *out);
pin_status_t pin_session_set_output_hint(pin_session_t *s, const pin_capture_opts_t *o);
pin_status_t pin_session_capture_start(pin_session_t *s, const pin_capture_opts_t *o,
                                        int overwrite);
pin_status_t pin_session_capture_stop(pin_session_t *s);

pin_status_t pin_session_get_status(pin_session_t *s, pin_status_snapshot_t *out);
int pin_session_poll_event(pin_session_t *s, pin_event_t *out);

pin_status_t pin_session_run_actions(pin_session_t *s, const pin_launch_t *launch);

void pin_session_set_aspect(pin_session_t *s, pin_aspect_t aspect);

void pin_session_monitor_enable(pin_session_t *s, int enabled);
int pin_session_monitor_read(pin_session_t *s, int16_t *out, int max_frames);
int pin_session_monitor_available(pin_session_t *s);

/* Global (not per-session): firmware directory override, see pin_api.h's
 * pin_set_firmware_dir(). Resolves fpga-ohci.bin / fpga-capture.bin. */
pin_status_t pin_session_set_firmware_dir(const char *utf8_dir);
pin_status_t pin_session_firmware_path(pin_kind_t for_kind, char *out, size_t out_size);

/* Global (not per-session): the replay (virtual device) source file, see
 * pin_api.h's pin_set_replay_file(). NULL/"" clears it. The getter returns
 * 0 and fills out[] if a file is set (by this call or PIN_REPLAY, which
 * still wins if both are set, for ctest compatibility), else -1. */
void pin_session_set_replay_file(const char *path);
int pin_session_get_replay_file(char *out, size_t out_size);

/* Process-wide device-change wait, see pin_api.h's pin_devices_wait() /
 * pin_devices_wake(). Platform-specific implementation in
 * pin_devices_wait_win.c / pin_devices_wait_unix.c. */
int pin_session_devices_wait(int timeout_ms);
void pin_session_devices_wake(void);

/* Format/option-rules table (pin_api.h's pin_formats()/pin_format_info()):
 * lives here, not in pin_api.c, since pin_session.c itself needs it (e.g.
 * to resolve an output extension) and pin_api.c is a thin wrapper over this
 * layer, never the other way around. */
int pin_session_formats(pin_kind_t kind, pin_format_info_t *out, int max);
pin_status_t pin_session_format_info(pin_format_t f, pin_format_info_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PIN_SESSION_H */
