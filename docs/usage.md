# Using it: capture, diagnostics, and proving nothing was dropped

Two ways in, both on the same core library:

- **The GUI**, `PinnacleCapture.exe` ([gui/windows/README.md](../gui/windows/README.md)):
  device list, live preview, deck control, DV/HDV/analog capture with scene
  splitting and the same integrity checks.
- **The command-line tools** in `build\dist\cli\` (see [building.md](building.md)):

| tool | what it does |
|---|---|
| `pincli` | DV / HDV capture straight from the FireWire port, to `.dv` or `.ts` |
| `pinanalog` | analog (composite / S-video) capture to AVI |
| `pindeck` | deck control: play, pause, stop, FF, REW, state, timecode |
| `pinlist` | list attached units and their state |
| `pinctl` | the session API as a CLI (capture, formats, presets); also the API's integration test |

The tools look for their FPGA bitstream in `firmware/` relative to the current
directory, so run them from `build\dist\cli` (or pass `-b`).

The device must be bound to WinUSB (or libusb-compatible) rather than the vendor
driver: see [windows-driver.md](windows-driver.md).

## Running a capture

```
pincli -o out.dv -t 300
```

| flag | meaning |
|---|---|
| `-o`, `--output` | output path (required) |
| `-b`, `--bitstream` | FPGA bitstream (default `firmware/fpga-ohci.bin`) |
| `-t`, `--duration` | stop after N seconds; without it, runs until Ctrl+C |

`-t` takes exactly the same shutdown path as Ctrl+C: stop the receive queue,
send the 4-packet stop sequence while still draining EP 0x88, then discard any
frame the stop landed in the middle of, so the file always ends on a whole
frame.

The format is detected from the stream. A DV camera gives a raw DV elementary
stream (`.dv`); an **HDV camera gives an MPEG-2 transport stream (`.ts`)**, so
name the output accordingly ([hdv.md](hdv.md)). NTSC and PAL are both handled.
Audio comes through as two `pcm_s16le` 32 kHz stereo tracks.

```
ffplay out.dv
ffmpeg -i out.dv -c:v copy -c:a copy out.avi     # rewrap, no re-encode
```

**Timing.** Bring-up takes about **5.5 s** before the first byte arrives:
bitstream upload, a 1.5 s FPGA settle, the 1394 link start-up, finding the
camera and connecting to its output plug ([startup.md](startup.md)). `-t`
counts from after that. A capture window shorter than that looks empty.

**The camera has to be transmitting.** In tape mode with the tape stopped, the
device produces zero bytes, which looks identical to a hardware fault. See
the next section.

## Telling device faults apart from "nothing is being sent"

Zero bytes on EP 0x88 has several causes. Two checks separate them.

**The FPGA did not come up.** The `05`/`06` status reads bracketing the
bitstream upload answer `<cmd> 01` when ready and `<cmd> 00` when not. This is
checked, and `pincli` aborts with

```
pincli: init failed: device reports not ready (FPGA did not come up; needs a physical USB power cycle)
```

Neither a warm reboot nor rebinding the host controller clears this, since
neither cuts VBUS. It needs a physical replug.

**The receiver is fine, the source is quiet.** `PINNACLE_PROBE=1` reads seven
OHCI registers after the start sequence:

```
probe IntEvent        (0x080) = 0x04d10030
probe SelfIDCount     (0x068) = 0x00010014
probe LinkControlSet  (0x0e0) = 0x00300600
probe NodeID          (0x0e8) = 0x8000ffc0
probe CycleTimer      (0x0f0) = 0x339dc36a
probe IR0.CtrlSet     (0x400) = 0xc0009400
probe IR0.Match       (0x410) = 0x2000003f
```

| register | this value means |
|---|---|
| `NodeID` `0x8000ffc0` | `iDValid` set, bus `0x3FF`, we are node 0 |
| `SelfIDCount` `0x00010014` | generation 1, no `selfIDError`: the bus reset completed |
| `LinkControlSet` `0x00300600` | cycle master + cycle timer enabled, receiving self-ID and PHY packets |
| `CycleTimer` non-zero, changing | the link is clocked and alive |
| `IR0.CtrlSet` `0xc0009400` | bufferFill + isochHeader; `run` (b15), `wake` (b12) and `active` (b10) set, **`dead` (b11) clear**: the receiver is healthy |
| `IR0.Match` `0x2000003f` | listening on channel 63, tag 1 |

If `IR0.CtrlSet` shows `run` and `active` with `dead` clear and there is still
no data, the receive side works and the camera is not transmitting.

## Proving no data was dropped

A 5-minute DV capture (`-t 300`) checked three independent ways: 8,967 frames
(99.73% of the 8,991 NTSC expects; the shortfall is start-up latency), 89,670
DIF sequences with none zero-filled, **0 CIP/DBC discontinuities** over
2,242,030 data blocks, and `ffmpeg -f null -` decoding it with **0 errors**.
A 5-minute HDV capture was likewise clean (7,487 pictures, 0 TS continuity
errors).

**The camera gives us no timecode in live view.** The SMPTE timecode pack is
absent, and REC DATE/TIME read "no information". That is normal (timecode comes
off tape), so timecode cannot verify continuity here, and ffmpeg prints
`Detected timecode is invalid` once. Don't re-litigate it.

What is used instead:

**CIP/DBC continuity**, reported at the end of every run:

```
pincli: continuity: OK — 335515 data blocks, no CIP/DBC discontinuity (1 at stream join, expected)
```

The IEC 61883 CIP header carries a data block counter that the *camera*
increments once per 480-byte data block, modulo 256. A jump means data was lost
somewhere between the camera's transmitter and the file. Empty rate-padding
packets are skipped (implementations disagree over whether their DBC is the
previous or the next value). The "at stream join" discontinuity is not loss:
the isochronous ring is already running when we attach, so the first read lands
mid-record and the framing layer resyncs by discarding bytes. Exactly one
always appears there, so it is counted separately. A healthy run reports `0`
real discontinuities and `1` join.

For HDV the DBC is weak (a hole of exactly 32 source packets wraps it), so the
per-PID TS continuity counters are authoritative there; see [hdv.md](hdv.md).

**An independent decode.** `ffmpeg -v error -i out.dv -f null -` should print
nothing (apart from the timecode line above); `-map 0:v:0` for `.ts`. Compare
the frame count with the wall-clock duration.

For analog, `pinanalog`'s progress line counts missing, truncated and audio-missing
frames; a clean run shows zeros and exit status 0 ([analog.md](analog.md)).

### The USB thread must never wait on the disk

Found while chasing sporadic single holes in HDV captures; it applies equally
to DV. The holes always sat ~5 s into a run (the first time the kernel's
writeback flusher fires), each following a 40-50 ms stall in the capture
callback (`fwrite` blocked on the filesystem). The type-9 ring addresses were
continuous, so nothing was lost on USB: the FPGA's OHCI receive FIFO overran
while the host was not reading, dropped whole bus cycles (~25, ~3 ms), then
resumed. The device ends each transfer after only ~4 KB, so 32 queued
transfers held only ~36 ms.

The fix is 256 queued transfers (~290 ms) and a 64 MB ring buffer drained by a
second thread that runs the reassembler and writes the file. `pincli` prints
the writer buffer's high-water mark at the end and stops with a warning if it
ever overflowed.

The reassembler concatenates type-9 payloads in arrival order and ignores their
ring addresses. That is right while addresses advance contiguously; if an
overrun ever shows up again (a message whose address goes backwards), honour
the addresses instead.

## Diagnostics

All opt-in environment variables; the defaults are the right values.

| variable | effect |
|---|---|
| `PINNACLE_PROBE=1` | dump the OHCI registers above after the start sequence |
| `PINNACLE_DEBUG_EP88=1` | log every EP 0x88 completion: size, delivery time, libusb completion time and callbacks over 1 ms. Not for loss testing: the log runs on the USB thread and adds its own stalls |
| `PINNACLE_DEBUG_EP84=1` | log every EP 0x84 record with its arrival time |
| `PINNACLE_RAW_DUMP=<path>` | write the raw EP 0x88 byte stream to a file before reassembly. This is also the input format of the replay device below |
| `PINNACLE_DEBUG_1394`, `PINNACLE_DEBUG_ANALOG`, `PINNACLE_VIDEO_QUEUE`, `PINNACLE_VIDEO_XFER` | 1394 / analog debug logging and the analog USB queue depth and transfer size |

## Two traps

**The command channel blocks while EP 0x88 has an unread backlog.** Anything
that writes to EP 0x02 while the isochronous receive context is running must
keep reading EP 0x88 at the same time, or the write times out. The stop
sequence and `PINNACLE_PROBE=1` both do.

**A non-transmitting camera is indistinguishable from a dead device** unless
you check the two things above. One investigation concluded the device had been
bricked by the stop sequence when it had not.

## Testing without hardware

Set `PIN_REPLAY=<file>` (a `.dv`, `.ts`, or a raw `PINNACLE_RAW_DUMP` file) to
get a virtual "replay" device that plays a recording back through the full
pipeline:

```
set PIN_REPLAY=tests\data\ep88-pal.bin
pinctl capture -i dv -f dv -o out --duration 5
```

The ctest suite is built on this; see [building.md](building.md).
