# Using it: capture, diagnostics, and proving nothing was dropped

Two ways in, both on the same core library:

- **The GUI**, `MarvinCaptureGUI.exe` ([gui/windows/README.md](../gui/windows/README.md)):
  device list, live preview, deck control, DV/HDV/analog capture with scene
  splitting and the same integrity checks. It remembers its window position
  and size, and keeps a separate set of settings (output folders and names,
  formats, passes, idle/duration limits, aspect, standard, picture controls)
  for each camera/capture unit, keyed by the unit's FireWire GUID, so switching
  device in the list loads that unit's own options (defaults for a new one). It asks before capturing when the output drive has under 25 GB
  free. During a capture the deck buttons are disabled; "Manual capture"
  records without touching the tape and "Automatic rewind & capture" drives
  the deck (their stop labels: "Stop capture & continue tape" / "Stop capture
  & stop tape"). While capturing both stop buttons are available, however the
  capture was started: "continue tape" never sends deck Stop, "stop tape"
  always does.
  When the camera stops sending (end of tape, blank tape) the preview shows
  "No camera or deck signal" and, if the "Stop no signal (min)" option is on,
  "Stopping capture in 5m30s" with a bar running down; the same time is
  appended to the stop button. At zero the capture stops and, for "Automatic
  rewind & capture", so does the tape. With several passes the tape is
  rewound instead and the next pass starts from the beginning. The "Stop
  after (min)" limit is per pass and counts only capture time (not the rewind);
  it ends a pass like the no-signal timeout, and in the last pass it ends the
  capture and stops the tape. "Capture passes" is greyed out (fixed at 1, with a
  tooltip saying why) while neither "Stop no signal" nor "Stop after" is set,
  because multi-pass needs one of them to detect the end of a pass; with only
  "Stop after" set, each pass is stopped and rewound after that time. The
  value you chose comes back when a limit is set again. The core enforces the
  same rule (`pin_capture_passes_allowed`, `pin_capture_opts_normalize`), so
  `--passes 2` without `--idle-min` / a duration limit also captures once.
  How this works inside: [deck-control.md](deck-control.md#capture-flow-in-the-session-engine).
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

**Scene splitting (`--split`, DV/HDV).** A confirmed timecode / recording-date
break starts a new file (`name-0001`, `name-0002`, ...). A new file is only
created once the new segment is a real one, i.e. it has reached **at least
1 s and at least 1 MB**; the frames are held in memory until then. If the
capture, the pass or the tape ends first, or another break arrives first, the
held frames are appended to the previous file instead, so a few odd frames
with a weird timecode at the end of a scene or tape don't leave a tiny file
behind (the very first file is always created). Only content-triggered splits
are deferred this way; a hard size limit would not be. Details:
`src/engine/pin_split.h`.

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

### Frames with error (live counters)

The status snapshot (`pin_get_status`, see `pin_api.h`) carries error counters
next to the frame counts, as a total (since the capture started, or since the
session started while not capturing) and for the current clip (restarts with
every output file): `frames`/`frames_error`/`frames_dropped` and
`clip_frames`/`clip_frames_error`/`clip_frames_dropped`. A frame counts as "with
error" when:

- **DV**: a DIF block is missing or its ID does not match its position (a lost
  sequence is zero-padded, so it shows up here), a video block has a non-zero
  STA status nibble (7/15 = error, 2/4/6/10/12/14 = concealed by the camera),
  an audio block holds the constant error fill (0x8000 and vendor values), the
  first channel pair is partially all-zero (a Sony deck mutes by writing zeros;
  an all-zero or unused pair is silence, not an error), or ~every video block
  is STA 14 (a Samsung camera that re-encoded the frame). A frame with >= 90 %
  of its blocks missing is also "dropped".
- **HDV**: transport_error_indicator, a continuity-counter gap on any PID, a
  video PES that does not start properly or has no picture header. A B or P
  picture that depends on a damaged I/P picture of the same GOP counts as damaged
  too (a damaged I/P damages the P pictures after it up to the next I, and the
  B pictures that use it). An audio-only gap marks that picture but does not
  spread. A stream break (rewind, pause) restarts the tracking instead of
  counting as loss.
- **Analog**: a repeated (dropped) frame.

Verified against the repo's fixtures only (clean camera frames report zero
errors, injected damage is detected); the live-camera behaviour of the quirk
heuristics is untested here. The clip counters count a unit when it arrives, so
frames held back by a pending split lookahead (about a second) land in the
previous clip.

### Sizes and time left (live)

The same status snapshot reports `clip_bytes_written` (the current file),
`total_bytes_written` (every file of this capture), `disk_free_bytes` and
`est_seconds_left` (free space / data rate; divide by 3600 for hours). The rate
is, in order: the **measured** rate of the running capture (average of the last
10 minutes, from 10 s of data on), for analog FFV1 the rate **learned** from the
previous capture, else the **nominal** one: DV and HDV 13 GB/h (about 25 Mbit/s),
analog AVI from what the core writes (YUY2 720x576 at 25 fps or 720x480 at
29.97 fps plus 48 kHz 16-bit stereo, about 21 MB/s), FFV1 30 GB/h.
`est_rate_source` says which (0 / 1 / 2) and `est_bytes_per_hour` is the rate.
An FFV1 capture saves its 10-minute average every minute and when it ends as
`ffv1_bytes_per_hour` in the `[core]` section of the settings file
(`core.ffv1_bytes_per_hour` for `pin_settings_get/set`); implausible values
(< 1 GB/h, > 300 GB/h) are ignored.

### When a capture stops by itself

Every end of a capture is reported once, after its files are closed, as
`PIN_EVT_CAPTURE_ENDED` (`a` = `pin_stop_reason_t`, text = one sentence such as
"Capture stopped after capturing 12m30s, because the output drive is almost
full (60 MB left).") and in the status (`stop_reason`, `stop_captured_s`,
`stop_text`, kept until the next capture starts). The captured time is the
frame count of all passes, not the wall time.

`pin_stop_reason_abnormal()` splits the reasons. **Abnormal** (device or camera
gone, disk full, write error, any error): the GUI shows the sentence in a
dialog with an OK button, and `pinctl capture` exits with 2. **Normal** (the
user's stop, the no-signal timeout, the time limit, end of tape): no dialog;
the GUI shows the sentence in the status bar instead of "Ready" until the next
capture starts (not for the user's own stop).

| reason | when |
|---|---|
| `NO_SIGNAL` / `TIME_LIMIT` | the no-signal timeout or the time limit, in the last pass |
| `END_OF_TAPE` | replay device: the end of the file in the last pass |
| `DEVICE_LOST` | the USB stream failed and the unit no longer answers a standard GET_STATUS on EP0 (unplugged); the session goes to ERROR. Another USB failure is `ERROR` |
| `CAMERA_LOST` | DV / HDV: the FireWire bus was reset while the capture was under way (cable moved, camera switched off): frames were lost, so it stops rather than carry on in the same file |
| `DISK_FULL` | free space on the output drive fell below what finishing the file needs: 64 MB, plus what the disk writer still has queued, plus (HDV to MOV / MKV only) the size of the file, because those are remuxed from a temp `.ts` when closed. Checked once a second |
| `WRITE_ERROR` | writing a file failed, or the next file (split, pass) could not be created |

A capture that drives the deck (`start_deck`) stops the tape when it ends for
any reason except a lost device or camera. `PINNACLE_DISK_RESERVE_MB=<n>`
replaces the 64 MB margin; set it to the drive's free space minus a few MB to
test the disk-full stop (verified that way with the replay device).

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
| `PINNACLE_DEBUG_1394`, `PINNACLE_VIDEO_QUEUE`, `PINNACLE_VIDEO_XFER` | 1394 debug logging, and the analog USB queue depth and transfer size |
| `PINNACLE_DISK_RESERVE_MB=<n>` | free space (MB) a capture keeps on the output drive beyond the writer queue before it stops with "disk full" (default 64) |

These are read by the CLIs and by the capture engine (so the GUI honours
them too). Started from a console, the GUI also prints its log there;
`PINNACLE_LOG_LEVEL=0` adds the core's debug lines.

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
