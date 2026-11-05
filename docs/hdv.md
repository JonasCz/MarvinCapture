# HDV capture

Status as of **2026-09-25**. HDV over the FireWire port works with the same
hardware, the same unmodified FPGA bitstream and the same start sequence as DV.
Only the host-side reassembly is different. Read
[protocol.md](protocol.md) first for the two
outer framing layers, which are shared.

## Why it works

The FPGA is a generic OHCI-1394 controller and EP 0x88 tunnels its isochronous
receive DMA. OHCI does not care what the isochronous payload is, so an HDV
camera in live mode "just arrives" on the same ring. First contact (HDV camera
in live/E-E mode, 2026-09-25) needed no change to the device side: the
blind `oPCR[0]` compare-swap and `IR0.ContextMatch` channel 63 that were captured
from a DV session connect to this camera as well.

Bandwidth is not the constraint: HDV is 19–25 Mbit/s against DV's 28.8, and the
bulk pipe carries ~3.4 MB/s of ring data either way.

## What is on the wire

Same layers, different CIP payload (IEC 61883-4, MPEG2-TS):

| | DV (IEC 61883-2) | HDV (IEC 61883-4) |
|---|---|---|
| CIP quadlet 0 | `01 78 00 <DBC>` | `01 06 c4 <DBC>` |
| CIP quadlet 1 | `80 00 <SYT>` (FMT 0x00) | `a0 00 00 00` (FMT 0x20) |
| DBS | 0x78 (480-byte data block) | 6 (24-byte data block) |
| FN / SPH | – | 3 (8 data blocks per source packet) / 1 |
| isoch payload | 1 × 480 B DIF block | 1–3 × 192 B **source packets** |
| source packet | – | 4-byte timestamp + one 188-byte TS packet |

