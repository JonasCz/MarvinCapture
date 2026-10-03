# Deck control (AV/C over FCP)

**Status (2026-09-25): working.** Tested on a Canon HDV camcorder (company
ID `0x000085`, 1080i/25 HDV tape) with `pindeck`, a deck-control tool since
removed (the traffic shown below is what `MarvinCaptureCLI --debug` prints for
the same commands):

- Play, Pause, Stop, FF, Rewind, TRANSPORT STATE and TIME CODE all work.
- Every command is answered on a **single send**, within a few
  milliseconds, and takes effect immediately.
- That is confirmed independently of the camera's replies by the EP 0x88
  stream: about 3.6 MB/s of HDV while the tape moves, 0 when stopped.
- The GUI and `MarvinCaptureCLI` use it through the session engine (`pin_deck`).
- The capture flow (rewind first, multi-pass, time limit, no-signal stop) was
  also verified on a Pinnacle 510-USB with a DV camcorder (short tape), see
  "Capture flow in the session engine" and "Verified on the DV camcorder".
- A Canon HDV camcorder with an HDV tape was tested again on a Pinnacle 510-USB
  (2026-10-03): rewind, FF, PLAY, STOP, capture with timecode waits, see
  "Verified on the Canon HDV camcorder".

The transport lives in the core library:
[`src/core/pinnacle_1394.c`](../src/core/pinnacle_1394.c) provides
`p1394_avc()`. It relies on the start-up described in
[startup.md](startup.md), and the message framing on EP 0x02 / 0x84 is in
[protocol.md](protocol.md).

Output of the removed `pindeck` (`state play wait:6 timecode pause state ff
wait:3 rew stop`); `MarvinCaptureCLI --debug` prints the same bytes as
`debug: AV/C -> 00 20 c3 75` and `debug: AV/C <- ACCEPTED 09 20 c3 75`:

```
device up (5.4 s), camera is node 1
[  5.652] step state
  <- STABLE          0c 20 c4 60          transport: WIND stop
[  5.662] step play
  <- ACCEPTED        09 20 c3 75          PLAY forward
  [  7.673] EP 0x88:    219.6 KB/s        (tape threading)
  [  9.703] EP 0x88:   3664.8 KB/s
[ 11.669] step timecode
  <- STABLE          0c 20 51 71 06 33 01 00   time code 00:01:33:06
[ 11.672] step pause
  <- ACCEPTED        09 20 c3 7d          PLAY forward-pause
[ 13.695] step state
  <- STABLE          0c 20 c3 7d          transport: PLAY forward-pause
[ 13.703] step ff
  <- ACCEPTED        09 20 c4 75          WIND fast-forward
  [ 15.719] EP 0x88:      0.0 KB/s
...
```

## Where the recipe came from

No new capture was needed. The old replayed start sequence already
contained about 45 complete AV/C transactions: the ones Windows'
`avc.sys`/`msdv.sys` issued while enumerating the camera, including
TRANSPORT STATE (`01 20 d0 7f`). Deck control is the same transaction with
a different payload.

MarvinBus64.sys contains **no AV/C or FCP code at all**, so there was
nothing vendor-specific to reverse: our driver plays the role of `avc.sys`.
The relevant decompile functions:

- `REQUEST_ASYNC_WRITE` = `FUN_00013ad0`.
- Header builders `FUN_00034d60` (tcode 0), `FUN_00035250` (tcode 1) and
  `FUN_000354b0` (tcode 9).
- The IRB table is at VA `0x4c000`.

`MarvinCaptureCLI --debug` prints this traffic (`AV/C -> ...` for each command
frame, `AV/C <- ACCEPTED ...` for the response, the ctype name first). Raw
AV/C probing of arbitrary frames (the old `pindeck raw:<hex>` and `reg:<ohci
offset>` steps) is no longer available from a shipped tool; the debug log shows
the traffic of the normal commands only.

## Sending a packet (AT request context)

`p1394_submit()` sends each transaction cold, one at a time, from RAM slot
`0x11c0`, as one EP 0x02 transfer:

1. type 2: OHCI `0x184` (ATreq.ContextControlClear) = `0x8000` (run).
2. type 8: the descriptor block into device RAM at `0x11c0`.
3. type 2: OHCI `0x18c` (ATreq.CommandPtr) = `0x11c0 | Z`.
4. type 2: OHCI `0x180` (ATreq.ContextControlSet) = `0x9000` (run|wake).

