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
  camera: put what was reasoned and not verified into `docs/handoff-deck-flow.md`.
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
  or the capture; total time (`max_duration_minutes`, summed over passes) always
  ends it; both send deck Stop when `start_deck`. Remaining seconds are in the
  status snapshot (`idle_stop_remaining_s`, `duration_remaining_s`).
- Timecode while winding comes from the TIME CODE status poll, only when no
  stream timecode is arriving.
- Mirroring a new status field: `pin_api.h` (appended, ABI-compatible) ->
  `Interop/NativeStructs.cs` same order.