Observed stream from the test camera: MPEG-2 Main, 1440x1080 (SAR 4:3), 25 fps,
25 Mbit/s, plus MP2 48 kHz stereo audio. PIDs: video `0x810`, audio `0x814`,
PAT `0x0`, PMT `0x81`. The DV CIP validation in `dv_reassembler.c` distinguishes
the two by the FMT field (`h[8] & 0x3f`) and, for HDV, `DBS == 6` and a payload
that is a multiple of 192 bytes; the first data packet sets the format, and it
follows a tape that changes between DV and HDV ([below](#tapes-that-mix-dv-and-hdv-or-hdv-formats)).

## What the capture does

`MarvinCaptureCLI --capture out.ts ...` detects HDV automatically; no flag. It strips the two
outer layers, drops the 4-byte source-packet timestamps and writes the 188-byte
TS packets back to back. The output plays directly (`ffplay out.ts`).

**It starts on a real boundary and ends on one.** Nothing is written until a
video picture that carries an MPEG-2 sequence header (the start of a GOP) is
seen — the ring is already running when we join, so what precedes that is a
partial picture and possibly stale ring contents. This mirrors DV waiting for
`Dseq 0`. When the gate opens, the most recently seen PAT and PMT are written
first, so the file is decodable from its first packet. Every file starts this
way: the next pass's, a scene split's (below), and one from the replay device.
On stop, the trailing partial picture is cut, so the file ends on a whole one.
(A non-seekable output, e.g. a pipe, cannot be cut and keeps the tail.)

## Format: what the stream says

The format is read from the video stream itself, picture by picture (`hdv_error.c`):
the size, `aspect_ratio_information` and `frame_rate_code` from the sequence header,
`progressive_sequence` (and the frame rate extension, 0 for HDV) from the sequence
extension, and the field order from the picture coding extension (`top_field_first` of
interlaced pictures). The status shows it (`video_label` "1080i25", "1080i29.97",
"720p59.94"; `video_interlaced`, the frame rate and the display aspect), the sinks get it
(size, rate, interlaced or progressive, field order, and the SAR from the real raster:
4:3 at 1440x1080 16:9, 1:1 at 1280x720), the scene split counts the GOP timecode at that
rate, and the replay device plays a `.ts` / `.m2t` at it. Until the first sequence
header, 1080i29.97 16:9 top field first is assumed. A raw `.ts` / `.m2t` is the stream
itself, so the format only matters to the MOV / MKV remux, which is disabled for HDV.

Not handled: progressive material inside an interlaced stream shows as the stream says
it is coded: 24p with pulldown flags (`repeat_first_field`) as 1080i29.97, frame modes
such as 25F / 30F as 1080i. Only 1080i25 has been seen from a camera; 720p59.94 is tested
with an ffmpeg-made file, 1080i29.97 with hand-built headers only.

## Scene split (`--split`)

HDV carries a timecode only in the GOP header, in the picture that opens each GOP (12
pictures at 1080i25, 15 at 1080i29.97, 6 at 720p); the recording date/time of the aux
stream is not read. A timecode break counts once **three GOPs** in a row agree on the
new timecode (about 1 s at 1080i). One or two GOPs with a wrong timecode, or GOP headers
lost in a dropout, never split: the next good GOP is compared with where the timecode
should be by then. Until a break is settled, the pictures since its first GOP are held in
memory (`src/engine/pin_holdback.h`), so the cut is back-dated exactly: the new file
starts with the PAT, the PMT and then the picture that opens the GOP where the new
timecode begins (with its sequence header), and decodes from its first packet; the
previous file ends with the picture before it. The 1 s / 1 MB minimum for a new file
([usage.md](usage.md)) counts pictures at the stream's frame rate. A camera that writes
00:00:00:00 in every GOP header gives no scene split.

Tested with an ffmpeg-made tape (a timecode jump, and a stray GOP that must not split;
`test_replay_hdv_split`), not with a camera's tape yet. Unverified: whether a camera
starts a recording with a closed GOP (or sets `broken_link`); an open GOP's leading B
pictures would not decode at the start of the new file.

## Tapes that mix DV and HDV, or HDV formats

- **DV <-> HDV.** The reassembler drops data packets of the other format until
  `DV_FMT_SWITCH_PACKETS` (256) of them arrive in a row with no packet of the current
  format between them: more than the receive ring holds (stale ring contents when a read
  joins never switch it), about one DV frame's worth or 35 ms. Shorter runs stay
  dropped (`fmt_dropped_packets`; if they were the current stream's packets with a
  corrupt header, the next one shows them as a DBC gap). On a switch the run is replayed into the new format, so nothing of it is
  lost; the old format's unit in progress ends as at the end of a capture (a complete DV
  frame is written, the HDV picture in progress dropped), and HDV starts again at its
  gate (PAT/PMT, then a picture with a sequence header; PIDs and continuity counters
  learnt afresh). The session then confirms the new kind over its own cool-down (3 GOPs,
  or 5 DV frames) and starts a new file in the other kind's format and extension from
  the first unit of the new kind ([usage.md](usage.md#running-a-capture)); on standard
  output the capture ends instead.
- **1080i <-> 720p, or another frame rate.** A change of the sequence header's size,
  frame rate or `progressive_sequence` counts after 3 GOPs in a row say it (a single
  GOP with a corrupt header changes nothing) and starts a new file at the GOP where it
  began, with PAT and PMT in front.
- **Another video PID.** The video PID is the one the gate opened on. When GOPs
  (a video PES with a sequence header) arrive on another video PID twice with no
  picture of the tracked one between them, the gate opens again on the new PID: the
  picture in progress (which ran on into the new PID) and the new PID's first GOP are
  lost, the next picture comes behind the latest PAT/PMT. A picture that grows past
  4 MB without another video PES start (the video PID went silent) is dropped and the
  gate re-armed, so memory stays bounded.

Tested with ffmpeg-made streams back to back in synthetic EP 0x88 dumps
(`test_reassembler_mixed`, `test_replay_mixed_kind`), not with a camera. Unknown: what a
camera sends when a tape switches between DV and HDV (a bus reset ends the capture as
`CAMERA_LOST`; a gap or a burst of garbage should be handled as above), and whether any
camera changes the video PID or the HDV format within a tape.

## Verifying integrity — read this before trusting the DBC line

The capture tracks two independent checks, but **they are not equally strong for
HDV**:

- **TS continuity counters (authoritative).** Per-PID 4-bit CC. Every lost
  packet shows up as a jump on the affected PIDs.
- **CIP DBC (weak).** The CIP data-block counter advances by **8 per source
  packet** and is 8 bits wide, so a hole of exactly 32 source packets
  (6,144 bytes) wraps it back to the expected value and is **invisible**. A
  60 s capture showed exactly that: video, audio and PAT CC all jumped (about
  32 packets lost) while the DBC line said "no discontinuity". For DV the DBC
  window is 256 blocks = 123 KB, which is why it was sufficient there.

Because one hole surfaces once per PID, CC jumps that follow an already-counted
DBC discontinuity are reported separately ("explained by CIP discontinuities")
rather than being double-counted as loss.

Independent check on the file:

```bash
ffmpeg -v error -i out.ts -map 0:v:0 -f null -    # a healthy file prints nothing or one join warning
```

## Capture verified on hardware

Pinnacle 510-USB with a Canon HDV camcorder (2026-10-03): `--capture` to `.ts`,
`.m2t`, `.mkv`, `.mov` and to stdout (`--capture -`) all produce files ffprobe
reads, with timecode-driven starts and stops (`--wait timecode=HH:MM:SS:FF`).

- **Timecode.** The GOP header `time_code` of this camera is always 00:00:00:00, so
  it is ignored until it shows a non-zero value; until then the deck's AV/C TIME
  CODE poll provides the timecode during capture ([deck-control.md](deck-control.md)).
  A stream with a real GOP timecode (other cameras) is used as before.
- **Timestamps.** A raw `.ts` (and the stdout stream) is a byte-exact copy, so its
  PTS starts wherever the tape's clock was (a `.ts` of a capture at tape position
  21 s starts at 21.3 s). The rewrapped `.mov` / `.mkv` are rebased to start at 0
  (`sink_rewrap.c`, all streams moved by the earliest start).
- **File names.** `.m2t` is kept for a raw capture (the format is the same as `.ts`).
- **Startup noise.** The preview decoder is not fed before the first MPEG-2 sequence
  header any more; FFmpeg's "Invalid frame dimensions 0x0" at the start of every
  capture is gone (and demoted to debug if it still appears).

## Known behaviour

- **A clean 5-minute run exists**: 7,487 pictures, 0 CC errors, 0 DBC discontinuities, ffmpeg decodes it
  with no messages at all.
- **Earlier sporadic holes were a host-side stall, now fixed.** Early runs
  occasionally had one hole of ~25 bus cycles (~3 ms, a damaged picture)
  about 5 s in. Cause: a filesystem flush blocked the thread that keeps the
  USB queue full, the FPGA's receive FIFO overran and dropped bus cycles. Not
  the camera, not the bus. the capture now decouples disk writes from USB reads;
  see [usage.md](usage.md#the-usb-thread-must-never-wait-on-the-disk)
  for the evidence and the A/B. A hole, if one ever occurs, is kept as received
  and only counted (TS has no fixed-size unit to zero-pad, unlike DV).
- **Live mode only.** As with DV, this receives whatever the camera transmits.
  Tape transport control is separate: see [deck-control.md](deck-control.md).
- Not tested on hardware: 720p HDV, S400 cameras, 1080i/29.97 (only 1080i/25). The
  format detection is tested with ffmpeg-made 1080i25 and 720p59.94 streams.
