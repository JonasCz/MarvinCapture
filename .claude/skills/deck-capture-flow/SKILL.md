---
name: deck-capture-flow
description: How the session engine drives the camera/deck during a DV/HDV capture (rewind first, PLAY, stop on no-signal/total-time, multi-pass, timecode while winding): the async AV/C state machine and its traps. Read before changing deck or capture behaviour in src/engine/pin_session.c.
---

# Deck / capture flow (src/engine/pin_session.c, pin_deck.c)

Details and the AV/C byte values: `docs/deck-control.md` ("Capture flow in the
session engine"). The traps:

- **Send deck commands only with `deck_send()`.** `pin_deck_async_t` is one
  shared struct; `dv_tick()` only polls it while `deck_busy` is set and reuses it
  every second for the TRANSPORT STATE poll. A bare `pin_deck_async_start()`
  without `deck_busy = 1` is overwritten by the next poll and the command is never
  sent (this was the "rewinds but never plays" and "Stop is not sent" bug).
- **No camera in the loop, so no end-to-end test.** The engine's deck code needs
  a real 1394 link; replay sessions skip `dv_tick()`'s deck part. Unit tests cover
  only the pure parts (`tests/engine/test_pin_deck.c`). Anything else needs the
  camera: say in the commit/report what was reasoned and not verified, and
  list it under "Capture flow" in `docs/deck-control.md`.
- **ACCEPTED is not done.** A status query right after REW/STOP can report the old
  mode; BOT/stopped is trusted only 3 s after the command was acknowledged
  (`deck_cmd_done_s`). NOT_IMPLEMENTED (08) echoes the command: never map it to a
  deck state.
- **Timers that count "no data"** (`last_data_s`) must be reset when a capture or
  pass starts; the minutes spent rewinding are not "no signal".
- A stopped deck sends no data, so the stream kind (DV/HDV) stays unknown: PLAY
  must go out before waiting for it, and rewinding must not wait for it.
- State flow with `start_deck` + `rewind_first`: READY -> REWINDING (REW, poll to
  STOPPED) -> READY (PLAY sent, waiting for the first frame) -> CAPTURING.
  Between passes: CAPTURING -> REWINDING (`pass_rewinding`) -> CAPTURING.
- Stopping: no-signal (`idle_stop_minutes`) ends a pass (rewind if passes are left)
  or the capture; the time limit (`max_duration_minutes`) is per pass (`elapsed_s`,
  reset when the next pass starts, so no rewind time) and ends a pass exactly like
  no-signal does; in the last pass both end the capture and send deck Stop when
  `start_deck`. Remaining seconds are in the
  status snapshot (`idle_stop_remaining_s`, `duration_remaining_s`).
- Passes need a limit: `pin_capture_passes_allowed()` / `pin_capture_opts_normalize()`
  (no-signal or time limit, else passes = 1), enforced in `pin_session_capture_start`
  except for replay sessions; the GUI mirrors it via `Native.PassesAllowed`.
- **End a capture only with `capture_end(s, reason, detail, deck_node)`** (then
  set the state). It closes the file, sends Stop, and reports the reason
  (`PIN_EVT_CAPTURE_ENDED` + `stop_*` status; abnormal reasons per
  `pin_stop_abnormal()` get the GUI's dialog, normal ones a status-bar line). A pass change uses `finish_file()`. `capture_guard()` (disk reserve,
  writer failure) runs from dv_tick / analog_tick / the replay loops;
  `stream_failed()` handles a read loop that returned by itself (unplug).
- Bus reset during a capture ends it (`PIN_STOP_CAMERA_LOST`, user's decision:
  a loose cable must not silently lose frames), then the usual re-scan runs.
  The bus watch runs while `capture_active()`.
- Manual stop: `pin_capture_stop_ex(stop_deck)` (PIN_STOP_DECK_AS_STARTED/NO/YES)
  decides about deck Stop independent of `start_deck`; the GUI's two stop buttons
  use NO / YES.
- Timecode while winding comes from the TIME CODE status poll, only when no
  stream timecode is arriving.
- Mirroring a new status field: `pin_api.h` (appended, ABI-compatible) ->
  `Interop/NativeStructs.cs` same order.
