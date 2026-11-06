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
itself, so the format only matters to MOV / MKV (below).

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
00:00:00:00 in every GOP header gives no timecode split.

The camera's clock is a break too: the DTS of the video PES that opens each GOP
(`hdv_error.c`, `pin_scene.c`) must be where the pictures fed since put it, within 1 s
either way, with the same three-GOP debounce. That catches a camera that paused and
resumed with the timecode running on. A dropout that lost pictures moves the clock and
the pictures on together, so it is no break; the 33-bit wrap is no break either.

MOV and MKV always split (`pin_format_info_t.scene_split_forced`; the GUIs show the
option checked and disabled, `pin_format_scene_split_help()` says why): their timing is
made new from 0 and must not run across another recording.

Tested with an ffmpeg-made tape (a timecode jump, and a stray GOP that must not split;
`test_replay_hdv_split`), not with a camera's tape yet. Unverified: whether a camera
starts a recording with a closed GOP (or sets `broken_link`); an open GOP's leading B
pictures would not decode at the start of the new file.

## MOV and MKV: written live, damaged GOPs repaired

`PIN_FMT_HDV_MOV` / `PIN_FMT_HDV_MKV` (`--format hdv-mov`, `hdv-mkv`, or a `.mov` /
`.mkv` name) are written by `src/sinks/sink_hdvmux.c` while the tape plays: nothing is
remuxed when the capture stops, and there is no temporary file. The aim is a file an
editor such as DaVinci Resolve takes without complaint even from a tape with dropouts:
before this, the way to get one was to re-encode the whole capture (to ProRes, say);
this re-encodes only the damaged parts. Desktop only: Android does not offer them.

**Threads.** The capture's writer thread demuxes: it reads the PAT and PMT, puts the
PES packets back together, cuts the video into pictures at their start codes and GOPs
(at a GOP or sequence header, or an I picture), and the audio into MP2 frames. A
continuity-counter gap, a transport error bit or a lost sync byte marks the PES being
assembled as damaged. The GOPs go to a **decode thread**, which decodes every picture
with FFmpeg's MPEG-2 decoder (one decoder for the whole file, so open GOPs have their
references), decides about each GOP once the decoder has given back all of its frames
(a frame of a later GOP came out), and writes the file in order. Repairs go to 1-3
**encode workers**.

**Passthrough.** A GOP is copied bit for bit if it starts with an I picture, its
`temporal_reference`s are 0..n-1, no packet of it was lost or flagged, the camera clock
shows no pictures missing in front of it, and the decoder gave back every picture
without concealing anything (`decode_error_flags`, `AV_FRAME_FLAG_CORRUPT`). A clean tape
gives a file whose MPEG-2 is the tape's. The packet checks stay next to the decoder's:
they cost nothing and catch damage that still decodes as valid codes (a lost packet the
decoder happens to read through); the decoder catches what the packets do not show (bits
flipped on the tape, a picture cut short at the end of a capture).

**Repair.** Damaged GOPs in a row make a repair. The decode thread hands their decoded
pictures, error concealment and all, straight to an encode worker, which encodes them
again with FFmpeg's MPEG-2 encoder: closed GOPs with the source's size, frame rate,
aspect, field order, bit rate, VBV size, profile and level, `intra_dc_precision`,
quantiser type, intra VLC and scan. Each picture lands on the frame its PES PTS says; a
frame no picture landed on (lost in the dropout) repeats its neighbour, so the file keeps
the tape's length and the audio its sync. The GOP after a repair: its leading B pictures
refer to the repair's last picture; if it is open (whether cameras close their GOPs is
not known), those B frames are encoded into the repair too and the GOP is made closed (the
B pictures dropped, the `temporal_reference`s moved down, `closed_gop` set), so the repair
ends at the next good GOP's I picture and goes no further. A repair longer than 2 s is
handed over in parts.

**The start of a file** is always a whole frame that refers to nothing: the first GOP's
leading B pictures (which refer to a picture of the previous file or of nothing) are left
out and the GOP made closed, so the first frame is its I picture. The same after a jump of
the camera clock. A first GOP that is damaged is re-encoded as closed GOPs, which start
with an I frame too.

**Keeping up.** If more than 64 MB of GOPs wait for the decode thread, more than 6 s of
decoded frames wait for an encoder, or more than 160 MB wait in front of the muxer behind
a repair, the computer cannot keep up: the capture stops with an error
(`reencode_behind`, "checking and repairing the HDV stream could not keep up"); use `.m2t`
for such a tape.

**Timestamps.** The video counts frames from 0, the audio samples from 0, neither ever
taken from the tape. The camera clock only says where things go: an audio frame goes where
its PTS falls on the video, through the GOP it belongs to; missing audio (lost, damaged,
or not there yet at the start or the end) is MP2 silence, made once with FFmpeg's MP2
encoder; audio that would overlap is dropped. A PES with a gap keeps only the frames in
front of it, and the next intact PES brings the audio back. A jump of the clock that does
not split the file (a scene shorter than the split's minimum) is taken up: the output goes
on without a gap.

**Error counts.** For these formats the status's frames "with error" (`frames_error`,
`clip_frames_error`, the GUIs' error count) are the files' own: every frame that was
re-encoded, including frames repeated for lost pictures, and every frame that played with
its audio replaced by silence (at the very start and end of a file only beyond 2 audio
frames, where a camera's audio just begins or ends a little after or before its video).
`frames_dropped` counts the repeated frames. They come from the sink
(`pin_sink_status_t.frames_error`, `frames_repeated`; a closed file's last counts through
`final_status` and the closer), so they lag the picture count by a GOP or a repair.

**Containers.** MOV gets the HDV sample description Final Cut writes (`hdv3` 1080i50,
`hdv2` 1080i60, `hdv1` / `hdv5` / `hdv9` / `hdva` 720p30/25/60/50, `hdv6`..`hdv8`
1080p), not FFmpeg's choice, the XDCAM tag of the raster; Matroska the sequence header as
CodecPrivate.

**Tested** by `test_hdv_repair` on ffmpeg-made streams damaged as a dropout damages them,
each decoded back and compared frame by frame and audio block by audio block with the clean
source, and the sink's error counts checked against what it re-encoded and silenced: clean
(identical output, no error), 40 packets lost, 0.6 s lost (a GOP and more: frames repeated,
silence), 500 garbled packets, transport error bits, bits flipped with no packet error
(found by the decoder alone), audio packets lost alone (video untouched), three dropouts in
three GOPs, a capture that starts mid-GOP and ends mid-picture, a dropout in the first GOP,
closed GOPs, a clock jump, 720p, damage everywhere, and the repair falling behind.
`test_replay_hdv_mov` covers the whole capture: a clock jump without `--split` gives two
clean files, and a dropout shows in the status's error count. Not tested with a camera's
damaged tape yet, nor in Resolve. Damage that decodes as valid codes and shows in no packet
(a flipped coefficient) is passed through: nobody can see it without looking at the
picture. Pulldown (`repeat_first_field`, 720p24 in 60p) is timed as one frame per picture.

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
