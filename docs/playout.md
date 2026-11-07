# MarvinPlayout: playing DV and HDV files out over FireWire

`MarvinPlayout` plays `.dv` (DV) or `.m2t`/`.ts` (HDV) files out of the FireWire port of the
Pinnacle device, as a virtual tape deck. It is a separate command-line program installed next to
`MarvinCaptureCLI` and uses the same left-to-right syntax and wait conditions (see [cli.md](cli.md)).

**Status: nothing on the device side has run yet.** The packetizer, the virtual tape and deck, the AV/C
target, the script and the whole playout loop are unit-tested without a device, and `--dry-run`
works. The code that talks to the Marvin (transmit on EP 0x06, the 1394 plug registers, the IRM) is
written from the decompiled vendor driver ([handoff/playout-feasibility.md](handoff/playout-feasibility.md))
and has never been tried. Every assumption is marked `GUESS:` in the source; the test plan is at the
end of this page.

## What it does

- The files you name are one **tape**, in the order given; each file is one recording and keeps its
  own timecode (a file without one continues the previous file's). All files must be the same kind
  (DV PAL, DV NTSC, or HDV with the same picture size and rate).
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
7. Recording to tape: AV/C RECORD to the camcorder while playing ("export to tape").
