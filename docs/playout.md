# MarvinPlayout: playing DV and HDV files out over FireWire

`MarvinPlayout` plays `.dv` (DV) or `.m2t`/`.ts` (HDV) files out of the FireWire port of the
Pinnacle device, as a virtual tape deck. It is a separate command-line program installed next to
`MarvinCaptureCLI` and uses the same left-to-right syntax and wait conditions (see [cli.md](cli.md)).

**Status: DV playout works on hardware with four fixes not yet in the code** (2026-11-20, 510-USB to
500-USB: bit-exact PAL and NTSC, script mode and virtual deck). Without them it does not transmit
(the EP 0x06 stream stalls). See
[Hardware test results](#hardware-test-results-2026-11-19) and its second session. The packetizer, the virtual tape and deck, the AV/C
target, the script and the whole playout loop are unit-tested without a device, and `--dry-run`
works. The code that talks to the Marvin (transmit on EP 0x06, the 1394 plug registers, the IRM) is
written from the decompiled vendor driver ([handoff/playout-feasibility.md](handoff/playout-feasibility.md))
and has never been tried. Every assumption is marked `GUESS:` in the source; the test plan is at the
end of this page.

## What it does

- The files you name are one **tape**, in the order given; each file is one recording and keeps its
  own timecode (a file without one continues the previous file's). The files may be of different
  formats, like a tape with mixed recordings: see [Tapes that mix formats](#tapes-that-mix-formats).
- The **virtual deck** behaves like a camcorder in VCR mode: PLAY, PAUSE, STOP, FF, REW and timecode
  search take time, the tape runs at a speed, and a search ends paused at its target.
- It **answers AV/C commands** from the node on the other end of the cable (a capture program, or
  another Marvin running `MarvinCaptureCLI --rew --wait --play --capture ...`), and it can **run its own
  actions** from the command line.
- It sends the stream as the camcorder would: IEC 61883-2 (DV) or -4 (HDV) packets, one isochronous
  packet per bus cycle. While the deck is stopped or winding it sends empty packets; paused, it repeats
  the paused frame (DV) or sends empty packets (HDV).

## Usage

```
MarvinPlayout [settings] FILE... [settings and actions...]
```

Run `MarvinPlayout --help` for all options. Settings and files come before the first action; actions
run left to right as in `MarvinCaptureCLI`:

| | |
|---|---|
| `--play --pause --stop --ff --rew` | transport command; returns as soon as the deck has taken it |
| `--goto HH:MM:SS:FF` | timecode search: wind there, settle, pause. Fails (exit 3) if it is not on the tape |
| `--wait [COND,...]`, `--wait-any`, `--wait-all` | `idle[=DUR]`, `timecode=HH:MM:SS:FF`, `wallclock=DUR` (the capture conditions `signal`, `nosignal`, `captured` do not apply) |
| `--wind-speed X`, `--start-delay SEC`, `--search-settle SEC` | the timing model below |
| `--channel N`, `--speed s100|s200|s400` | isochronous channel (default 63, the broadcast channel) and bus speed (default s100) |
| `-d ID` | device (default: the first one) |
| `--no-virtual-deck` | do not answer AV/C; only the command line drives the deck |
| `--stay` | keep serving after the actions, until Ctrl-C |
| `--dry-run OUT`, `--realtime` | no device: write the stream to a file; paced like the bus with `--realtime` |
| `--switch KIND=gap:SEC,blank:SEC,reset:0\|1,restart:0\|1` | experiments: change what happens when the tape changes format (below) |
| `--debug` | debug log: raw AV/C traffic, bring-up details |

Exit codes: 0 ok, 1 usage error, 2 device or bring-up error (also: no device, no firmware), 3 a wait
failed or the deck refused an action, 130 Ctrl-C (a second Ctrl-C quits at once).

### Examples

```sh
# Serve as a deck for a capture program on the other end of the cable, until Ctrl-C
MarvinPlayout tape.dv

# Two recordings as one tape; send the first 10 s of the second one to a camcorder in DV-in mode
MarvinPlayout a.dv b.dv --goto 00:12:00:00 --wait --play --wait wallclock=00:00:10 --stop

# Check what would go out, without a device (as fast as possible)
MarvinPlayout --dry-run out.dv a.dv b.dv
MarvinPlayout --dry-run out.dv a.dv --rew --wait --play --wait timecode=00:10:00:20 --stop
```

What a script does with the connection: **with actions, MarvinPlayout makes the connection itself** (a
broadcast connection on `--channel`, allocating channel and bandwidth at the isochronous resource
manager unless the channel is 63) and transmits from the start; the script ends when its last action
is done (unless `--stay`). **With no actions it is a pure virtual deck**: it transmits only while a
controller has connected our output plug (a point-to-point connection, or a broadcast one), and serves
until Ctrl-C. With `--dry-run` and no actions the whole tape is played once and the program exits.

### Status

One line a second on stderr (the stream itself is never on stdout):

```
Tape: DV PAL, 2 files, 00:00:04:00
STOP  00:10:00:00  file 1/2 dv-pal.dv  frames sent 0  local connection
PLAY  00:10:00:15  file 1/2 dv-pal.dv  frames sent 15  controller node 1
PAUSE 00:10:00:07  file 2/2 dv-pal.dv  frames sent 65  controller node 1
```

"frames sent" counts tape frames put on the bus, repeats while paused included. In a dry run without
`--realtime` the lines come once per real second although the simulated time runs much faster.

## The timing model

Everything is a guess until a real receiver has been tried; all of it can be changed on the command line.

| what | default |
|---|---|
| stop or pause to play | 1.0 s before the picture starts (`--start-delay`) |
| play to pause or stop | 0.3 s; play goes on meanwhile |
| FF / REW | 30 times real time (`--wind-speed`); stop at the end / start of the tape |
| wind starts | 0.5 s after the command |
| timecode search | wind to the target, 1.5 s settle (`--search-settle`), end paused there |
| end of the tape in play | STOP |

A command changes the state a controller sees at once (as AV/C decks do after ACCEPTED); what comes out
follows after the delay. In PLAY the tape advances exactly one frame per frame period sent, so a late
loop iteration never drops or repeats a frame; the wall clock only drives mode changes and winding.

The clock: with a device, the host's monotonic clock, and the 1394 output queue (about one frame deep)
paces the loop. With `--dry-run`, a **simulated clock** advances one frame period per period produced, so
the deck behaves as if the time had passed but a 2 h tape takes seconds; `--realtime` sleeps instead.

## The output

`--dry-run OUT` feeds the packets through the receive side of the capture code (the same path the
device's EP 0x88 stream takes), so the file is what a capture program would record from this stream.
DV: byte for byte the tape's frames (plus repeats while paused). HDV: PAT/PMT first, then the stream from
the first GOP on; the picture in progress at the end is dropped. This is what `tests/playout` checks.

## Tapes that mix formats

A tape may hold DV and HDV, DV PAL and NTSC, HDV 1080i and 720p, in any order. Every file is played in
its own format and keeps its own timecode base and drop-frame flag (a file without timecode continues
from the label after the previous file's last frame, its frame number rescaled to the new base: PAL
`00:10:00:12` goes on as NTSC `00:10:00:14`; a timecode search reads the label in each file's own base,
earliest file first). The deck counts tape frames, one per output period, and its wind speed uses the
first file's frame rate, so winding past a format change is only approximately 30 times real time.

**What a real camera sends over FireWire when its tape switches format has not been observed yet.**
MarvinPlayout implements a best guess, kept in one small table (`src/playout/pout_switch.c`) so it is
easy to change once a camera has been seen ([testing-required.md](testing-required.md#3-tapes-that-mix-dv-and-hdv-or-pal-and-ntsc)).
When the next frame to send is in another format than the stream:

1. **gap**: the stream stops, no isochronous packets at all, for `gap_s`. The deck keeps playing, so the
   tape frames under the head in that time (and in the blank time) are lost, like a real tape running.
2. The stream starts again in the new format (the CIP header, DBC from 0; the bus cycle count runs on) and
   sends **blank** time of empty packets of the new format.
3. Then the frames go on. An HDV frame after a switch starts at its GOP, like after a search.

On the device, the switch can also **restart the IT context** (needed when the packet size and CIP format
change) and **initiate a bus reset** (off by default; the stream then restarts as after any bus reset, which
ends the switch). From the switch on, the AV/C OUTPUT PLUG SIGNAL FORMAT answer and the oPCR payload follow
the new format. For a mixed tape bandwidth and the IT ring are sized for the larger HDV packets (584 bytes) up
front, so the IRM allocation does not change mid-stream. If nobody receives during a switch (no
connection, bus reset) it ends at once.

| switch | gap | blank | bus reset | restart IT | why |
|---|---|---|---|---|---|
| DV <-> HDV (`kind`) | 1.0 s | 0.2 s | no | yes | camcorders with an "i.LINK conversion: off / AUTO" setting switch the i.LINK stream with the tape and warn that the signal is interrupted at the change; packet size and CIP format change |
| DV PAL <-> NTSC (`std`; also HDV 50 <-> 60 Hz) | 0 | 0.5 s | no | no | the stream goes on; the deck re-locks its servo to the new frame rate |
| HDV 1080i <-> 720p (`hdv-format`) | 0.5 s | 0.2 s | no | no | same CIP format, the encoder chain restarts on a new sequence |

**This is a guess.** For experiments the table can be changed on the command line, several times if wanted:

```sh
MarvinPlayout --switch kind=gap:2,blank:0,reset:1 --switch std=blank:0.1 dv.dv hdv.m2t ntsc.dv
```

`KIND` is `std`, `hdv-format` or `kind`; keys not mentioned keep their value. `--switch kind=gap:0,blank:0`
loses no frames. With `--dry-run` there is no device, so `reset` and `restart` do nothing; the output
file is what is checked: one `.dv` holds PAL and NTSC together, but a DV <-> HDV change starts a new file
(`out-0002.m2t`, `out-0003.dv`, numbered as a capture numbers scenes) and the summary lists the files:

```
$ MarvinPlayout --dry-run out.dv dv-pal.dv hdv.ts dv-ntsc.dv --switch kind=gap:0.4,blank:0.2
Tape: mixed DV PAL + HDV 1080i50 + DV NTSC, 3 files, 00:00:05:05
playout: the tape changes from DV PAL to HDV 1080i50 (kind): 0.40 s without packets, then 0.20 s of empty ones
playout: the format changes, continuing in out-0002.m2t
...
99 frames sent to 3 files: out.dv, out-0002.m2t, out-0003.dv
```

## What is a guess

- **Everything on the device**: EP 0x06 descriptor and message layout (`pout_it.h`), when the IT context
  starts, the type-11 message, the byte order of packets and descriptors, the 4-block ring, the cycle
  number of the first packet (`START_LEAD_CYCLES`, 3 ms), the SYT / source-packet timestamps and the 3-cycle
  transfer delay (`pout_cip.h`). `--debug` logs the traffic.
- **The 1394 target side** (`pout_csr.h`): the AT response header layout, the quadlet byte order, that a
  controller finds the node from the AV/C unit directory alone, the oPCR field widths for HDV, that bus
  resets clear the plugs and the IRM resources (`pout_link_rescan`). After a bus reset the stream stops
  for a moment and restarts; a local connection is made again, a controller must reconnect.
- **AV/C answers** (`pout_avc.h`): what has no recorded camera answer behind it. The UNIT INFO company ID
  is 0, which some controllers dislike.
- **Format switches** of a mixed tape: everything above (`pout_switch.h`): whether the stream stops at all,
  for how long, whether empty packets follow, bus reset or not, when the plug and AV/C answers change.
- **HDV**: timecode comes from the GOP headers (not the aux stream), a "frame" is a slot of 1/fps on the
  PCR timeline (720p50/60 gives two pictures per slot), and playing starts at the GOP start before the
  requested frame, so what goes out is up to a GOP behind the deck's timecode and the last frames of the
  tape are not sent (`pout_session.h`).
- **HDV RAM layout (TODO)**: the vendor's four 584-byte payload buffers (from RAM 0x1770) end at 0x20b0, past
  the start of our self-ID buffer at 0x2000 (`pinnacle_1394.c`, link init). The vendor driver has the same
  overlap with its own self-ID area, so it is probably harmless, but nobody has checked, and `pout_it` has no
  knob for the buffer base (`POUT_IT_RAM_BUFFERS` is the vendor's). A warning is logged when an HDV stream starts.
  If HDV misbehaves at bus resets or in the first seconds, move the self-ID buffer below the ring.
- **Speed and channel**: `--speed` sets the oPCR rate and the speed of a local connection; `--channel 63`
  needs no IRM allocation. The default is s100, the safe one; S400 works only if the whole path does.
- **Firmware**: `fpga-ohci.bin` is looked for in the settings key `Paths/firmware_dir`, then `firmware/`
  next to the executable, then `./firmware` (the build tree and `dist/` have it next to `MarvinPlayout`).

## Hardware test plan

Follow [handoff/playout-feasibility.md](handoff/playout-feasibility.md#hardware-test-plan-later); in
detail, with `--debug` and the exit code, each step only if the previous worked:

1. `MarvinPlayout --dry-run out.dv tape.dv` and compare with the tape (no device needed; automatic in ctest).
2. **Transmit only**: a DV camcorder in VCR / DV-in mode as receiver, `MarvinPlayout short.dv --play --wait`.
   Picture on the camcorder proves EP 0x06, IT and the CIP framing. If nothing is shown: try `--speed s100`
   and a camcorder on a direct cable, look at the `--debug` log for EP 0x84 descriptor status writebacks
   and "stream failed" messages.
3. **Bus reset**: unplug and replug the camcorder while playing; the stream must restart by itself.
4. **Virtual deck, no script** (`MarvinPlayout tape.dv`): a PC with a FireWire port and dvgrab / Windows
   MSDV as the controller; check that the node is found, that the output plug can be connected and that
   PLAY, STOP, FF, REW and a timecode search work.
5. **Two Marvins**: one runs `MarvinPlayout tape.dv`, the other `MarvinCaptureCLI --rew --wait --play
   --capture cap.dv --wait nosignal=00:00:05`; the capture should equal the tape.
6. **HDV**: the same with a `.m2t` and the Canon HDV camcorder as receiver; then the RAM-overlap TODO above.
7. **Mixed tape**: `dv-pal.dv hdv.m2t dv-ntsc.dv` to a capture program: it should follow the switches (a new file
   per kind); try `--switch kind=reset:1` and other values, and compare with what a real camera does
   ([testing-required.md](testing-required.md#31-dv-and-hdv-on-one-tape)).
8. Recording to tape: AV/C RECORD to the camcorder while playing ("export to tape").

## Hardware test results (2026-11-19)

A 510-USB running MarvinPlayout and a 500-USB running MarvinCaptureCLI, joined by one FireWire cable,
on Windows 10 x64, HEAD 8523d32 (with the GUID-read fix described in
[testing-required.md](testing-required.md#bench-session-2026-11-19-windows-500-usb--510-usb-head-8523d32)).
Tape: a 20 s PAL DV file made by ffmpeg (`testsrc2`, timecode 00:10:00:00).

| step | result |
|---|---|
| 1. `--dry-run` | the output equals the tape byte for byte (500 frames) |
| bring-up, warm start | works: OHCI design found and kept, link up, 2-node bus |
| 1394 target side (`pout_csr`) | works: the 500 read oMPR `0x3f000001` and oPCR[0], and its compare-swap connected the plug (`0x813f007a` / `0xc13f007a`, channel 63); MarvinPlayout logged "a controller connected our output plug" and its responses completed with `ack_complete` (`0x11`) |
| IT contexts in the FPGA | 8 (`IsoXmitIntMaskSet` reads back `0xff`) |
| 2. / 5. transmit | **fails**: the first EP 0x06 bulk write (~26.8 KB) times out after 3 s (`LIBUSB_ERROR_TIMEOUT`, "EP 0x06 write failed (-1 of 26880 bytes, usb -7)"), 0 frames sent, exit 2. Same in virtual-deck mode (after the 500 connected) and in script mode with no receiver |
| AV/C target | not reached: the capture's REW came after MarvinPlayout had exited |

What the registers say after the stall (read through EP 0x02 from a local diagnostic build): IT0
ContextControl `0x00009011` (run and wake set, not active, event `ack_complete`), CommandPtr
`0x000016b3` (block 0, Z = 3), IsoXmitIntEvent 0. So the context was started, ran its program and
stopped at its end. The likely blocker is the **type-11 message**: the FPGA holds the EP 0x06 stream
in it and never lets it go.

Experiments with a local build (env switches, not in the code):

- s bit on every OUTPUT_LAST (`status_every` 1 instead of 64): no change, same stall. Afterwards
  EP 0x02 writes on that unit also timed out until the next bring-up.
- **No type-11 messages at all**: EP 0x06 takes everything and MarvinPlayout runs to the end of its
  script (exit 0), but at about twice real time (1499 frames in 30 s; nothing paces the loop), and
  the 500-USB, connected to the plug on channel 63, receives **0 EP 0x88 completions** ("No video
  received", exit 5). Whether anything reached the wire is unknown.

The type-11 reading above was wrong; the second session (below) found the real causes.

### Second session (2026-11-20): playout works

Same bench, same 510-USB → 500-USB cable. `MarvinBus64.sys` / `MarvinAVS64.sys` were read again
(the installed copies, ImageBase `0x10000`, so the Ghidra addresses are VA = RVA + `0x10000`), then
fixes were tried with env switches in a local build (not in the code). **With four changes, DV
playout works bit-exactly, in script mode and as a virtual deck.**

What the vendor driver says (certain from the disassembly):

- **The EP 0x06 stream `pout_it.c` builds is the vendor's, byte for byte**: slot `n % 4`, sequence
  `n % 64` in the OUTPUT_LAST address word, CommandPtr `0x16b0` (Z = 0) plus run|wake at packet 3, a
  patch and a wake on every packet from 4 on, type 11 = `0xb0000000 | first << 26 | ((n - 3) & 0x3f)`
  on every packet from 3 on (`FUN_0002db20`, its only caller of `FUN_0002c400`). The one difference is
  the s bit (below). A USB transfer is cut after the packet that passes 30000 bytes; nothing marks a
  transfer's end.
- **Nothing else is set up for transmit**: TALK (`FUN_0003db50`) sends type-4 register 0 with bit 8
  clear (isochronous on), then only builds host state; no IsoXmitIntMask, IntMask, IT match,
  ContextControlClear or FPGA register >= `0x800`. Stop (`FUN_0002e450`): type-4 bit 8 set,
  ContextControlClear `0x8000`, then poll ContextControl until active and run are clear (200 ms).
- `MarvinAVS64` queues up to 300 EP 0x06 URBs (`MaxPending1394IsoWrite` default `0x12c`), plain bulk
  with `USBD_SHORT_TRANSFER_OK`, no timeout; EP 0x84 always has reads pending. The vendor reacts to
  bus resets through the EP 0x84 type-`0xa` IntEvent record (bit 17) and clears it with `0x084` =
  `0x20000`.
- The FPGA has IT event bits (16..22 of the type-6 event mask `0x0c`: `IT_fifo_underflow`,
  `IT_cycle_missed`, `IT_wait_begin`, `IT_wait_done`, ...); the vendor leaves them masked
  (`0x2008`). Enabling them (`0x7f2008`) produced no EP 0x84 record we could see.

What was wrong, each found by bisecting on hardware (5 / 5 runs pass with all four, from a freshly
loaded design each time):

1. **No cycle master.** When the idle 500 was root (`NodeID 0x8000ffc0`), no cycle start packets were
   sent, IT0 never became active (ContextControl `0x9000`, CommandPtr still `0x16b0`), the type-11 wait
   for packet 0 never ended and EP 0x06 stopped after ~1 KB. Every passing run had the 510 as root
   (`0xc000ffc1`), where its own cycleMaster bit (already set by the link init) takes effect. **Fix
   tried:** after link init, if NodeID bit 30 (root) is clear, set PHY register 1 RHB | IBR (`0xc0`)
   and wait for the reset: the 510 became root every time. (A real camcorder as root is cycle master;
   an idle Marvin or a PC without a running stack may not be. Forcing root unconditionally costs
   one bus reset; a bus with another working cycle master would not need it.)
2. **The s bit.** With `status_every` 64 (the s bit on every 64th OUTPUT_LAST) EP 0x06 stopped after
   11776 bytes every time; with s never set it never did. The vendor sets s only on the last packet
   of a client buffer that asked for completion. **Fix:** never set it (`status_every` 0).
3. **EP 0x84 not read while waiting for an EP 0x06 chunk.** `take_free_locked()` slept while the
   sender thread blocked; with a bus reset during the stream (the 500's bring-up), the unread EP 0x84
   records stalled the FPGA: EP 0x06 stopped (7168 of 19908 bytes) and EP 0x02 writes timed out until
   the next bring-up. **Fix tried:** `pout_link_pump(k, 1)` instead of `sleep_ms(1)` there.
4. **Bus resets are seen too late.** SelfIDCount's generation changed only ~1.5 s after the reset
   (after the controller had already reconnected), while IntEvent bit 17 was set at once. The
   controller (the 500's capture) connects our oPCR within milliseconds of its own reset, so the
   0.25 s poll saw the reset after the new connection and its rescan wiped it: the deck played, 0
   frames sent. **Fix tried:** detect the reset by IntEvent bit 17 (clear with `0x084` = `0x20000`),
   and check it before dispatching each batch of incoming requests, so the CSR state is reset before
   a lock that came after the reset; the session's own reset handling then keeps the plugs.

Results with the four fixes:

| run | result |
|---|---|
| script `pal20.dv --play --wait wallclock=00:00:05 --stop`, no receiver | 95 frames in 5 s (real time), exit 0; 5 / 5 |
| script, 500 capturing (`--capture`, no deck commands), 20 s | survived a bus reset ("restarting the stream"); 194 frames captured, **identical to tape frames 22..215**, 0 errors |
| script, `pal10min.dv`, 2 min | 2966 sent, 2950 captured, 0 errors, all identical and consecutive |
| **virtual deck** `MarvinPlayout pal20.dv`, capture `--rew --wait --play --capture x.dv --wait nosignal=00:00:05` | the 500 connected oPCR[0] (p2p), REW / PLAY / status / timecode over AV/C, capture exit 0: **499 frames = tape frames 0..498, identical**, 0 errors |
| the same with `ntsc20.dv` | 598 frames = tape frames 0..597, identical |

Still wrong or untested:

- **The last tape frame is missing** in both virtual-deck captures (499 of 500, 598 of 599); not yet
  known whether the playout or the capture's no-signal stop drops it.
- **MarvinPlayout hangs after an EP 0x06 failure** in script mode: it never exits (killed by
  `timeout` after 40–60 s, exit 124) instead of exit 2.
- IT status writebacks with the s bit (why they stall) and the FPGA's IT events are not understood;
  not needed with s off.
- Steps 2 (camcorder as receiver), 3 (replug), 6 (HDV), 7 (mixed tape), 8 (record) not done; the
  Windows MSDV / dvgrab controller of step 4 neither.
