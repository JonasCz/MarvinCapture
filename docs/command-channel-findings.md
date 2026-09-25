# Command-channel handshake — findings (2026-09-22)

Read [../HANDOFF.md](../HANDOFF.md) first for background.

Status: capture, clean stop, and multi-minute stability all work. Since
2026-09-25 the 1394 start-up is no longer a replay. It is generated step by
step, the camera's node and channel are discovered, and a real oPCR
connection is made: see [startup.md](startup.md), which supersedes the
start-sequence details below.

## Resolved: the command-channel wedge

**Symptom**: after FPGA bitstream upload + alt setting 1, the device accepted
exactly 2 bulk-OUT writes on EP 0x02 (the command channel), then NAKed
(`LIBUSB_ERROR_TIMEOUT`) every subsequent write indefinitely. Deterministic,
independent of write content, timing, read strategy, or endpoint-halt
clearing (see git history for the full ruled-out list from the initial
investigation).

**Root cause**: the low-level config channel (EP 0x01 OUT / 0x81 IN,
previously assumed "unused for DV") carries a firmware bring-up sequence that
the driver had never replayed. Disassembling `MarvinBus64.sys` (extracted
locally from the user's own driver install, never redistributed — see
licensing note below) surfaced a large table of named hardware registers
("Throttle Async TX/RX", "ASync/Isoch rx/tx fw Config", "USB ep flush",
etc.), which pointed at this channel as more than just the analog decoder's
I2C bus. Re-examining the existing authentic cold-boot capture
(`traces/20260922-142720-coldboot-driver-init.pcapng`) with that in mind
turned up 80 request/reply exchanges on EP 0x01/0x81 between `SET_INTERFACE`
alt 0 and the start of the bitstream upload, plus one more right after the
bitstream and before alt 1 — all previously skipped entirely.

Two of these are simple status reads (`05 00`→`05 01` before the bitstream,
`06 00`→`06 01` after). Replaying just those two in isolation still failed:
the device answered but returned "not ready" (`05 00`, `06 00`) instead of
"ready". Replaying the **full** 80-entry sequence — mostly SAA7113
analog-decoder I2C register writes, plus two 20-byte EEPROM/serial reads —
flips both status reads to "ready", and the command channel then accepts the
full 232-packet start sequence without wedging.

This is now implemented in `pinnacle_device.c`
(`PINNACLE_CONFIG_PREBITSTREAM_SEQ` / `_POSTBITSTREAM_SEQ` in
`protocol_data.h`) and confirmed working: `pincli` reaches "streaming
started" and writes real, correctly-structured 720x480 NTSC DV data (valid
DIF headers, ffprobe recognizes it as `dvvideo`/`yuv411p`).

**Licensing note**: `MarvinBus64.sys` was extracted from the user's own
driver install and analyzed locally (Ghidra headless + radare2, both
installed on the Ubuntu host for this), never redistributed — same policy as
the FPGA bitstream.

## Resolved: the device is an OHCI-1394 controller, and EP 0x88 is not raw DV

This is the big one. Disassembling `MarvinBus64.sys` properly (Ghidra
headless decompile of all 608 functions, plus parsing the PE's `.data`
tables) showed the driver carries **two** complete hardware back-ends,
selected at runtime from a 47-entry op table at VA `0x4df50`. Our device
uses the second one, which is a **standard OHCI-1394 host controller
implemented in the FPGA**. The FPGA register table in the binary's strings
("Isoch rx timeout", "Event mask", "Throttle Isoch RX", ...) belongs to the
*other* back-end and is dead code on this hardware — an earlier read of
those strings sent this investigation down a blind alley.

Confirmation is byte-exact: `FUN_0002cd00` contains a literal 13-entry
{register, value} init table
(`{0xe4,0xffffffff},{0x104,0xffffffff},...,{0x840,0x10000020}`) that is,
in order, exactly one of the packets we already replay.

### The command word

Everything on EP 0x02 / EP 0x84 / EP 0x88 is a stream of messages sharing
one little-endian `u32` header:

```
bits 31:28  type
bit  27     reply wanted; bits 26:20 = 7-bit tag echoed in the reply
bits 19:0   register address      (register messages)
bits 26:16  payload byte length   (payload messages)
bits 15:0   device RAM address    (payload messages)
```

