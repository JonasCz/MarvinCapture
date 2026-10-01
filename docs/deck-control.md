# Deck control (AV/C over FCP)

**Status (2026-09-25): working.** Tested on a Canon HDV camcorder (company
ID `0x000085`, 1080i/25 HDV tape) with `pindeck`:

- Play, Pause, Stop, FF, Rewind, TRANSPORT STATE and TIME CODE all work.
- Every command is answered on a **single send**, within a few
  milliseconds, and takes effect immediately.
- That is confirmed independently of the camera's replies by the EP 0x88
  stream: about 3.6 MB/s of HDV while the tape moves, 0 when stopped.
- The GUI and `pinctl` use it through the session engine (`pin_deck`);
  `pincli` does not, and it has not been tried with a DV camera.

The transport lives in the core library:
[`src/core/pinnacle_1394.c`](../src/core/pinnacle_1394.c) provides
`p1394_avc()`. It relies on the start-up described in
[startup.md](startup.md), and the message framing on EP 0x02 / 0x84 is in
[protocol.md](protocol.md).

```
$ pindeck state play wait:6 timecode pause state ff wait:3 rew stop
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

`pindeck -vv` prints this traffic.

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
| state (status) | `01 20 d0 7f` | `0c 20 <transport opcode> <mode>` |
| unit info | `01 ff 30 ff ff ff ff ff` | `0c ff 30 07 20 00 00 85` (VCR, Canon) |
| subunit info | `01 ff 31 07 ff ff ff ff` | NOT_IMPLEMENTED |
| time code (status) | `01 20 51 71 ff ff ff ff` | `0c 20 51 71 FF SS MM HH` (BCD) while playing |

## Camera behaviour (Canon HDV)

- **NOT_IMPLEMENTED means "busy".** While the mechanism changes mode (for
  example tape threading after PLAY, or mid-rewind), the camera may answer
  transport commands and TRANSPORT STATE with NOT_IMPLEMENTED (`08`), where
  it should say REJECTED or INTERIM. `pindeck` retries transport commands
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
  the file as soon as the first frame says DV or HDV. BOT is accepted only
  3 s after REW was acknowledged, because a status query right after REW can
  still say "stopped". The session is REWINDING during the rewind.
- **Stopping.** The no-signal timeout (`idle_stop_minutes`; time since data
  last arrived) and the total-time limit (`max_duration_minutes`, capture time
  summed over all passes) close the file and, if `start_deck`, send Stop.
  `pin_status_snapshot_t.idle_stop_remaining_s` / `duration_remaining_s` give
  the seconds left (-1 = off); `pin_format_remaining()` formats "5m30s".
- **Multi-pass.** The no-signal timeout is also how the end of the tape is
  noticed. With passes left it closes the file, sends REW (state REWINDING,
  `pass_rewinding`), and at BOT sends PLAY and opens the next file; the
  no-signal timer restarts with each pass (it must not count the rewind). The
  total-time limit ends everything instead.
- Not implemented: detecting the end of tape from the deck state alone (with
  the no-signal timeout off, a multi-pass capture waits forever at the end).
- The deck's own timecode while winding is described under "Timecode while
  winding" below.

## Tool

`pindeck [-b bitstream] [-v|-vv] [-r raw.bin] step...`

- It runs the same bring-up as `pincli`, then the steps: `play`, `pause`,
  `stop`, `ff`, `rew`, `state`, `timecode`, `subunits`, `wait:<sec>`,
  `raw:<hex>` and `reg:<ohci offset>`.
- It prints each AV/C exchange, and the EP 0x88 rate once a second during
  `wait:`.
- On exit it stops isochronous receive and releases the camera's plug. The
  tape keeps doing whatever it was last told, so `pindeck play` followed by
  `pincli` captures from tape.
- `PINNACLE_DEBUG_1394=1|2` logs the link layer. Level 2 adds every EP 0x84
  record and our config ROM.

## Open items

- Wire `p1394_avc()` into `pincli` (`--play`, stopping at the end). During
  capture EP 0x84 is read by the capture event loop, so FCP responses need to
  be handled from there.
- Try a DV camera.
- Send the AV/C inquiries Windows sent (UNIT INFO, plug signal format) to
  detect DV versus HDV and the camera's capabilities before capture.