It then waits for the xferStatus writeback, retrying on `ack_busy`.

The vendor driver *chains* blocks through three slots instead
(`0x11c0`/`0x11e0`/`0x1200`), patching the previous block's branch word.
Since we wait for each status anyway, chaining buys nothing.

| form | block | Z | status writeback |
|---|---|---|---|
| quadlet write (tcode 0) | `120c0010 0 0 0` + 4 header quadlets (data in q3) | 2 | base+0x0c |
| write response (tcode 2), quadlet read (tcode 4) | `120c000c 0 0 0` + 3 header quadlets | 2 | base+0x0c |
| block write (tcode 1), lock (tcode 9) | `02000010 0 0 0` + 4 header quadlets + `100c<len> <base+0x30> 0 0` + payload | 3 | base+0x2c |

**The OHCI AT header:**

- `q0 = tl<<10 | rt<<8 | tcode<<4 | spd`.
- `q1 = dest<<16 | offset_hi`; for responses, `dest<<16 | rcode<<12`.
- `q2 = offset_lo`.
- `q3 = len<<16 | extended tcode` for block/lock, or the data quadlet for a
  quadlet write.

**Payload bytes, including the data quadlet of a quadlet write, go in wire
order** (`00 20 c3 75` as bytes), because `noByteSwapData` is cleared.

**Completion** is a type-9 write to the status address on EP 0x84. Bits
20:16 of the value are the OHCI event:

| code | event |
|---|---|
| `0x11` | ack_complete |
| `0x12` | ack_pending |
| `0x14` | ack_busy_X (retried) |
| `0x03` | evt_missing_ack (no node at that address) |

## Receiving (AR contexts)

**Buffers.** The AR request buffer is device RAM `0x3000`–`0x4fff`, and the
AR response buffer is `0x5000`–`0x6fff`.

**Arrival.** Every received packet arrives promptly as a type-9 message
into those ranges:

1. 4 header quadlets;
2. the payload, in wire order;
3. a trailer quadlet whose bits 20:16 are the ack our link sent.

**Where each answer lands:**

- The camera's responses to our reads and locks, and its write responses,
  land in the AR response buffer.
- Its FCP reply is a write to our `0xFFFFF0000D00` in the AR request
  buffer, acked `ack_pending`. We owe it a write response (tcode 2, its
  tlabel, rcode 0), which `p1394_answer_owed()` sends.

## AV/C commands (VCR subunit, `0x20`)

| step | bytes | observed on the Canon |
|---|---|---|
| play | `00 20 c3 75` | ACCEPTED. The stream runs at ~210 KB/s for ~3 s while the tape threads, then 3.6 MB/s. |
| pause | `00 20 c3 7d` | ACCEPTED; state `c3 7d`; the still frame keeps streaming |
| stop | `00 20 c4 60` | ACCEPTED; the stream drops to 0 within ~1 s |
| ff | `00 20 c4 75` | ACCEPTED; state `0b … c4 75` (IN_TRANSITION) while winding |
| rew | `00 20 c4 65` | ACCEPTED; likewise |
| search (time code control) | `00 20 51 20 FF SS MM HH` | NOT_IMPLEMENTED on the DV camcorder and on the Canon HDV (see below) |
| state (status) | `01 20 d0 7f` | `0c 20 <transport opcode> <mode>` |
| unit info | `01 ff 30 ff ff ff ff ff` | `0c ff 30 07 20 00 00 85` (VCR, Canon) |
| subunit info | `01 ff 31 07 ff ff ff ff` | `0c ff 31 07 20 38 ff ff` on the DV camcorder and on the Canon HDV camcorder of 2026-10-03; an earlier Canon HDV unit (first tests, `pindeck`) was recorded as NOT_IMPLEMENTED, so it varies by unit |
| time code (status) | `01 20 51 71 ff ff ff ff` | `0c 20 51 71 FF SS MM HH` (BCD) while playing; on the Canon HDV also while winding, REJECTED at the start of the tape |

## Camera behaviour (Canon HDV)