| type | meaning |
|------|---------|
| 2 | OHCI register write — address is `0x10000 + reg_offset`, value follows |
| 3 | OHCI register read — same addressing, reply carries the value |
| 4 | FPGA USB-side register write (indexed; the driver keeps a shadow copy). Index 0 = EP 0x84 idle time / isochronous-to-USB gate; see [startup.md](startup.md). |
| 5 | read of that bank (reply comes back as type 5) |
| 6 | FPGA extension-bank write (OHCI-style offsets, separate register file) |
| 8 | block write to device RAM (`len` bytes follow) |
| 9 | block read / device→host data (`len` bytes follow) |
| 10 | OHCI `IntEvent` report |

So the sequences we had been replaying blind are readable now. For example
`00 04 01 20 | 00 90 00 00` is "write OHCI `0x400`
(IsoRecvContextControlSet) = `0x9000`", i.e. `run|wake` — literally *start
isochronous receive*. The stop sequence's `04 04 01 20 | 00 80 00 00`
clears `run`. `0c 04 01 20 | 81 11 80 00` sets `IsoRecvCommandPtr` to
descriptor `0x801180` with Z=1.

### Isochronous receive is a closed 2-descriptor ring

`REQUEST_ISOCH_LISTEN` builds two descriptors that both fill the same
36,864-byte buffer, with the last branching back to the first:

```
desc @0x801180 = 280C9000 00807000 00801191 00009000
desc @0x801190 = 280C9000 00807000 00801181 00009000   <- branches back
0x40C IR0.CmdPtr  = 0x00801181
0x400 IR0.CtrlSet = 0x00009000   (run|wake)
```

It is a "listen forever" switch — the host never re-attaches buffers. That
explains the earlier observation that the vendor driver sends **zero**
commands during steady streaming. There is no keepalive, no timer, and no
watchdog anywhere in the binary (`KeSetTimer` does not appear at all).

## Resolved: video corruption — EP 0x88 has two layers of framing

The reassembler was treating EP 0x88 as raw DV with occasional wrapper
bytes. It is not. It carries the *same* type-9 message framing as the
command channel, wrapping the OHCI isochronous-receive DMA ring:

1. **Type-9 messages**: `u32 = (9<<28) | flags | (len<<16) | ram_addr`,
   then `len` bytes. `ram_addr` `0x7000`–`0xFFFF` is the isoch ring;
   `0x118C` / `0x119C` are IR descriptor status writebacks carrying no
   stream data. **Message lengths vary and are not aligned to anything in
   the payload**, so a 4-byte header routinely lands in the middle of a DIF
   block. That is the whole corruption story.
2. **Inside the ring**, OHCI buffer-fill records:
   `[isoch header u32][payload, quadlet-padded][trailer u32]`, where the
   isoch header is `dataLength<<16 | tag<<14 | chan<<8 | tcode<<4 | sy` and
   the payload is an IEC 61883 packet: 8-byte CIP header
   (`01 78 00 <DBC> 80 00 <SYT>`, DBS=0x78=480 bytes, FMT=0 DVCR) followed
   by 480 bytes of DIF — or nothing at all for the empty packets DV sends
   to pad its rate to the 8 kHz cycle clock.

Stripping both layers yields a byte-exact DIF stream. Verified over a
16 MB slice of a live capture: **1306 sequence headers, every stride
exactly 12,000 bytes, 0/195,750 wrong block SCTs, 0 wrong Dseq, 0 wrong
DBN, 0 video blocks with a non-zero STA**, and every packet's OHCI
`xferStatus` is `0x8411` (`run|active|ack_complete`) — no overruns, no
errors. End to end, a 128 MB / 1073-frame capture decodes with **zero**
ffmpeg errors, versus tens of thousands before.

This is implemented in `dv_reassembler.c` as three stacked layers
(message demux → buffer-fill records → DIF sequence/frame assembly), each
resynchronising independently.

## Resolved: "doesn't sustain"

Not reproducible. The ~240 ms cutoff documented below does not occur any
more: streaming now runs for as long as it is asked to — **140.8 seconds
continuous, 542 MB, 4225 frames, with no inter-arrival gap anywhere in the
run exceeding 50 ms**, and routinely 35 s+ in shorter tests.

Being honest about causation: an A/B test (`PINNACLE_EP84_DRAIN=0/1`, four
alternating 30-second runs) showed the EP 0x84 drain thread added during
this work is **not** what fixed it — runs sustained the full window with
the drain disabled too. The most likely explanation is that the earlier
240 ms measurements were taken with the device (or the camera) in a state
it has since been power-cycled out of. The drain is kept anyway because it
is what the vendor driver does and costs nothing.

