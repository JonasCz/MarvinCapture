# Handoff: deck / capture flow changes (untested with a camera)

Written without a camera or deck attached (only the 500-USB), so every change
below was found by reading `src/engine/pin_session.c` and `pin_deck.c`; only the
pure helpers have unit tests (`tests/engine/test_pin_deck.c`). Background:
[deck-control.md](deck-control.md#capture-flow-in-the-session-engine) and the
`deck-capture-flow` skill.

## What was found and changed

1. **"Automatic rewind & capture" never sent PLAY** (and Stop was lost at the end
   of a capture). Cause: `pin_deck_async_start()` was called without
   `deck_busy = 1` in `start_capture_now()`, the capture-stop paths and the
   time-limit stop. The state machine only advances while `deck_busy` is set, and
   the 1 Hz transport-state poll restarts the same struct, so the command was
   overwritten before it was ever sent. All sends now go through `deck_send()`.
2. Rewind-first waited for the stream kind (DV/HDV) before even starting; a
   stopped deck sends nothing, so it could never start. REW now starts at once,
   PLAY goes out after BOT, and the file opens when the first frame arrives.
3. BOT was "deck state == STOPPED". Now also 3 s after REW was acknowledged
   (docs say ACCEPTED is not "done" and an early status query can still say
   stopped).
4. The no-signal timer (`last_data_s`) was not reset when the capture or the next
   pass started, so after minutes of rewinding it was already expired: the
   capture would have stopped immediately with a "no signal" limit set.
5. NOT_IMPLEMENTED answers (echo of the command) were mapped to a deck state.
6. Time limit ("Stop after") is per pass again (capture time of the current
   pass, rewind excluded): it ends the pass like the no-signal timeout (rewind +
   next pass, or deck Stop and end in the last pass). An intermediate version
   made it span all passes; that was reverted.

## To verify with the camera

- Multi-pass with only "Stop after" (`--passes 2`, `idle_stop_minutes = 0`,
  `max_duration_minutes = 1`): pass ends after 1 min of capture, REW, BOT, PLAY,
  pass 2; the minute is not counted while rewinding. Only the pure rule
  (`pin_capture_passes_allowed`) has a unit test.

- `pin_capture_stop_ex`: GUI "Stop capture & stop tape" on a Manual capture must
  stop the tape; "Stop capture & continue tape" on an Automatic capture must
  leave it playing (only reasoned, no camera test).

- `pinctl capture -d <id> -i dv -o x --rewind-first --idle-min 1` (tape not at the
  start): REW, stops at BOT, PLAY, file starts, and the countdown/Stop at the end.
- The Canon's state after REW reaches the start. The code expects TRANSPORT STATE
  `c4 60` (WIND stop). If it reports another wind mode (e.g. `c4 45` is mapped to
  REWINDING; anything else is UNKNOWN), BOT is never seen and the capture stays in
  REWINDING. Check with `pindeck rew wait:20 state`.
- Does TIME CODE (`01 20 51 71 ff ff ff ff`) answer while winding, and with
  which response byte (0c/0b)? `pindeck rew wait:3 timecode wait:2 timecode stop`.
  If it is NOT_IMPLEMENTED the display just stays blank (nothing breaks).
- Multi-pass (`--passes 2 --idle-min 1`): at the end of the tape no data -> 1 min
  later REW -> BOT -> PLAY -> second file. A threading delay after PLAY of more
  than the no-signal timeout would end pass 2 at once (the timer restarts at the
  PLAY, so use a timeout of at least 1 min).
- Not implemented: end-of-tape detection from the deck state (the deck going
  STOPPED while playing). Only the no-signal timeout ends a pass; with it off a
  multi-pass capture waits forever at the end of the tape. Left out on purpose
  because a false "stopped" during PLAY threading would abort a good capture.
- GUI: while the rewind runs the window shows state Rewinding, then briefly READY
  (PLAY sent, waiting for the first frame) before Capturing; buttons follow
  `IsCapturing` so they are momentarily usable in that READY gap.
