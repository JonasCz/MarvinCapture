# What still needs testing (and probably fixing)

Status: **2026-11-05, second revision**, after the first round of fixes (commits
`3f9190b`, `e73dd6a`, `f72463a`, `e5fa4ee`, `84d41aa`). The first revision, earlier the
same day, was a survey of what had never been tested and where the code was likely
wrong. This one says, item by item, what was done and what still needs testing, and
with what. No hardware was available for this round: everything marked fixed or tested
below was tested in code on synthetic data (ffmpeg-made recordings, and EP 0x88 dumps
built from them), not on a device.

## How to read this

Every item is tagged with what it takes to test it:

| Tag | Meaning |
|---|---|
| **HW** | Needs a real camera, deck, tape or Pinnacle unit. |
| **FW-PC** | Can be done with a computer that has its own FireWire port, or possibly a second Marvin, acting as the "camera": it transmits a synthetic DV/HDV stream (or answers AV/C commands) into the MovieBox. See [FireWire PC as a test source](#a-firewire-pc-as-a-test-source). |
| **CODE** | Can be tested in code only: unit tests in `tests/engine`, synthetic DV frames / TS packets, synthetic EP 0x88 dumps (`tests/make_ep88.c`), or the replay device. No hardware. |

Many items carry two tags: a synthetic test gives the logic, the real device
confirms what cameras actually send.

Status markers:

- **Fixed**: the code was changed this round and a test on synthetic data covers it.
  What is left (usually HW) is listed with it.
- **Tested in code**: the behaviour is now covered by tests; little or no fix was needed.
- **Partly done**: some of the item was done; the rest is listed.
- **Left alone**: deliberately not touched this round; still open.
- Items with none of these were not taken up this round and keep their original
  marker: **Bug (confirmed in code)** when the code was read and the fault is plain,
  **Likely bug** when it follows from reading but was not traced end to end, and
  **Untested** when the code may be fine but nothing has exercised it.

Code is referred to by file and function rather than by line number.

**What has been verified on hardware so far** (unchanged this round):

- HDV: one Canon HDV camcorder, 1080i/25 (1440x1080, 25 fps, PIDs video `0x810`,
  audio `0x814`, PMT `0x81`), on a 500-USB and a 510-USB ([hdv.md](hdv.md),
  [deck-control.md](deck-control.md)).
- DV: one DV camcorder (make and model not recorded) with a short tape, on a 510-USB
  under Windows; one camcorder in "AV to DV" passthrough for 60 min.
- Analog: PAL composite, PAL S-video, NTSC composite ([analog.md](analog.md)).
- Devices: 500-USB (`0213`) and 510-USB (`0223`). Hosts: Windows x64, Apple Silicon
  macOS, Linux x86_64.

## Done in this round

| Item | What changed | Tests | Still needs |
|---|---|---|---|
| 1.1 HDV output parameters | size, frame rate, interlaced/progressive, field order and aspect from the stream | `test_replay_hdv`, `test_pin_vidfmt` | HW: 720p and 1080i60 cameras (HDV MOV/MKV are the only sinks that use them) |
| 1.2 720p SAR, DAR | SAR from the real raster, DAR from `aspect_ratio_information` | `test_hdv_rewrap`, `test_pin_vidfmt` | HW |
| 1.3 HDV MOV/MKV field order | progressive / TT / BB from the params | `test_hdv_rewrap` | HW |
| HDV MOV/MKV under damage | written live (`sink_hdvmux.c`): every GOP decoded to check it, damaged GOPs (packet loss or decoder concealment) re-encoded from the decoded pictures, lost pictures repeated, lost audio silence, timestamps from 0, scene split always on (also at a jump of the camera clock), the error counter from what the file re-encoded or silenced | `test_hdv_repair`, `test_replay_hdv_mov`, `test_pin_scene`, `test_hdv_error`, `test_pin_closer` | HW: a camera's damaged tape; the files in DaVinci Resolve; whether cameras close their GOPs; the CPU it takes on a slow computer and on a phone (Android offers it; a phone that cannot keep up stops the capture) |
| 1.4, 2.2 progressive | from `progressive_sequence`; field order from `top_field_first` | `test_pin_vidfmt`, `test_replay_hdv` | 24p / pulldown left alone; CODE: 720p50, 1080p; HW |
| 1.10 29.97 assumed | scene split, `captured_seconds()` and `.ts` replay at the detected rate; `.m2t` and TS by content | `test_replay_hdv`, `test_replay_hdv_split` | (a replay nit, see 1.10) |
| 1.11 drop-frame bit | bit 6, as in DV | `test_hdv_aux` | HW: a camera's aux stream (PID 0x811) |
| 3.1, 3.2 DV <-> HDV | the reassembler follows the switch; a new file in the other kind's format; a pipe stops | `test_reassembler_mixed`, `test_replay_mixed_kind`, `test_pin_fmtwatch` | HW, FW-PC: what a camera sends at the switch |
| 3.3 PAL -> NTSC | every DV frame sized by its own evidence | `test_reassembler_mixed`, `test_replay_mixed_dv` | HW, FW-PC: a real mixed tape |
| 3.4-3.6 format fixed at open | format watch with a cool-down; a new file where the output cannot carry a change | `test_pin_fmtwatch`, `test_replay_mixed_dv` | CODE: aspect end to end; HW: 16:9, 12-bit, 4-channel DV |
| 3.7 HDV format / PID change | PID change followed; picture buffer capped; a format change starts a new file | `test_reassembler_mixed`, `test_pin_fmtwatch` | CODE: the session's cut end to end; HW |
| 3.15 per-kind options | aspect (`aspect` / `aspect_hdv`) and title per kind after a DV <-> HDV change; aspect drop-down disabled for raw `.dv` / `.m2t`; core help texts | `test_replay_mixed_kind`, `test_pin_platform_api`, `test_pin_ui_api` | CODE: build and check the GUI side on Windows, macOS, Android (unbuilt) |
| 3.8 HDV scene split | hold-back FIFO, debounce in GOPs, cut on the GOP, PAT/PMT in front | `test_replay_hdv_split`, `test_pin_holdback`, `test_pin_scene` | HW: a camera's tape |
| 3.10 scene detector rate | restarts at every format cut | none of its own | CODE |
| 5.3 disk full, slow, write error | test seams (no fix needed) | `test_replay_disk`, `test_cli_pipe` | HW: a real full or slow drive; CODE: HDV, analog |
| 5.4 file-size limits | FAT32: DV/HDV go on in a new file, analog stops cleanly; AVI writer refuses a 1025th segment | `test_replay_fat32`, `test_avi_odml`, `test_pin_stop` | HW: a FAT32 stick, an 8-hour run; CODE: DV AVI past 1 GiB, analog counter wrap |
| 6 fixtures | generated at test time (ffmpeg, `make_ep88`); missing ones fail under `CI` | the replay, reassembler and most sink tests | HW: 12-bit / 4-channel DV and HDV aux data, recorded once |
| 6 replay NTSC as PAL | a `.dv` is sized by the DSF bit | `test_replay_unplug`, `test_dv_audio_vs_libavformat` | (nothing) |

**Left alone on purpose:** 3.9 (zero GOP timecode: perhaps the camera was in live
mode; check that first) and 24p in DV and HDV (2.1, the 24p parts of 1.4 and 2.2).
**Not taken up this round:** 1.5-1.9, 2.3, 2.4, 3.11-3.14, section 4, 5.1, 5.2,
5.5-5.9 and the rest of section 6. **New:** 3.15, open points in the format-change
handling.

---

## 1. HDV 720p and other HDV variants

Only 1440x1080 at 25 fps has been seen from a camera ([hdv.md](hdv.md) "Not tested on
hardware: 720p HDV, S400 cameras, 1080i/29.97"). Since this round the format is read
from the stream ([hdv.md](hdv.md#format-what-the-stream-says)) and tested with
ffmpeg-made 1080i25 and 720p59.94 streams. Raw `.m2t`/`.ts` is still the only HDV output
enabled; MOV/MKV for HDV stays disabled as untested (the format table in
`pin_session.c`), so the sink parameters below only matter once that is switched on, or
once another HDV sink exists.

**1.1 HDV output parameters were fixed at 1440x1080, top field first, 25 fps.**
**Fixed** (`e73dd6a`).
- `open_sink_for_scene()` hands the sinks the stream's size, frame rate
  (`frame_rate_code` with the sequence extension's `frame_rate_extension_n/_d`),
  interlaced or progressive, field order and aspect. `is_pal` decides nothing for HDV.
  Until the first sequence header, 1080i29.97 16:9 top field first is assumed.
- The session's PID map starts from the PAT/PMT the reassembler writes in front of the
  first picture, so the format is known from the first picture on (before, only once a
  PAT/PMT rode inside a picture).
- Tests: `test_replay_hdv` checks the status for `hdv.ts` ("1080i25") and `hdv-720p.ts`
  (1280x720, 60000/1001, progressive, 16:9, "720p59.94"); `test_pin_vidfmt` parses
  hand-built sequence and picture extensions (1080i29.97, 720p59.94, one split across
  two TS packets).
- Not asserted: the parameters a sink actually receives from the session (no test sink;
  the raw TS sink ignores them). 1080i29.97 and bottom field first exist only as
  hand-built headers, not as a stream.

**CODE** (ffmpeg-made 1080i29.97 and bottom-field-first streams through the replay; a
test sink that records its parameters, before HDV MOV/MKV is enabled), **HW** (what JVC
720p and 1080i60 cameras really send).

**1.2 720p got the wrong pixel aspect ratio.** **Fixed** (`e73dd6a`).
`pin_sink_sar_for()` takes the real height: 1440x1080 16:9 gives 4:3, 1280x720 16:9
gives 1:1. The display aspect comes from `aspect_ratio_information`
(`pin_vidfmt_mpeg2_dar()`), 16:9 only until a sequence header is seen. Tests:
`test_hdv_rewrap` (the SAR values, and the 720p stream remuxed with square samples),
`test_pin_vidfmt` (the aspect codes). **HW** (a 720p camera; a 4:3 HDV stream, if any
camera writes one).

**1.3 The remux marked all HDV video top-field-first interlaced.** **Fixed**
(`e73dd6a`). `sink_rewrap.c` takes the field order from the params: progressive, TT or
BB. `test_hdv_rewrap` remuxes the 720p stream and reads it back as progressive, 1080i as
TT. The HDV remux itself stays disabled (see section 6, "HDV remux under damage").
Progressive material inside an interlaced stream: see 1.4.

**1.4 "Progressive" was decided by height alone, and pulldown is ignored.** **Partly
done** (`e73dd6a`); the 24p part is **left alone** for now.
- Done: `hdv_error.c` reads the sequence extension (`progressive_sequence`,
  `frame_rate_extension`) and the first picture's coding extension
  (`picture_structure`, `top_field_first`, `progressive_frame`).
  `pin_vidfmt_hdv_interlaced()` takes `progressive_sequence` (the height only until one
  is seen), so 1080p reads as progressive; the field order comes from `top_field_first`
  of interlaced pictures.
- Not done: `repeat_first_field` is not read. 24p carried with 2:3 pulldown flags (Sony,
  Canon 24F) still shows as 1080i29.97, frame modes such as 25F/30F as 1080i, and
  `captured_seconds()` (the length in the stop message), `unit_seconds()` and the scene
  split count pictures at the coded rate. If JVC 720p24/720p30 is carried as a 60p/50p
  stream with repeat flags (unverified), the rate shown is double the real one.

**HW** (what JVC, Sony and Canon really write in 24p/25F/30F modes), **FW-PC**
(transmit an ffmpeg-made 24p/pulldown TS), **CODE** once a policy is decided
(hand-built picture extensions with RFF, as `test_pin_vidfmt` builds the others).

**1.5 PAT/PMT parsing is single-packet and single-shot.** Likely bug. Partly changed by
3.7: a video PID change is now followed.
- `hdv_scan_pat_pmt()` (`hdv_aux.c`) parses only the PUSI packet. A PSI section split
  over two TS packets is cut short, and `pmt_found` is still set, so the PMT is never
  re-read.
- Only video `stream_type 0x02` and audio `0x03/0x04` are recognised.
- A PMT version change mid-stream is followed only when the video PID changes with it:
  the reassembler then switches to the new PID after two GOPs, and the session's PID map
  starts again from the latest PAT/PMT (`dv_on_unit()`). A new audio PID on the same
  video PID is not picked up (monitoring only).
- The PAT/PMT kept for the start of a file assume one packet each, in both places that
  keep them: the reassembler (`ts_handle_packet()`) and the session's `hdv_psi_scan()`,
  which since this round also starts every file after a scene or format cut. A
  multi-packet PMT would leave such a file without one.

The PIDs are not hard-coded: `hdv-720p.ts` uses ffmpeg's PIDs (PMT 0x1000, video
0x100, audio 0x101) and is read correctly, but 0x810/0x814 is the only pair seen from a
camera. This is the main risk for JVC 720p. **CODE** (a PMT split across packets, a
version bump, another audio PID), **FW-PC**, **HW** (a JVC ProHD/HDV camera or deck).

**1.6 The start gate needs the sequence header in the first packet of the video PES.**
Untested. `ts_gate_try_open()` (`dv_reassembler.c`) looks for `00 00 01 B3` only in the
PES-start packet, and so does the new PID-change check. A long PES header, or user data
placed before the sequence header, would push it into the second packet; the gate would
never open and nothing would be written, with no message. **CODE**, **FW-PC**.

**1.7 One picture per PES is assumed.** Untested. A unit ends at each new video PES
start (`ts_handle_packet()`). `hdv_error.c` records only the first picture's type (and,
since this round, its coding extension) in a unit, and may count a false `pes_errors`
when a unit has a second PES start. Fine for the Canon and for ffmpeg's output; unknown
for others. **CODE**.

**1.8 The CIP check accepts only FMT 0x20, DBS 6, payload a multiple of 192.**
Untested. FN/SPH/QPC are not checked (`iso_header_ok()` in `dv_reassembler.c`). A camera
with another source-packet layout is dropped as noise with no diagnostic. S200/S400
senders have never been seen. **HW**, **FW-PC** (a sender at S400, if the software
allows it).

**1.9 HDV audio.** Untested. Only MPEG-1 Layer II is recognised. The monitor path
(`pin_hdv_audio.c`) handles 44.1/32 kHz MP2 and mono (duplicated), but `pin_hdv_audio`
has no test of its own. More than two channels are ignored. The sinks are told 48 kHz
stereo whatever the stream carries (`open_sink_for_scene()`); the HDV MOV/MKV sink
reads the rate and channels from the MP2 frames instead. The file keeps the TS audio
untouched, so this affects monitoring only. **CODE** with `hdv.ts` and ffmpeg-made MP2 at other rates.

**1.10 HDV scene split and replay pacing assumed 29.97 fps.** **Fixed** (`e73dd6a`,
`3f9190b`).
- The scene detector is configured at the detected rate (`pin_scene_config_hdv()`;
  29.97 only while no sequence header has been seen), and so are `captured_seconds()`
  and the split lookahead's `unit_seconds()`.
- Replay of a `.ts`/`.m2t` paces at the detected rate. An MPEG-2 TS is recognised by its
  sync bytes, or by a `.ts` or `.m2t` name.
- Tests: `test_replay_hdv` (1080i25 and 720p59.94), `test_replay_hdv_split`.
- Left: replay of a raw EP 0x88 dump still paces every unit, HDV pictures included, at
  the DV rate (25 or 29.97, `replay_run()`). This affects replay timing only. **CODE**,
  if it matters.

**1.11 `hdv_aux_scan_datetime` read the drop-frame flag from bit 7.** **Fixed**
(`e73dd6a`): bit 6, as in `dv_subcode.c`; bit 7 is the colour-frame flag. `test_hdv_aux`
checks both flags on a hand-built pack. No capture code calls the function yet, and its
reading of the aux stream on PID 0x811 is a best guess (`hdv_aux.h`). **HW**: a real
camera's aux stream, kept as a small optional recording in `tests/data`.

## 2. 24p and other frame-rate modes

24p was **left alone** this round, in DV and in HDV.

**2.1 DV 24p (Panasonic DVX100 24p/24pA, Canon 24F in DV) is neither detected nor
documented.** Untested; **left alone**. Such tapes are 29.97 DV with 2:3 or 2:3:3:2
(advanced) pulldown. The app treats them as 29.97i BFF (`open_sink_for_scene()`), and no
VAUX bits beyond the aspect code are parsed. Capturing them as plain 29.97 loses nothing
and is probably the right default. But:
- nothing in [usage.md](usage.md) says so;
- nobody has checked that such tapes capture cleanly;
- removing the pulldown is not offered.

**HW** (a DVX100 24p/24pA tape, a Canon 24F tape), **FW-PC** (ffmpeg can make 24p
material with telecine to 29.97 DV), **CODE** (if 24pA detection or reverse telecine
is ever wanted, the cadence detection is pure logic).

**2.2 HDV 24p/25p/30p and 720p50/60.** **Partly done.** 720p59.94 progressive is
detected and tested with an ffmpeg-made stream (1.1); 720p50 and 1080p25/30 take the
same path (`progressive_sequence`) but no stream tests them (**CODE**). 24p with pulldown
flags is **left alone** (1.4). **FW-PC**, **HW**.

**2.3 GOP timecode at 50/60 fps and drop frame.** Untested. The timecode label is built
from the GOP header's picture count (`dv_on_unit()`), and drop frame is shown as `;`
whatever the rate. At 720p50/60 the frame field runs to 49/59. The scene detector now
counts the GOP timecode at the detected rate, but its drop-frame correction applies only
at a nominal 30 fps (`pin_scene.c`), so 59.94 drop frame is counted without it. How HDV
cameras number GOP timecode at 50/60 and 59.94 is unknown. The deck-timecode fallback
(`hdv_tc_live`) has been seen on one Canon only. **CODE**, **HW**.

**2.4 DV timecode accepts frame numbers up to 29 for PAL.** Untested
(`dv_subcode.c`). Out-of-range values are not flagged. **CODE**.

## 3. Tapes with a mix of standards and formats

Before this round nothing tested a mid-stream format change, and the reassembler locked
the format on the first packet. Since `e5fa4ee` the capture follows the stream
([usage.md](usage.md#running-a-capture),
[hdv.md](hdv.md#tapes-that-mix-dv-and-hdv-or-hdv-formats),
[protocol.md](protocol.md)):

- **Reassembler.** DV <-> HDV switches after 256 data packets of the other format in a
  row (`DV_FMT_SWITCH_PACKETS`); a shorter run is dropped and counted
  (`fmt_dropped_packets`), a switch counted in `format_switches`. The run is replayed
  into the new format, so nothing of it is lost; the old format's unit in progress ends
  as at the end of a capture. Every DV frame is sized by its own evidence (`std_switches`
  counts changes of the confirmed standard). An HDV video PID change is followed after
  two GOPs, and a picture past 4 MB is dropped.
- **Session.** `pin_fmtwatch` watches the format: the kind; DV standard, aspect and audio
  rate, channels and bits; HDV size, frame rate and `progressive_sequence`. A change
  counts after 5 DV frames or 3 HDV GOPs in a row (units that do not carry the value are
  neutral), and is back-dated to its first unit through the hold-back FIFO
  (`pin_holdback`), so one corrupt frame or GOP never splits. Then
  `pin_format_change_action()` decides:

| Output | New file on |
|---|---|
| raw `.dv` | PAL <-> NTSC only (aspect and audio are per frame) |
| DV AVI / MOV / H.264 / HEVC | standard, aspect (unless `--aspect` forces one), audio rate, channels or bits |
| HDV `.m2t` / `.ts` | size, frame rate, interlaced/progressive |
| any file | DV <-> HDV: a new file in the other kind's format and extension |
| standard output | never a new file; DV <-> HDV ends the capture (`PIN_STOP_ERROR`, "the input changed from DV to HDV; a pipe carries one format only"; the CLI exits 4) |

New files are numbered `name-0002.ext`, ... without `--split` (the next scene number with
it); the name check before a capture covers both kinds' extensions; the status shows the
confirmed format and `PIN_EVT_INPUT_FORMAT` is sent on a change. All of this was tested
with synthetic dumps only.

**3.1 DV then HDV (or HDV then DV) on one tape: the capture stalled.** **Fixed**
(`e5fa4ee`). The reassembler no longer locks the format on the first data packet (see
above).
- Tests: `test_reassembler_mixed` (DV, HDV, DV: both DV pieces whole, the TS from PAT,
  PMT and a picture with a sequence header, two switches; a 300-packet HDV burst inside
  DV dropped, the DV around it whole) and `test_replay_mixed_kind` (3.2).
- Still unknown: what a camera sends when the tape switches between DV and HDV. A bus
  reset ends the capture with `PIN_STOP_CAMERA_LOST`, itself untested (4.9); a gap or a
  burst of garbage should be handled as above.

**FW-PC** (transmit DV, then HDV, on the same channel), **HW** (a tape recorded in both
modes, played in an HDV camera or deck).

**3.2 If the stream kind changed within a session, the open file was wrong.** **Fixed**
(`e5fa4ee`). The policy: a new file in the other kind's format and extension (the
`--format` for that kind), from the first unit of the new kind; on standard output the
capture ends. Units of another kind never reach the open file.
- `test_replay_mixed_kind` captures DV -> HDV -> DV end to end: `name.dv` (every PAL
  frame), `name-0002.m2t` (starting with PAT, PMT and a GOP) and `name-0003.dv` (the
  NTSC frames); and to a pipe, which stops with `PIN_STOP_ERROR` and the reason after
  every PAL frame went out. The CLI's exit code 4 for that stop is documented, not run
  in a test.
- Open points: 3.15.

**HW**, **FW-PC** as for 3.1.

**3.3 PAL then NTSC mid-tape (DV) corrupted every later frame.** **Fixed** (`e5fa4ee`).
The reassembler no longer raises the sequence count to 12 for good: each frame is
emitted at its own size, by its own evidence (sequence 10 or 11 arrived, or the DSF bits
of at least three of its sequence headers, agreeing two to one); only a frame without
evidence takes the confirmed standard, which follows the stream after 3 frames
(`dv_end_frame()`, `std_follow()`).
- Tests: `test_reassembler_mixed` (PAL then NTSC: the output is the two `.dv` files byte
  for byte), `test_replay_mixed_dv` (as raw `.dv`: two files, byte for byte, no frame
  padded or counted damaged).
- Untested: damaged frames right at the switch, which take the confirmed standard.

**FW-PC**, **HW** (a PAL/NTSC mixed tape: rare, but it is what a deck with a
multi-standard tape, or a re-used tape, produces).

**3.4 Sinks freeze geometry, fps, aspect and audio format at open.** **Fixed** by
policy (`e5fa4ee`). The sinks still fix these when a file is opened, but a change now
starts a new file wherever the output cannot carry it (the table above), opened with the
new format's parameters. So a confirmed change never reaches a file opened for the old
format: no NTSC run in a PAL `sink_rewrap` file, none dropped by `sink_hwenc`, one
standard per raw `.dv` file. A stray frame or two of another standard or mode, too few
to count, still goes into the open file as before (units of another kind never do). A
pipe goes on (raw DIF and TS carry the change in every frame).
- Tests: `test_pin_fmtwatch` (the cool-down and every row of the table),
  `test_replay_mixed_dv` (PAL -> NTSC as raw `.dv`; 48 -> 32 kHz as DV AVI).
- Not run end to end: DV MOV, H.264 and HEVC across a change, and a pipe through a
  PAL/NTSC change. **CODE**.

**3.5 4:3 / 16:9 change mid-capture.** **Partly done** (`e5fa4ee`). The aspect is part
of the watched format: a change counts after 5 frames and, in DV AVI/MOV/H.264/HEVC,
starts a new file at its first frame with the new aspect (not when `--aspect` forces
one); raw `.dv` and a pipe carry it per frame. Before, the rest of the file kept the
first value, or with `--split` the next file picked it up up to ~1 s late.
- Tested only in `test_pin_fmtwatch` (the change back-dated to the first 16:9 frame,
  and the decisions). There is no 16:9 DV fixture, so no replay test (**CODE**: ffmpeg
  can make 16:9 DV).
- 16:9 detection still uses VAUX DISP codes 2 and 7 only; `test_dv_subcode` covers only
  `0x02` and a missing pack (**CODE**).

**FW-PC**, **HW** (a 16:9 DV source; a tape with both).

**3.6 DV audio mode change (32 kHz 12-bit to 48 kHz 16-bit, or 2 to 4 channels).**
**Partly done** (`e5fa4ee`). `sink_rewrap` still fixes the audio track's rate and
channel count from the first audio frame, but an audio change (rate, channels or bits)
now starts a new DV AVI/MOV/H.264/HEVC file with the new audio; raw `.dv` goes on (the
audio is per frame).
- `test_replay_mixed_dv` captures 48 kHz then 32 kHz (both 16-bit stereo) as DV AVI:
  two AVIs, the second's audio track at 32 kHz, every frame read back by libavformat.
- Not testable with ffmpeg-made data: 12-bit nonlinear and 4-channel audio (ffmpeg's DV
  muxer writes 16-bit stereo only). A 12-bit section mid-tape and a second stereo pair
  that appears later are untested.
- Preview resamples per frame and is probably fine (maybe a click).

**HW** (an LP or 12-bit 4-channel recording, kept as `tests/data/dv-ntsc-32k.dv`; then
**CODE**: put it before or after `dv-ntsc.dv` in a `make_ep88` dump), **FW-PC**.

**3.7 HDV 1080i to 720p, or a PID change, mid-stream.** **Fixed** in code (`e5fa4ee`);
what cameras do is still speculative.
- The reassembler follows a video PID that changes and persists (a GOP on the new PID
  twice with no picture of the old one between; `ts_pid_switches`); the picture in
  progress and the new PID's first GOP are lost. A picture past 4 MB without another
  video PES start is dropped and the gate re-armed (`ts_pic_overflows`), so the picture
  buffer no longer grows without bound.
- The session counts a change of size, frame rate or `progressive_sequence` after 3 GOPs,
  sends `PIN_EVT_INPUT_FORMAT` and starts a new `.m2t` at the GOP where the change
  began, with PAT and PMT in front.
- Tests: `test_reassembler_mixed` (`hdv.ts` then `hdv-720p.ts` as one dump: video PID
  0x810 -> 0x100 and a new PMT; the new PID's first picture behind a fresh PAT/PMT, with
  a sequence header), `test_pin_fmtwatch` (the HDV cool-down and decision).
- Not run end to end: the session's cut on that dump (**CODE**: `ep88-hdv-pid.bin`
  through a replay capture; today only `test_reassembler_mixed` reads it).

**FW-PC**, **HW**: whether any camera changes the video PID or the HDV format within a
tape.

**3.8 HDV scene split could not work as designed.** **Fixed** (`f72463a`). On a test
tape with a timecode jump it did not split at all.
- A hold-back FIFO (`pin_holdback`, pure) replaces the 32-entry scene FIFO: HDV holds
  every picture from a pending break's first GOP on (bounded at 32 MiB); DV keeps its
  fixed window.
- The HDV detector (`pin_scene_config_hdv()`) debounces over 3 GOPs, with `sparse_tc`:
  one or two GOPs with a wrong timecode, or GOP headers lost in a dropout, never split;
  a timecode going backward is still a break.
- The cut lands on the first picture of the GOP with the new timecode. The new file
  starts with the latest PAT and PMT (kept by the session itself, `hdv_psi_scan()`),
  then that picture with its sequence header.
- The 1 s minimum segment counts pictures at the detected rate (`unit_seconds()`), and a
  replay capture also waits for a GOP before writing.
- Tests: `test_replay_hdv_split` (a tape of four ffmpeg-made pieces: a stray GOP that
  must not split, then a jump; exactly two files, each starting with PAT, PMT and a
  GOP, the second at the jump's first picture, every picture once and in order, both
  decode), `test_pin_holdback`, the HDV cases in `test_pin_scene`.

**HW**: whether cameras start a recording with a closed GOP or set `broken_link` (an
open GOP's leading B pictures would not decode at the start of the new file); GOPs
without a sequence header (the file then decodes from the next one); the debounce
against real dropouts. A camera that writes a zero timecode still never splits (3.9).

**3.9 HDV scene split never fires on cameras that write a zero GOP timecode.**
Bug (confirmed in code); **left alone** this round. The Canon wrote `00:00:00:00` in every
GOP header, but possibly only because it was in camera (live) mode rather than playing a
tape. The live-timecode display guards on a zero timecode (`hdv_gop_time_nonzero()`),
the scene record does not (`tc_valid` is set whenever a GOP header is found,
`dv_on_unit()`), so every picture looks continuous. The new HDV detector also treats a
standing timecode as continuous, and `test_pin_scene` (`test_hdv_zero_tc_never_cuts`)
pins that behaviour. HDV recording date/time from the aux stream is not used for
splitting (and its layout is unverified, 1.11). **HW** first: play a recorded tape in
the Canon in VCR mode and see whether the GOP timecode runs. If some camera really
writes zeros on tape: apply the same zero guard and consider the aux-PID recording date
(**CODE**).

**3.10 Scene detector fps was set once per pass.** **Fixed** (`e5fa4ee`). The detector
starts over at every format cut (`format_cut_begin()`), at the new rate. A change the
open file carries (raw `.dv` aspect or audio) keeps it, as the rate does not change
then. No test of its own: no test combines `--split` with a format change (**CODE**,
3.15).

**3.11 Gaps and blank sections.** Untested.
- DV: a partial frame at a gap is emitted zero-padded (counted damaged) when data
  resumes.
- HDV: a partial picture is glued to whatever arrives after the gap (since this round a
  picture past 4 MB is dropped, 3.7).
- The gate is re-armed per pass, not per gap.
- After a gap of more than 1 s, `dv_on_unit()` only restarts the HDV error tracking.

Documented hardware behaviour ([deck-control.md](deck-control.md)):
- A capture that starts on blank tape never gets a first frame and stays "capturing".
- The end of tape is seen only through the no-signal timeout.

**CODE** (insert gaps and mid-picture cuts in a replay, e.g. with `make_ep88`), **FW-PC**
(pause the sender), **HW** (a tape with unrecorded gaps).

**3.12 Timecode edge cases not in `test_pin_scene.c`.** Untested. HDV now has a few
cases (stray GOPs, lost GOP headers, a backward jump, a standing zero timecode). Still
not covered:
- REC START only, with no timecode and no date: probably never splits.
- A frozen timecode at pause or still.
- Small forward jumps.

What cameras do around REC START and still frames is unknown. **CODE**, **HW**.

**3.13 LP recordings.** Untested. Nothing in the code or docs mentions LP. LP means
32 kHz 12-bit audio (see 3.6) and generally more dropouts. **HW**.

**3.14 Analog: the standard changes during a capture.** Untested; not taken up (the
format watch above is DV/HDV only). The restart only happens in READY (`analog_tick()`);
geometry and audio packet size are fixed at start. With `--std auto` the status shows
the new standard while the file keeps the old geometry. [analog.md](analog.md) also
lists "what happens at the moment the signal drops or returns mid-capture" as open.
**HW** (a VCR tape with a PAL/NTSC change).

**3.15 Open points in the format-change handling** (new this round, from `e5fa4ee`).
Decisions first, then **CODE**:
- **Per-kind options come from the first kind.** **Partly fixed:** aspect and title are
  now per kind. `pin_capture_opts_t` has `aspect_hdv` next to `aspect` (analog and DV
  files take `aspect`, HDV files `aspect_hdv`, at every file open: start, scene split,
  format change, FAT32 cut, pass; an older caller's size gets `aspect_hdv = aspect`; the
  CLI's `--aspect` sets both), and the GUIs fill them from the analog / DV and the HDV
  settings, Auto where the format stores no aspect (`pin_format_info_t.supports_aspect`:
  not raw `.dv` or `.m2t`; the aspect drop-down is then disabled, with the core's
  reason, `pin_format_aspect_help`). The GUIs always pass the name as the title; the
  core stores it in each file whose format has one (`supports_title`), so a DV AVI after
  an HDV `.m2t` gets it too. The name field's tooltip names those formats
  (`pin_format_title_help`). Tests: `test_replay_mixed_kind` (DV -> HDV -> DV as DV AVI
  with 16:9 for DV, 4:3 for HDV and a title: both AVIs read back 16:9 with the title;
  a tape that starts on HDV: the DV AVI after the `.m2t` is 16:9 with the title),
  `test_pin_platform_api` (`supports_aspect`, the old sizes), `test_pin_ui_api` (the help
  texts). Not testable yet: `aspect_hdv` in an HDV file, since no HDV format that stores
  an aspect can be captured (HDV MOV / MKV are disabled). **Deliberately still from the
  start kind:** scene split, idle stop, maximum duration and passes (per capture / tape).
  **CODE, unbuilt:** the GUI side (Windows, macOS, Android: interop, enable rule,
  tooltips, `BuildCaptureOpts` / `buildCaptureOpts` / `buildPlan`) was not built or run;
  build each and check the aspect drop-downs (DV tab on raw DV: greyed with the
  tooltip; DV AVI: enabled; the HDV tab: always greyed today) and that a DV <-> HDV
  capture gives each file its tab's aspect.
- **A bit-depth-only change splits non-raw DV.** 12-bit <-> 16-bit at the same rate and
  channels starts a new DV AVI/MOV/H.264/HEVC file, probably needlessly: those files
  hold 16-bit PCM either way.
- **The first unit's format is taken at once** (`PIN_FMTWATCH_FIRST`), without a
  cool-down. A damaged first frame or GOP could set the first file's format; the real
  one would then cut a new file 5 frames or 3 GOPs later.
- **The scene number counts format cuts.** Without `--split` the status's scene number
  reads 2 after a format change.
- **Test gaps:** `--split` together with a format change (3.10), an aspect change end to
  end (3.5), the session's HDV format cut (3.7), MOV/H.264/HEVC and a pipe across a
  change (3.4).

## 4. Cameras and decks from other manufacturers

Not taken up this round. Tested: one Canon HDV camcorder and one unnamed DV camcorder.
Never tested:
- Sony, Panasonic, JVC, Samsung, Sharp.
- Any dedicated deck: Sony DSR / GV-D, Panasonic AG-DV, JVC HR-DVS, Sony HVR-M15/M25,
  JVC BR-HD50.
- DVCAM, DVCPRO25, Digital8.

The Sony and Samsung error-classification quirks in [usage.md](usage.md) were derived
from files, not live cameras.

A note on testing: the deck code (`pin_deck.c`, deck parts of `pin_session.c`) calls
`p1394_avc*` directly. There is no seam for scripted AV/C replies, and replay sessions
skip deck control. `test_pin_deck.c` covers only the pure byte builders and parsers. The
most useful single change for **CODE** testing here is a small fake-link layer, a
function table or a scripted `p1394_avc_*` stub, so the retry, timeout and sequencing
logic can run against canned replies.

**4.1 NOT_IMPLEMENTED is treated as "busy" and ends as success.** Bug (confirmed in code),
for any deck that really does not implement a command.
- For transport opcodes, a `0x08` NOT_IMPLEMENTED reply is retried up to 5 times,
  0.7 s apart.
- After that the async path returns DONE: only `0x0a` REJECTED counts as failure
  (`pin_deck_async_poll()`).
- The "busy" reading comes from one Canon.

On another camera (for example FF or PLAY in camera mode), the app waits ~3.5 s and
then believes the command was sent. A rewind-first capture would wait for a rewind that
never started, and a STOP would be reported as done. **CODE** (with the fake link), then
**HW** / **FW-PC** to see what real decks answer.

**4.2 Transport-state mapping is narrow.** Untested (`pin_deck_state_from_avc()`).
- Five codes have been observed. `c3 6d`, `c3 65` and `c4 45` are guesses.
- Any other `c3` mode counts as PLAYING (slow, reverse, frame advance).
- `c1` (load medium, no tape) and `c0` map to UNKNOWN.

A wrong "stopped" ends rewind-first or multi-pass early. **CODE** (table test over the
AV/C transport modes; assert unknown modes never read as stopped), **HW** to collect real
codes.

**4.3 The VCR subunit is hard-coded as `0x20`.** Untested. SUBUNIT INFO and UNIT INFO
are never sent ([deck-control.md](deck-control.md) lists this as open). A device whose
tape subunit is elsewhere ignores every command, which then times out ("ignoring
unrelated FCP response"). **CODE** (parse SUBUNIT INFO replies), **FW-PC**, **HW**.

**4.4 Camera mode vs VCR mode is not detected.** Untested. In camera mode many camcorders
reject or do not implement transport commands; that feeds 4.1. It may also be why the
Canon sent a zero GOP timecode (3.9). **HW**.

**4.5 Fixed timeouts, no resend.** Untested. Sync commands wait 3 s (10 s after INTERIM);
async commands fail after 3 s with no resend, so a lost STOP stays lost. Responses are
matched by subunit and opcode only, not ctype, so a late reply to an earlier command
could be taken for the reply to a newer one with the same opcode (speculative). A
deck-state query that keeps failing is ignored silently: there is no "deck not
responding" status (the transport-state poll in `dv_tick()`). **CODE**, **HW** (slow
decks: DVCAM, Digital8).

**4.6 Timecode from the deck.** Partly tested.
- Covered: the BCD parse, drop frame and invalid inputs (`test_pin_deck.c`).
- Untested: ABSOLUTE TRACK NUMBER is never used; a deck that reports only ATN or a
  counter; the NTSC drop-frame deck path; `--wait timecode=` against such decks.
- Seek (`51 20`, `52 20`) was not implemented on either tested camera, so there is no
  seek feature.

**CODE** for parse edge cases, **HW**.

**4.7 Plug and channel handling.**
- Only `oPCR[0]` is connected (compare-swap, up to 4 retries).
- Nothing is allocated from an isochronous resource manager.
- If no node answers the plug read, the device listens on broadcast channel 63
  (`pinnacle_stream_start()`).

A camera that wants a properly allocated point-to-point connection, has several output
plugs, or moves channel after connecting, has not been seen. **CODE** (`pcr_adjust`
against a fake register pair: concurrent change, count 0/63, failed lock), **FW-PC**
(a sender with plug emulation, see below), **HW**.

**4.8 Camera discovery takes the first node that answers oMPR** (`p1394_find_camera()`).
Two devices on the bus (a camera and a deck, or a PC) are not handled deliberately.
**FW-PC** (PC plus camera on one bus), **HW**.

**4.9 Bus reset during a capture** ends with `PIN_STOP_CAMERA_LOST`; documented as
untested. A capture still waiting for its first frame keeps waiting. Some cameras may
reset the bus on a mode switch, tape load or power save, or when a tape switches between
DV and HDV (3.1). **CODE** (fake link injecting a SelfIDCount change), **FW-PC** (unplug
the PC's cable or restart its sender), **HW**.

**4.10 Collect a camera matrix.** For each new camera or deck, record in
[deck-control.md](deck-control.md):
- the UNIT INFO and SUBUNIT INFO answers;
- the transport-state bytes in every mode;
- the TIME CODE answers in camera and VCR mode;
- the oMPR/oPCR values;
- a `--debug` AV/C log;
- a short capture of each format the camera records (also as optional fixtures, see
  section 6).

Also record which camera the existing DV tests used. Priority:
- a Sony DV or DVCAM deck;
- a Panasonic and a JVC camcorder;
- a Sony HDV deck (HVR-M15/M25);
- a JVC 720p HDV camera;
- a Digital8 camcorder.

**HW**.

## 5. Pinnacle units, hosts, robustness

**5.1 Other Pinnacle models.**
- 700-USB (`0212`) and 710-USB / MovieBox Plus (`0224`): "supported, untested". Their
  extra I2C chip and PHY ports are not driven.
- MovieBox Deluxe (`0206`): the FX2 firmware download, the re-enumeration wait and the
  GUID read are best guesses. If the GUID read fails, a fixed fallback GUID is used, so
  two such units on one bus would collide.
- Warm-start shortcuts are enabled for 0213/0223 only. They are "not verified" on a
  true power-up.

**HW**; **CODE** for the model table and a fake libusb for the FX2 byte sequence.

**5.2 Hosts and OS.**
- Windows on Arm and Intel macOS: built, never run against the device.
- Android: never run against the box. Only API 34 in an emulator; USB permission,
  firmware upload, unplug and thermal behaviour are open
  ([handoff/android-plan.md](handoff/android-plan.md)).
- USB host controllers other than Intel and Apple (AMD, ASMedia, Renesas): not
  mentioned anywhere.
- Hubs: logic only; Android reports no hub depth, so its hub warning never fires.

**HW**.

**5.3 Disk full, slow disk, write error.** **Tested in code** (`84d41aa`); no fix was
needed.
- Test seams, environment variables that change nothing unless set:
  `PIN_TEST_FREE_SPACE` (the free space `capture_guard()` sees),
  `PIN_TEST_WRITE_FAIL_AFTER` (a raw or DV rewrap sink's file refuses bytes beyond a
  budget, as ENOSPC would; `pin_sink_fault.c`), `PIN_TEST_SINK_DELAY_MS` (a slow
  drive), `PIN_TEST_WRITER_QUEUE` (a small writer queue).
- `test_replay_disk` (DV): `DISK_FULL` while there is still room to finish, the raw DV
  whole frames and the DV AVI indexed (libavformat reads every frame); `WRITE_ERROR` at
  once, the file closed and readable; `DISK_SLOW` with no hole (compared with the
  source).
- `test_cli_pipe`: the same slow-queue path on a pipe (`PIPE_SLOW`, exit 4), and a
  reader that exits early (`PIPE_CLOSED`, exit 4).
- Not covered: HDV onto a full or failing drive (**CODE**); analog, which has no replay
  (5.8), and the H.264/HEVC sinks, which have no write-fail seam (**CODE**); what each
  OS really reports (**HW**: a nearly full drive, a USB stick pulled while writing, a
  slow network drive).

**5.4 Long captures and file-size limits.** **Partly done** (`84d41aa`).
- **Fixed, FAT32.** Before, a capture ended with a write error at 4 GiB, a DV AVI
  without its index. Now a DV or HDV capture to a FAT32 drive goes on in a new file
  (`name-0002.ext`, ...) before the open one would pass 4 GiB, through the hard cut a
  format change uses (on a DV frame, or an HDV GOP with PAT and PMT in front), keeping
  1/64 of the limit (64 MiB) for the container's index or trailer. An analog capture
  stops there with `DISK_FULL` ("the file reached the 4 GiB size limit of the FAT32
  drive"), the file finished. Pipes are never affected. The output check says
  beforehand what will happen. `test_replay_fat32` (seam `PIN_TEST_FAT32_LIMIT`, a 2 MB
  limit) checks raw DV, DV AVI and HDV across the cuts and the output check's note;
  `test_pin_stop` the texts and the cap. The analog stop runs in no test (no analog
  replay).
- **Fixed, OpenDML.** Past 1024 segments (~1 TB, ~14 h of analog AVI) the AVI writer
  reported a write failed but still wrote it into a segment no index lists, and the
  header counted frames the index did not have. That write is now refused and the file
  stays whole. `test_avi_odml` (segments made small with `avi_writer_set_segment_limit()`)
  writes 14 RIFF segments and reads every frame back through the indexes, then fills the
  super index (1024 entries).
- Not tested: a real FAT32 drive and its detection (`fat32_and_free()`; **HW**: a FAT32
  USB stick); DV AVI past FFmpeg's own 1 GiB RIFF segments (`sink_rewrap`; **CODE** with
  a long synthetic capture, or **HW**); the 8-hour goal (**HW**: one real run per OS);
  the analog 16-bit counter wrap at 65,536 frames, which was a real bug before (the
  audio loss at 21m50s; **CODE**, needs a fake USB layer or a recorded analog dump, see
  5.8). The 32-bit header counters do not wrap within 8 hours.

**5.5 Unplug.** Covered only through the replay device (`test_replay_unplug.c`, DV and
HDV). Not covered: a real libusb unplug during bring-up, firmware upload, a deck command
or file finalising; an Android detach. **CODE** (replay failures at each phase), **HW**.

**5.6 Sleep/resume and USB autosuspend.** Nothing handles them. A laptop sleeping
mid-capture leaves an undefined state. Decide the behaviour (stop with "device lost"),
test it with an injected error (**CODE**), confirm on each OS (**HW**).

**5.7 USB stall and drop behaviour.** The 256-transfer queue and 64 MB ring were tuned
once by hand ([usage.md](usage.md)). Partly covered since this round: the `DISK_SLOW`
case of `test_replay_disk` and the slow reader of `test_cli_pipe` run a stalled writer
against a small queue. Still no stress test of the USB side (the transfer queue and the
ring) and nothing under TSan. **CODE** (writer stall with a bounded ring, under TSan),
**HW** (busy disk, hub).

**5.8 Analog standards.** Untested: NTSC S-video, PAL-M, PAL-N, PAL-60, NTSC-4.43,
NTSC-J, SECAM; the vendor's NTSC audio packet size is unknown. The analog stream code
(`pinnacle_analog.c`, `pinnacle_stream.c`) has no tests, and there is no analog replay
fixture. Weak or jittery sources (worn VHS) with `--std auto` are untested. **HW**;
**CODE** for the SAA7113 register table per standard and, with a recorded analog dump,
the repair and reassembly logic.

**5.9 Hardware encoders.** Tested only with the FFV1 stand-in
(`PIN_HWENC_ENCODER=ffv1`). VideoToolbox, Media Foundation and MediaCodec have never run
in tests. The Android emulator's encoder stopped after 18 s ("took no picture for 10 s").
**CODE** (a stuck-encoder fault to exercise the watchdog), **HW** per OS.

## 6. Test infrastructure (all CODE)

- **CI still runs nothing automatically.** The workflow runs only on manual dispatch,
  and the ctest suite only when "Run the ctest suite" is ticked (off by default,
  [ci.md](ci.md)); then each desktop job installs ffmpeg (`install-deps.sh --tests`,
  `install-deps.ps1 -Tests`) and runs the whole suite. Android never runs tests. There
  are no sanitizers. Suggested: a Linux push job that runs ctest, plus ASan/UBSan and
  TSan builds. The core is thread-heavy: USB, writer, closer, preview.
- **Fixtures: done** (`3f9190b`). Nothing in `tests/data` is needed any more. The
  fixture-setup test `pin_test_data` (`tests/make_fixtures.cmake`) makes the DV and
  HDV recordings with ffmpeg at test time, and `tests/make_ep88.c` wraps them into raw
  EP 0x88 dumps (CIP packets, OHCI receive records, type-9 messages with ring wraps,
  junk in front), several inputs back to back for the mixed-format tapes
  ([../tests/data/README.md](../tests/data/README.md)). Without ffmpeg the tests that
  need them print `SKIP` and pass; with `CI` set they fail (`tests/test_fixture.h`). The
  sha256 baseline of a real dump is replaced by a byte-for-byte round trip
  (`reassembler_ep88_pal`, `reassembler_ep88_ntsc`).
  - Still only from a real recording: 12-bit nonlinear and 4-channel DV audio (the
    optional `tests/data/dv-ntsc-32k.dv`; ffmpeg's DV muxer writes 16-bit stereo only)
    and HDV aux data on PID 0x811. **HW**, recorded once.
  - No test now runs on a dump of the real endpoint: the generated dumps follow
    `make_ep88`'s model of it ([protocol.md](protocol.md)). A short real DV and HDV dump,
    kept as optional fixtures like `dv-ntsc-32k.dv`, would keep that model honest.
- **Synthetic HDV streams: mostly done.** ffmpeg makes `hdv.ts` (1080i25 with the
  camera's PIDs), `hdv-720p.ts` (720p59.94 progressive, ffmpeg's PIDs) and the
  `hdv-split-*` pieces (GOP timecode jumps); `test_pin_vidfmt` builds sequence and
  picture extensions by hand. Still missing: 1080i29.97 and bottom-field-first streams,
  720p50, 1080p24/25/30, 24p with pulldown flags (when 24p is taken up), split PSI and a
  PMT version change (1.5), long PES headers and a zero PES length (1.6, 1.7).
- **Untested modules:**
  - `pin_hdv_audio.c` and `pin_update_net.c` have no tests;
  - the `MarvinCaptureCLI` executable now runs in ctest, but only on its pipe paths
    (`test_cli_pipe`: DV and HDV into ffmpeg, `PIPE_CLOSED` and `PIPE_SLOW`, exit 4);
    its parser and sequencer are covered through the core (`test_pin_script`,
    `test_pin_script_run`); its other exit codes and its status output are not;
  - `pin_preview.c` is tested only on DV;
  - the GUIs have no automated tests, which the core-first rule partly offsets.
- **Replay misread some NTSC `.dv` files as PAL.** **Fixed** (`3f9190b`). `replay_run()`
  sizes a `.dv` file's frames by the DSF bit of its first header block, not by the file
  size. `dv-ntsc.dv` (30 NTSC frames, also a whole number of PAL frames) is replayed by
  `test_replay_unplug`, which checks the frames counted against the file;
  `test_dv_audio_vs_libavformat` sizes frames the same way.
- **Test seams.** New this round: `PIN_TEST_WRITE_FAIL_AFTER`, `PIN_TEST_FREE_SPACE`,
  `PIN_TEST_WRITER_QUEUE`, `PIN_TEST_SINK_DELAY_MS`, `PIN_TEST_FAT32_LIMIT` and
  `avi_writer_set_segment_limit()` (5.3, 5.4), next to `PIN_REPLAY_UNPLUG_AFTER`. There
  is still no seam for the 1394 link (AV/C replies, bus reset; section 4) or for libusb
  (5.1, 5.5, 5.6).
- **HDV MOV/MKV under damage.** Done: the close-time remux is gone. `sink_hdvmux.c` writes
  live and re-encodes damaged GOPs. `test_hdv_repair` covers dropped, garbled and
  error-flagged TS packets, lost audio, cut starts and ends, closed GOPs, a clock jump,
  720p, damage everywhere and a repair that falls behind; `test_replay_hdv_mov` covers the
  whole capture. Still to test with a camera's damaged tape (HW) and in DaVinci Resolve.

## A FireWire PC as a test source

A computer with a FireWire port, connected to the MovieBox's 1394 port, can stand in
for a camera. This sits between "pure code" and "real camera". It exercises the real
USB, FPGA and 1394 path with input we control, so we can produce formats nobody owns a
camera for:
- 720p, 1080i60, 24p, pulldown;
- unusual PIDs and multi-packet PSI;
- mid-stream DV/HDV and PAL/NTSC switches;
- 4:3/16:9 and audio-mode changes;
- gaps.

**What the code tests cover now, and what is left for this rig.** Since this round
`tests/make_ep88.c` builds the EP 0x88 stream the device would send for any sequence of
DV and TS inputs, and the reassembler and replay tests run mixed tapes (DV/HDV,
PAL/NTSC, an audio change, an HDV PID change), 720p and GOP timecode jumps through the
reassembler and, most of them, the session. The logic of most switches is therefore
tested without a sender. What only a sender can show:
- that the real USB, FPGA and 1394 path delivers such streams as `make_ep88` models them
  (a format switch on a running channel, the empty packets, the receive ring);
- timing: a sender running in real time, pauses and gaps (3.11), a sender that stops
  and restarts (bus reset, 4.9);
- the formats nobody owns a camera for, through the device: 720p, 1080i60, 24p and
  pulldown (1.4, 2.1), other PIDs and multi-packet PSI (1.5);
- the deck and plug logic over a real bus (section 4).

**What the sender has to do, from our side:**

- **Stream only.** If no node answers the oMPR read, we listen on broadcast channel 63
  anyway (`pinnacle_stream_start()`). So a sender that transmits DV (IEC 61883-2) or
  MPEG-TS (IEC 61883-4) on **channel 63** should be captured with no plug emulation.
  This is the simplest setup; verify it first.
- **With plug emulation.** If the sender exposes oMPR/oPCR registers, we connect to it
  like a camera, which also tests `p1394_connect` and the channel taken from the plug.
- **Deck control.** This needs software on the PC that answers AV/C FCP commands like
  a tape subunit. With a scriptable responder, the deck findings in section 4 can be
  tested over the real bus. Examples: NOT_IMPLEMENTED, odd transport states,
  INTERIM, slow replies, another subunit address.

**Candidate software. None of this has been tried; check before relying on it.**

- **Linux on the PC or the old Mac** (most flexible; a live USB is enough):
  - `libiec61883` (and its `plugctl` / `plugreport` tools) can transmit DV and
    MPEG-2 TS, and can emulate local plug registers.
  - Its example programs (`test-dv`, `test-mpeg2`) have a transmit mode.
  - The kernel's `firewire-core` character device can also take FCP requests, so a
    small scripted AV/C responder (a fake deck) is feasible.
- **Old Mac (OS X)**: Apple's FireWire SDK had AVCVideoServices with DV and MPEG-2
  transmitter samples and virtual tape-device samples (a virtual D-VHS / MPEG tape
  recorder that answers AV/C). If it still builds and runs on that machine, it is a
  ready-made source for both streams and deck replies. Older editors' "print to video"
  / FireWire output also sends DV.
- **Windows**: the inbox DV driver (MSDV) can send DV out through DirectShow (editors'
  "export to tape", WinDV-style tools). HDV/MPEG-TS out and AV/C target emulation are
  much harder on Windows. Use it for DV stream tests only.

**Making the content:** ffmpeg can produce all of it, and `tests/make_fixtures.cmake`
already has the commands for the DV and HDV-like pieces the tests use:
- DV: PAL and NTSC, 4:3 and 16:9, 32 kHz and 48 kHz, 24p telecined to 29.97.
- HDV-like MPEG-2 TS: 1440x1080 or 1280x720, with set PIDs, set
  `frame_rate_code` and progressive flags, MP2 audio.

Mixed tapes are files concatenated at the frame or TS-packet level, in the order
`make_ep88` takes them. Each output can be checked against the input byte for byte,
which a real tape never allows; `reassembler_ep88_*` and `test_reassembler_mixed`
already do that on the synthetic dumps, so the same checks carry over.

**Limits:** no real tape errors, dropouts, head-clog patterns or camera quirks; the
sender's timing is a PC's, not a camera's. Use it for formats, switches and protocol
logic; use real cameras and tapes for compatibility and error behaviour.

### A second Marvin as the source

Instead of a FireWire PC, a second Pinnacle unit could do the sending: two
MovieBoxes joined by a FireWire cable. Everything marked **FW-PC** above would then
apply. None of this has been tried.

**Why it should be possible:**
- The FPGA design is a standard OHCI-1394 controller ([hardware.md](hardware.md)).
  OHCI has isochronous transmit (IT) contexts as well as receive contexts.
- The USB interface has an OUT endpoint for this. EP 0x06 OUT is in alt 1, the
  DV/HDV setting, and alt 2 is "output to the camera (not used)".
- The vendor driver exposes the unit to Windows as an ordinary 1394 bus
  (`MarvinBus64.sys`, [windows-driver.md](windows-driver.md)), so the
  standard Windows DV stack can send through it. The analog "Render" bitstream is
  output to a TV, not 1394, and is not involved.

**Why it would beat a FireWire PC:**
- The whole rig is two USB devices on one host. It could run unattended on the
  Linux host, from a script, as a hardware regression test.
- The sender would be our own C code, so it could send exactly what the tests
  need: generated DV and TS, format switches, gaps, and other channels.
- Much of the sending side exists already: `tests/make_ep88.c` builds the IEC 61883
  packets a camera puts on the bus (DV packets of 6 DIF blocks with an empty packet
  after every 15, TS as 192-byte source packets, a continuous DBC across inputs, back to
  back for format switches). A transmitting unit would send those packets; the rest of
  what `make_ep88` writes (OHCI receive records, type-9 messages) is what the receiving
  unit adds.
  Missing for a real sender: pacing to the 8 kHz cycle and real SYT/SPH timestamps
  (`make_ep88` writes SYT 0xFFFF and synthetic SPH values, which our receiver ignores but
  a camera or decoder may not).
- The receiving unit's reassembled output should then equal the input files byte for
  byte, which the reassembler and mixed-format tests already check on the synthetic
  dumps: the same tests would run on hardware.
- Our link layer already receives and answers incoming async requests (the AR
  request context, `p1394_answer_owed`). So the sending unit could also answer
  oMPR/oPCR reads (plug emulation) and AV/C commands from a script. That is the
  fake deck for section 4 (NOT_IMPLEMENTED, INTERIM, odd transport states, slow
  replies), with no other software.
- Transmit is also what a future "write back to tape" feature would need.

**What is unknown, and how to find out, cheapest first:**

1. **Does the bitstream implement IT contexts at all?** OHCI lets software count
   them: write all ones to `IsoXmitIntMaskSet`, then read the mask back. One
   register poke on the Linux host, with no trace needed.
2. **How do the data and descriptors get into the device?**
   - On receive, descriptors in device RAM point at a buffer, and address bit 23
     routes it to EP 0x88 ([startup.md](startup.md)).
   - Transmit presumably mirrors that, with the host writing packet data on
     EP 0x06 and OUTPUT_MORE/OUTPUT_LAST descriptors reading it. This is a guess.
   - Read it in the Ghidra output of `MarvinBus64.sys` (IT register writes around
     OHCI offset 0x200, and the EP 0x06 pipe), or capture a USB trace of the vendor
     driver sending.
3. **A trace of the vendor driver sending.** On Windows with the Pinnacle driver,
   any DV-out program (an editor's "export to tape", WinDV-style tools) sends
   through the Marvin, recorded with USBPcap. A catch: Windows' DV stack only
   offers output to a DV device it has found on the bus. So the trace needs a real
   camera on that unit, recorded once.
4. **Then the sender itself:** a transmit loop in `src/core` (with `make_ep88`'s
   packet builders moved there as its payload side) and a `--transmit
   file.dv|file.ts` (or test-only) mode, with the receiving unit selected by
   `--device`.

The vendor-driver route also gives a no-code shortcut. With a camera attached, a
Windows PC with the Pinnacle driver is itself a "FireWire PC". But it can only send
DV to a camera it has found, not to another Marvin, so it does not replace step 4.

**Bus and topology notes:**
- Each 500-USB wires only PHY port 0, so two units take the one cable between them.
  A camera on the same bus as well needs a FireWire hub or repeater (a 700-USB may
  have more ports; untested).
- Both units set themselves cycle master in link init (`p1394_link_init()`). That
  takes effect only on the root node, so one of the two will be the cycle master.
  Check that this is stable.
- Bus reset handling then sees a second node with no camera behind it (4.8,
  camera discovery).
- Two DV streams use about 7 MB/s of USB, well within USB 2.0. Separate host
  controllers are still safer.

**Limits:** both ends are the same FPGA and PHY. A fault common to both, such as a
framing or timing assumption, could cancel out, and nothing about other makers'
1394 chips is learned. It is S100 only. Real cameras and tapes stay necessary for
compatibility and tape-error behaviour.

## Suggested order

For what is left after this round:

1. **HW, cheap, with the Canon HDV camcorder used so far (if it is at hand):**
   - play a recorded tape in VCR mode: does the GOP timecode run (3.9, 4.4)? Does a
     recording start on a closed GOP (3.8)?
   - record short optional fixtures: its aux stream (1.11), a real DV and HDV EP 0x88
     dump (section 6).
2. **CODE, cheap:**
   - the fake AV/C link, then the confirmed bug in 4.1 and the rest of section 4;
   - the missing end-to-end replay tests: HDV 1080i -> 720p through a capture (3.7), a
     4:3/16:9 DV change (3.5), `--split` with a format change (3.10), a pipe through a
     PAL/NTSC change and MOV/H.264/HEVC across a change (3.4), HDV onto a full drive
     (5.3);
   - more synthetic HDV: 1080i29.97 and bottom-field-first streams, 720p50, 1080p,
     split PSI, a PMT version change, long PES headers (1.1, 1.5-1.7);
   - CI: a Linux push job with ctest, and sanitizer builds;
   - `pin_hdv_audio` tests (1.9), the PAL timecode range (2.4).
3. **Decide:** the points in 3.15 (a bit-depth-only change; the per-kind options after
   a DV/HDV change are decided: aspect and title per kind, the rest from the start kind),
   what NOT_IMPLEMENTED means (4.1), the zero GOP timecode
   (3.9, after step 1), and when to take up 24p (1.4, 2.1, 2.2).
4. **FW-PC** (first check whether a second Marvin can transmit, step 1 of that section):
   - check stream-only capture on channel 63;
   - then the mixed DV/HDV and PAL/NTSC streams `make_ep88` already models, through
     the real path, compared byte for byte;
   - then 720p, 1080i60, other PIDs and gaps, and 24p when it is taken up;
   - then a scripted fake deck, if available.
5. **HW:**
   - the camera matrix (4.10), at least one Sony and one Panasonic/JVC;
   - a JVC 720p source;
   - a real mixed DV/HDV tape (what the camera sends at the switch: a bus reset, a gap,
     garbage) and a PAL/NTSC tape;
   - LP, 12-bit, 4-channel and 16:9 DV tapes (keep a short recording as a fixture);
   - a FAT32 stick, a nearly full drive and a slow one;
   - an 8-hour run per OS;
   - unplug, bus reset and sleep;
   - the untested Pinnacle models and hosts;
   - a DVX100 24p/24pA tape and a Canon 24F tape, when 24p is taken up.