Note also that the earlier "~240 ms then silence" figure was partly a
measurement artifact: several of those runs used a capture window barely
longer than the ~9 s device bring-up, so what looked like a cutoff was
sometimes just the end of the window.

## Historical: the original corruption and sustain investigation

Kept because the methodology notes — especially the usbmon caveat — still
apply. The conclusions here have been superseded by the sections above.

Both were tracked down with direct instrumentation rather than guessing (see
methodology below). They turned out to be two separate, unrelated problems.

### 1. Streaming stops after ~240ms — confirmed device-side, not a host bug

Initial usbmon captures of our own `pincli` runs (`pincap`, the same tool
used for the vendor-driver traces) showed **zero bytes ever received on EP
0x88**, which briefly looked like a device silently going idle. That reading
was wrong: it's a capture artifact. Our code reuses one malloc'd buffer for
every synchronous `libusb_bulk_transfer` call in a tight ~1-2ms poll loop,
and usbmon's snapshot of that buffer races with our own reuse of it —
confirmed because `pincli`'s own byte counters (read directly from
`libusb_bulk_transfer`'s `transferred` output, not from any capture) showed
hundreds of KB arriving in the same window usbmon reported as empty, and the
written `.dv` file contains genuine non-repeating structured content. Lesson:
usbmon is reliable for the vendor driver (going through VirtualBox's own
buffer pool) but not for our own high-frequency same-buffer read loop.