- **NOT_IMPLEMENTED means "busy".** While the mechanism changes mode (for
  example tape threading after PLAY, or mid-rewind), the camera may answer
  transport commands and TRANSPORT STATE with NOT_IMPLEMENTED (`08`), where
  it should say REJECTED or INTERIM. `pin_deck` retries transport commands
  up to 5 times, 0.7 s apart.
- **ACCEPTED means received, not done.** A TRANSPORT STATE sent a few
  milliseconds after STOP can still report the old mode (`STABLE PLAY`);
  about a second later it reports IN_TRANSITION or the new mode.

## Lessons that cost time

- **The replayed start sequence caused the "one command behind" effect.**
  With the old 232-packet replay, the camera seemed to execute each command
  only when the next one arrived, so a double send was needed.
  - The replay answered the camera's replies with the tlabels of a
    *different* camera, recorded in Windows. The real replies stayed
    unanswered, the camera kept re-sending them, and our commands were
    queued behind them.
  - With the generated start-up ([startup.md](startup.md)) this does not
    happen. The double send, the throw-away flush query and the "wait for
    quiet" were all removed.
- **A write response has a 12-byte (3-quadlet) header.** Sending it with
  reqCount 16 wedges the AT context: it stays `active`, `wake` is never
  consumed, and every later transaction silently goes nowhere.
- **Answer every FCP response.** Unanswered responses are re-sent about
  every 110 ms, with a new tlabel, indefinitely. A command sent while one is
  still pending gets `ack_busy_X`. `p1394_avc()` answers them before each
  command and while waiting.
- **Match responses to commands** by subunit and opcode. The exception is
  TRANSPORT STATE, whose response *replaces* the opcode with the transport
  mode (`0c 20 c4 60` = WIND/stop).
- **The camera must be on the bus.** Start-up now reports it: "1394 bus has
  2 node(s)", "camera is node 1".

## Capture flow in the session engine