Bypassing usbmon, `pinnacle_stream.c` gained an opt-in diagnostic
(`PINNACLE_DEBUG_EP88=1`) that logs every non-zero EP 0x88 completion's size
and arrival time directly from libusb. That showed the real picture:
**continuous, full-rate transfers (~3700-8100 bytes every ~2ms, matching the
vendor driver's own observed rate) for almost exactly 240ms, then complete
silence** — not a single further byte — for the rest of the capture window,
until Ctrl+C. This is deterministic and repeats across runs (~900KB per run,
consistently ~7 frames).

**Conclusion: the device itself stops sending after this fixed burst.** It
is not a host read-loop slowness/backpressure problem (USB bulk is
NAK-flow-controlled and lossless; a slow host just gets NAKed, it doesn't
lose data or drive the device silent). The real driver's own traces confirm
the device streams with **zero ongoing host-side commands** once told to
start (checked: `20260922-141844-ntsc-camera-playing.pcapng`, 16s, 46.9MB on
EP 0x88, literally 0 bytes on any other endpoint the entire time) — so
whatever makes it keep going is a device-internal state that our
`PINNACLE_STREAM_START_SEQ` isn't fully setting up. The tail 11 packets of
that sequence come from `stream-start-transition-TRIMMED.pcapng`, a trace
that (per the existing comment in `protocol_data.h`) was captured **8+
seconds into an already-running WinDV session** — i.e. it shows a
already-streaming device reacting to some other event, not a device being
told "stream forever" for the first time. Most likely we're replaying
whatever that mid-session event was (plausibly a one-shot "flush/replay
buffered frames" trigger) rather than the real "become isochronous cycle
master, stream indefinitely" command, which no capture we have shows in
isolation.

### 2. Video content corruption — root-caused to un-stripped packet framing

Byte-level analysis (not just structural spot-checks) of the raw EP 0x88
stream — captured with a new opt-in `PINNACLE_RAW_DUMP=<path>` hook in
`pinnacle_stream.c` that writes bytes exactly as received, bypassing the
reassembler — shows the DIF block-ID grid (`upper nibble of each 80-byte
block's ID0`: 1=header, 3=subcode, 5=VAUX, 7=audio, 9=video) walks correctly
for the header, both subcode blocks, and the first two VAUX blocks (bytes
0-399, blocks 0-4), then goes to what looks like random garbage from block 5
onward — **at exactly the same 400-byte offset in every single sequence
checked** (15/15 in one run). That precision (identical offset every time)
rules out random loss/corruption; it's structural.

Walking the raw bytes forward and re-locating each expected block ID
confirms: there's a **small (~4-30 byte) chunk of non-DIF data inserted
before block 5, and then again before several more blocks through the rest
of the sequence** — not one wrapper between sequences, but recurring small
insertions roughly every few blocks throughout. `dv_reassembler.c` has no
idea these exist; it grabs a naive contiguous 12,000-byte window starting at
a found header, so from the first insertion onward every subsequent byte is
shifted relative to the true DIF grid, which is exactly what "AC EOB marker
absent on nearly every macroblock" / pure-noise frames looks like once fed
to a DV decoder — not damaged video, *misaligned* video.

This is consistent with (and gives a much more precise shape to) the
existing "~850-byte inter-sequence wrapper" note from `linux-capture-setup.md`
— that number was measured only at sequence boundaries; it's now clear the
same kind of overhead recurs *inside* a sequence too, consistent with
per-1394-isochronous-packet framing in the tunneled bulk stream (MarvinBus64
is a full virtual-1394-bus driver — this is exactly the shape CIP-style
packet headers would take if a chunk of DIF blocks is delivered per
underlying isochronous packet). Reliably stripping it requires decoding the
insertion's own structure (probably a small quadlet-aligned header, possibly
carrying a timestamp/sequence count), not just guessing a fixed size — video
block content and header/subcode/VAUX content can share the same nibble
values, so unambiguous detection has to lean on additional structure in the
inserted bytes themselves, not just gap-searching.

### Stop-sequence wedge — RESOLVED, fix enabled by default

The stop sequence used to fail on packet 3 of 4 (`rc=-7`, the bulk-OUT write
itself times out). Packet 3 is an OHCI register read of `0x404`; packet 2 has
just cleared `run` on the isochronous receive context.

Cause: **the command channel stops accepting writes whenever EP 0x88 has a
backlog nobody is reading.** We tore the read loop down before sending the
stop sequence, so the DV still in flight had nowhere to go.

This was confirmed a second, independent way on 2026-09-22. `PINNACLE_PROBE=1`
issues seven OHCI register reads straight after the start sequence. With the
camera idle (no DV flowing) all seven succeed. With the camera playing, every
one fails — same endpoint, same `LIBUSB_ERROR_TIMEOUT`, and the only variable
is whether DV is piling up. Both the probe and the stop sequence now keep EP
0x88 drained, and both complete.

**The earlier "completing the stop sequence bricks the device" theory was
wrong.** What actually happened: after that run the camera was left not
transmitting, and a non-transmitting camera looks *exactly* like a wedged
device from the host — zero bytes on EP 0x88, with nothing to distinguish the
two. Two things now tell them apart:

- the `05`/`06` status reads bracketing the bitstream upload are checked, so a
  device whose FPGA did not come up reports `PINNACLE_ERR_NOT_READY` with an
  explicit "needs a physical power cycle" message instead of silently
  proceeding;
- `PINNACLE_PROBE=1` reads back `IR0.ContextControl`. If it shows `run` set,
  `active` set and `dead` clear, our receiver is healthy and the source is
  quiet.

Verified after a fresh replug: back-to-back captures, both with the stop
sequence running to completion, both streaming normally afterwards. The drain
is on by default; `PINNACLE_STOP_DRAIN=0` restores the old truncated stop.

### Losing ~0.05% of data blocks — fixed with a queued receive path

With one synchronous `libusb_bulk_transfer()` at a time, the endpoint is
unserviced between the return of one transfer and the submission of the next.
At 3.6 MB/s with 32 KB reads that is a gap roughly every 9 ms; the FX2's FIFO
backs up and data is lost. The vendor driver never does this — it keeps ~80
URBs (`NumberOfIsochBuffers`, a registry tunable) outstanding.

The read loop is now a ring of 32 asynchronous 32 KB transfers (~1 MB, ~290 ms
of slack), each resubmitted from its own completion callback, and consumed
strictly in submission order so the byte stream stays exact. The EP 0x84
status drain rides the same event loop, so only one thread ever calls
`libusb_handle_events()`.

A/B over 30 s each, same camera and source material (`PINNACLE_QUEUE_DEPTH=1`
reproduces the old behaviour):

| | depth 1 (old) | depth 32 (new) |
|---|---|---|
| DIF sequences written | 8,718 | 8,878 |
| zero-filled (lost) sequences | 12 | 2 (both the trailing partial frame) |
| CIP/DBC discontinuities | 2 | 1 |
| data blocks lost | 77 (0.0353%) | 1 (0.0005%) |

### Verifying that nothing was dropped

The camera provides **no timecode in live/E-E mode**. Checked directly with
`tools/dvcheck.py --dump-packs`: it emits VAUX packs 0x60, 0x61, 0x70 and
0x7F, while 0x62/0x63 (REC DATE / REC TIME) are all `0xFF` ("no information"),
the SMPTE timecode pack 0x13 is absent entirely, and there is no subcode data
at all. That is expected — subcode and timecode come off tape, and there is no
tape playing.

So continuity is established two other ways, both implemented:

1. **CIP/DBC continuity** (`dv_reassembler.c`, reported by `pincli`). The
   IEC 61883 CIP header carries a data block continuity counter that the
   *camera* increments once per 480-byte data block, modulo 256, across the
   whole connection. A jump means data was lost in transit — by the camera,
   the 1394 bus, the FPGA, USB or us. It is finer-grained than a frame and
   depends on no camera feature. Empty (CIP-only) rate-padding packets are
   skipped, because implementations disagree over whether their DBC carries
   the previous or the next value.
2. **Structural check** (`tools/dvcheck.py`). Verifies every DIF sequence is
   where it should be and that each of the 150 blocks per sequence carries the
   section type and DBN the DV spec requires, then cross-checks the frame
   count against wall-clock duration.

The two agree: on the 30 s depth-32 capture, DBC reported 1 lost block and
`dvcheck` reported zero bad headers, zero bad section types, zero bad DBNs and
no mid-capture zero-filled sequences.

Captures used to end with a zero-padded partial frame whenever the stop landed
mid-frame. The reassembler now truncates that partial frame instead, so the
file is always a whole number of complete frames.

## Diagnostics added (opt-in, no effect unless set)

- `PINNACLE_DEBUG_1394=1|2`: logs the generated start-up. That covers
  vendor status, NodeID/SelfIDCount, and camera and oPCR values. Level 2
  adds every EP 0x84 record and our config ROM.
- `PINNACLE_DEBUG_EP88=1` — logs every non-zero EP 0x88 read's size and
  arrival time to stderr. Use this instead of usbmon to characterise our own
  process's receive pattern (see race explanation above).
- `PINNACLE_RAW_DUMP=<path>` — writes the raw incoming EP 0x88 byte stream
  to a file, unmodified, before the reassembler touches it. Use this to
  study the true on-wire framing instead of the reassembler's output.
- `PINNACLE_DEBUG_EP84=1` — logs every EP 0x84 record with its arrival time.
- `PINNACLE_EP84_DRAIN=0` — disables the EP 0x84 drain (A/B only).
- `PINNACLE_STOP_DRAIN=0` — stops draining EP 0x88 during the stop sequence,
  restoring the old behaviour where the sequence fails on packet 3 of 4. The
  drain is on by default; this is for A/B only.
- `PINNACLE_QUEUE_DEPTH=<n>` — EP 0x88 transfers kept in flight (default 32,
  max 32). `1` reproduces the old synchronous read loop and its data loss.
- `PINNACLE_PROBE=1` — after the start sequence, reads `IntEvent`,
  `SelfIDCount`, `LinkControlSet`, `NodeID`, `CycleTimer`,
  `IR0.ContextControl` and `IR0.ContextMatch` and prints them. The fastest
  way to tell "our receiver is broken" from "the camera stopped sending":
  if `IR0.ContextControl` still reads back with `run` set and no `dead`
  bit, the receive side is healthy and the source has gone quiet.

## Next steps

1. ~~Replace the blind replay of the connection-management step with a real
   transaction.~~ **Done (2026-09-25).** The camera is found by reading each
   node's oMPR. The oPCR is read and then compare-swapped, IR0 listens on
   the channel the plug reports, and the connection is released on stop.
   See [startup.md](startup.md).
2. Decode the EP 0x84 event stream properly. Type-10 messages carry the
   OHCI `IntEvent` register:

   | bit | event |
   |---|---|
   | 17 | selfIDComplete |
   | 18 | busReset |
   | 23 | cycleLost |
   | 24 | cycleInconsistent |
   | 26 | cycleTooLong |

   That would turn most future failures into a printed reason instead of a
   bisect. Handling a bus reset *during* capture belongs here too: the
   connection must be re-established within 1 s.
3. Audio: `ffprobe` reports two `pcm_s16le` 32 kHz stereo streams in the
   captured DV, and they survive a full decode of a 5-minute file. So audio
   is coming through; it just hasn't been listened to yet.
4. ~~Deck control~~ **Done.** See [deck-control.md](deck-control.md).

### Not worth doing

- **Timecode-based drop detection.** The camera emits no timecode in live
  view — verified directly, see above. CIP/DBC continuity already covers it
  at finer granularity. Revisit only if capture from *tape* is ever added,
  where a real timecode exists.
- **Hunting the last isochronous resync.** Runs report a small number of
  layer-2 resyncs (3 to 123, not correlated with duration). They are ring and
  descriptor boundaries, not loss: the same runs report zero DBC
  discontinuities and `dvcheck` finds no structural damage.