`src/engine/pin_session.c` drives the deck during a capture (`dv_tick()`, run
from the stream loop's tick hook, about every 100 ms). Rules that matter:

- **One AV/C transaction at a time, through `pin_deck_async_t`.** It only
  advances while `deck_busy` is set, and the 1 Hz TRANSPORT STATE poll reuses
  the same struct. Always send commands with `deck_send()`, never with a bare
  `pin_deck_async_start()`: that was why "Automatic rewind & capture" never
  sent PLAY and why Stop was lost when a capture ended (the next poll
  overwrote the command before it was sent). `deck_send()` drops an in-flight
  status query, or queues behind an in-flight command.
- **Automatic rewind & capture** (`start_deck` + `rewind_first`): REW at once
  (no stream needed), poll the state until STOPPED (BOT), then PLAY, then open
  the file as soon as the first frame says DV or HDV (a stopped deck sends
  nothing, so the stream kind is unknown until PLAY: REW must not wait for it
  and PLAY goes out right after BOT). BOT is accepted only 3 s after REW was
  acknowledged, because a status query right after REW can still say
  "stopped". The session is REWINDING during the rewind, then READY (PLAY sent,
  waiting for the first frame) before CAPTURING; buttons follow `IsCapturing`,
  so they are usable in that READY gap. `last_data_s` (no-signal timer) is
  reset when the capture or the next pass starts, so the minutes of rewinding
  never count as "no signal".
- **NOT_IMPLEMENTED is never a deck state.** The answer (08) echoes the
  command; it must not be mapped to a transport state.
- **Stopping.** The no-signal timeout (`idle_stop_minutes`; time since data
  last arrived) and the time limit (`max_duration_minutes`, per pass: capture
  time of the current pass only, `elapsed_s`, which restarts with each pass so
  the rewind is not counted) end a pass; in the last pass they close the file
  and, if `start_deck`, send Stop.
  `pin_status_snapshot_t.idle_stop_remaining_s` / `duration_remaining_s` give
  the seconds left (-1 = off); `pin_format_remaining()` formats "5m30s".
- **Manual stop.** `pin_capture_stop()` sends deck Stop only if the capture was
  started with `start_deck`. `pin_capture_stop_ex(s, PIN_STOP_DECK_NO | _YES)`
  overrides that either way (the GUI's "continue tape" / "stop tape" buttons);
  the Stop goes through `deck_send()` like every other deck command.
- **Multi-pass.** The no-signal timeout is also how the end of the tape is
  noticed. With passes left it closes the file, sends REW (state REWINDING,
  `pass_rewinding`), and at BOT sends PLAY and opens the next file; the
  no-signal timer and the per-pass time limit restart with each pass (they must
  not count the rewind). Whichever limit comes first ends the pass.
- **Passes need a limit.** `pin_capture_passes_allowed(idle_min, duration_min)`
  is true when either is > 0; `pin_capture_opts_normalize()` forces `passes` to 1
  otherwise, and `pin_capture_start()` calls it for real devices (replay
  sessions are exempt: their end of file ends a pass). With only the time limit
  set, the no-signal check is off and the pass ends after the time alone; with
  both, whichever comes first.
- **Why it ended.** Every end of a capture goes through `capture_end()`, which
  closes the file, sends Stop if asked, and reports a `pin_stop_reason_t` with a
  ready-made sentence (`PIN_EVT_CAPTURE_ENDED`, `stop_*` in the status). See
  usage.md "When a capture stops by itself".
- **Bus reset during a capture.** The bus watch (`SelfIDCount` every 0.5 s)
  runs while a capture is active too. A topology change while the capture is
  under way (file open, or rewinding for it) ends it with `PIN_STOP_CAMERA_LOST`
  (no deck Stop: the node may have changed) and then re-scans as when idle. A
  capture still waiting for its first frame keeps waiting. Untested.
- Not implemented: detecting the end of tape from the deck state alone (with
  the no-signal timeout off, a multi-pass capture waits forever at the end).
  Left out on purpose: a false "stopped" while PLAY is still threading would
  abort a good capture. The DV camcorder does not stop by itself at the end of
  the recording (it keeps PLAY into blank tape), so the no-signal timeout is the
  only end-of-recording detection.
- **Blank section at the start.** A capture started while the deck is already
  in a blank section never gets a first frame and stays READY; the no-signal
  limit only applies after the first frame.
- Not yet verified on hardware: `pin_capture_stop_ex` variants (GUI only) and
  bus reset / camera loss during a capture.
- The deck's own timecode while winding is described under "Timecode while
  winding" below.

## Timecode while winding

TIME CODE (`01 20 51 71 ff ff ff ff`, answered `0c 20 51 71 FF SS MM HH`) is
the only position the deck reports over AV/C. While the tape winds there is no
DV stream (the camera sends 0 bytes), so the timecode display would stay empty.
`dv_tick()` therefore alternates TRANSPORT STATE and TIME CODE polls (0.5 s
apart, so each every second) while the deck is REWINDING or FAST_FORWARD and no
signal is arriving, and once more after the winding stopped. An IN_TRANSITION
(`0b`) answer is accepted as well as STABLE (`0c`); NOT_IMPLEMENTED, a short
answer or `ff` (no readable time code) leaves the last value on screen. While a
stream with its own timecode runs, the stream's value is used instead.

On the Canon HDV camcorder TIME CODE answers while winding in both directions
(see "Verified on the Canon HDV camcorder"). While an HDV stream plays whose GOP
headers carry no timecode (all zeros), the same poll runs during PLAY and capture.

On the DV camcorder TIME CODE answers during PLAY and when stopped mid-tape,
and is REJECTED (0a) during FF, at the start/end of the tape and in blank
sections. During REW a single poll was REJECTED, but the engine's alternating
polling did show a live timecode while rewinding. The parsing is covered by
`tests/engine/test_pin_deck.c`.

## Verified on the DV camcorder

Pinnacle 510-USB on Windows, DV camcorder with a short tape (about 5 min
recording):

- **TRANSPORT STATE** answers: stopped/wind-stop `0c 20 c4 60`; PLAY
  `0c 20 c3 75`; PAUSE `0c 20 c3 7d`; FF winding `0c 20 c4 75`; REW winding
  `0b 20 c4 65` (IN_TRANSITION) at the first poll, then `0c 20 c4 65`. REW
  reaching the start and FF reaching the end both end in `c4 60` (briefly
  `0b 20 c4 60` right at the end), so the `c4 60` mapping to STOPPED is right.
- **Wind/stop commands** ff `00 20 c4 75`, rew `00 20 c4 65` and stop
  `00 20 c4 60` all answer ACCEPTED (09). Wind speed is about 20x; a full
  rewind of the ~5 min recording took about 100 s.
- **Playing into blank tape:** the state stays PLAY (`c3 75`) and TIME CODE
  answers REJECTED (0a); the deck does not stop at the end of the recording.
- **No usable seek.** TIME CODE control (`00 20 51 20 FF SS MM HH`) is
  NOT_IMPLEMENTED and the deck does not move. ABSOLUTE TRACK NUMBER control
  (opcode 0x52, operand 0x20) is mostly NOT_IMPLEMENTED/REJECTED; one form,
  `00 20 52 20 00 00 01 ff`, was ACCEPTED late (about 10 s) and the deck seeked
  somewhere (ended in pause at 00:01:49:07), encoding not understood; while
  seeking the deck stopped answering status for a few seconds. Position the
  tape by FF/REW plus timecode polling instead.
- **Capture checks passed:** rewind-first capture (REW with live timecode, then
  PLAY, the file starts at the first frame); multi-pass with only a per-pass
  time limit (pass 1 about 60 s, rewind, pass 2; the rewind time is not
  counted); no-signal stop (the capture stopped 1 min after the recording
  ended and the deck was stopped).

## Verified on the Canon HDV camcorder

Pinnacle 510-USB on Windows, Canon HDV camcorder (company ID `0x000085`), HDV
tape (1440x1080 25 fps, 7+ minutes recorded), 2026-10-03:

- **TRANSPORT STATE** answers: REW accepted with `09` (ACCEPTED), then state
  `0b 20 c4 65` (IN_TRANSITION) for about 2 s, then `0c 20 c4 65`; FF `0b`/`0c 20
  c4 75`; PLAY `0b`/`0c 20 c3 75`; PAUSE `0c 20 c3 7d`; stopped `0b` right after
  STOP, then `0c 20 c4 60`.
- **TIME CODE** (`01 20 51 71 ff ff ff ff`) answers while winding and counts down
  during REW (and up during FF, where the first poll right after the command was
  REJECTED), and is REJECTED (`0a`) at the start of the tape. Unlike the DV
  camcorder's FF, it keeps answering while winding. The engine's alternating
  TRANSPORT STATE / TIME CODE poll therefore shows a live timecode while winding.
- **The HDV stream's GOP timecode is 00:00:00:00 in every GOP** on this camera, so
  the status line showed a constant zero while capturing. The engine uses the GOP
  timecode only once it has shown a non-zero value and otherwise keeps polling TIME
  CODE (0.5 s, alternating with TRANSPORT STATE) while the HDV plays, so timecode
  waits (`--wait HH:MM:SS:FF`) work during capture. See [hdv.md](hdv.md).
- **SUBUNIT INFO** is implemented on this unit: `0c ff 31 07 20 38 ff ff`.
- **No seek.** TIME CODE control (`00 20 51 20 FF SS MM HH`, several variants) and
  ABSOLUTE TRACK NUMBER control are NOT_IMPLEMENTED; ABSOLUTE TRACK NUMBER status
  works (`0c 20 52 71 ...`).
- **Conclusion:** neither tested deck (the DV camcorder, this Canon HDV) supports
  AV/C search, so there is no `--seek`; position the tape by FF/REW plus timecode
  polling.
- Capture flow verified: `--rew --wait --play --wait 00:00:20:00 --capture t.ts
  --wait 00:00:40:00 --stop` (the capture started at the deck's 00:00:20, the wait
  for 00:00:40 ended the capture after about 19 s, exit 0), and repeated
  `--ff --wait +00:00:40:00 --stop --rew --wait` rounds.

## Tool

`MarvinCaptureCLI` (`--rew`, `--ff`, `--play`, `--pause`, `--stop`, `--wait`,
[cli.md](cli.md)) and the GUI drive the deck through `pin_deck`. The tape keeps
doing whatever it was last told when the program exits (`--stop` also stops
it), and the camera's plug is released on exit. `--debug` logs every AV/C
exchange and the link layer; see [cli.md](cli.md#debug-log). The old
`pindeck` tool (with its `raw:`, `reg:` and `subunits` steps and the
`PINNACLE_DEBUG_1394` levels) is gone; the per-record EP 0x84 dump is kept
disabled under `#if 0` in `src/core/pinnacle_1394.c`.

## Open items

- Send the AV/C inquiries Windows sent (UNIT INFO, plug signal format) to
  detect DV versus HDV and the camera's capabilities before capture.
