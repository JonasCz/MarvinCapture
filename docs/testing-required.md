# What still needs testing

What has not been tested yet, grouped by the input that can test it: a kind of camera or
tape, a FireWire PC sending a stream we make, a drive, a host, or code alone. Everything
under one input can be checked in one sitting with that input.

Most of what is open needs a FireWire source: a camera or deck playing a particular kind
of tape, or a computer (or a second Marvin) acting as one. Much of the logic behind those
items already runs in ctest on synthetic data (ffmpeg-made recordings, EP 0x88 dumps built
by `tests/make_ep88.c`, the replay device). Such items are listed under the real input they
have never seen, with the tests that cover the logic.

## How to read this

- **Tested in code** names the ctest tests that cover the logic on synthetic data. What is
  left is the real input.
- **Known fault**: reading the code shows it does the wrong thing for that input. Not yet
  seen on hardware.
- **Unknown**: what real equipment does is not known, and the code makes an assumption.
- **Also from a sender**: a FireWire PC or a second Marvin can produce this input too
  ([section 7](#7-a-firewire-pc-or-a-second-marvin-as-the-source)).
- An item that several inputs can test is listed under the most common one, with a pointer
  from the others.
- Code is referred to by file and function rather than by line number.

## Verified on hardware

- **HDV**: one Canon HDV camcorder, 1080i/25 (1440x1080, PIDs video `0x810`, audio
  `0x814`, PMT `0x81`), on a 500-USB and a 510-USB: capture to raw `.m2t` / `.ts` and to
  standard output, deck control, timecode while winding ([hdv.md](hdv.md),
  [deck-control.md](deck-control.md)).
- **DV**: one DV camcorder (make and model not recorded) with a short tape, on a 510-USB
  under Windows; one camcorder in "AV to DV" passthrough for 60 minutes.
- **Analog**: PAL composite, PAL S-video, NTSC composite ([analog.md](analog.md)).
- **Units**: 500-USB (`0213`) and 510-USB (`0223`). **Hosts**: Windows x64, Apple Silicon
  macOS, Linux x86_64.

## Inputs at a glance

| Input | What it tests | Section |
|---|---|---|
| The Canon HDV camcorder (1080i/25) | HDV MOV / MKV from a real tape, scene split on a real tape, closed GOPs, the aux stream, real fixtures | [1.1](#11-hdv-1080i25-the-canon-hdv-camcorder) |
| A 60 Hz HDV camera (1080i/29.97) | the 29.97 format, drop-frame GOP timecode | [1.2](#12-hdv-1080i2997-a-60-hz-hdv-camera) |
| A JVC 720p camera or deck | other PIDs and PMT layouts, 720p50/59.94, GOP timecode at 50/60 | [1.3](#13-hdv-720p-a-jvc-prohd-or-hdv-camera-or-deck) |
| HDV in 24p, 25F or 30F | pulldown flags, frame modes | [1.4](#14-hdv-24p-25f-and-30f) |
| Any DV camcorder or deck | error classification on live tape, a real DV dump | [2.1](#21-any-dv-camcorder-or-deck) |
| 16:9 DV | aspect detection, a 4:3 / 16:9 change mid-tape | [2.2](#22-169-dv) |
| LP, 12-bit or 4-channel DV | 32 kHz 12-bit audio, a change of audio mode mid-tape | [2.3](#23-dv-with-32-khz-12-bit-or-4-channel-audio) |
| DV 24p (DVX100, Canon 24F) | 24p and 24pA pulldown in 29.97 DV | [2.4](#24-dv-24p) |
| DVCAM, DVCPRO25, Digital8 | the variants and their decks | [2.5](#25-dvcam-dvcpro25-digital8) |
| A tape with DV and HDV, or PAL and NTSC | what a camera sends at the switch (also MarvinPlayout's guess); the files | [3](#3-tapes-that-mix-dv-and-hdv-or-pal-and-ntsc) |
| A tape with gaps, blank stretches, stills | gaps, blank tape, REC START, frozen timecode | [4](#4-tapes-with-gaps-blank-stretches-and-stills) |
| Cameras and decks from other makers | deck control over AV/C | [5](#5-deck-control-cameras-and-decks-from-other-makers) |
| Any FireWire source, plus a second node | bus reset, plugs and channels, two nodes, S400 | [6](#6-the-firewire-bus) |
| A FireWire PC or a second Marvin | streams and AV/C replies no camera gives on demand | [7](#7-a-firewire-pc-or-a-second-marvin-as-the-source) |
| Analog sources | other standards, a standard change, signal loss, long runs | [8](#8-analog-sources) |
| Drives (any input) | FAT32, a full or failing drive, 8-hour runs | [9](#9-drives-and-long-captures) |
| Other Pinnacle units | 700-USB, 710-USB, MovieBox Deluxe, cold start | [10](#10-other-pinnacle-units) |
| Other hosts | Android on a phone, Windows on Arm, Intel macOS, USB controllers, sleep | [11](#11-hosts-operating-systems-and-usb) |
| Each GUI, built | aspect, title and help texts, HDV MOV / MKV options | [12](#12-the-guis) |
| No hardware | what is left to test in code | [13](#13-no-hardware-in-code) |
| Code review | data loss and wrong counts found by reading the code: fixes and tests | [17](#17-code-review-data-loss-and-wrong-counts) |

Open decisions are in [section 14](#14-open-decisions), how to set up a sender in
[section 15](#15-setting-up-a-sender), a suggested order in
[section 16](#16-suggested-order), and bugs found by reading the code (data loss, wrong
counts) in [section 17](#17-code-review-data-loss-and-wrong-counts).

---

## 1. HDV sources

The format is read from the stream ([hdv.md](hdv.md#format-what-the-stream-says)): size,
frame rate (`frame_rate_code` with the sequence extension), interlaced or progressive
(`progressive_sequence`), field order (`top_field_first`) and aspect
(`aspect_ratio_information`). Until the first sequence header, 1080i/29.97 16:9 top field
first is assumed. Tested in code with ffmpeg-made 1080i/25 (`hdv.ts`, the Canon's PIDs) and
720p/59.94 (`hdv-720p.ts`, ffmpeg's PIDs) streams: `test_replay_hdv`, `test_pin_vidfmt`,
`test_hdv_rewrap`, `test_hdv_repair`.

### 1.1 HDV 1080i/25: the Canon HDV camcorder

The camera used so far. Everything here needs only it and a few recorded tapes.

- **HDV MOV / MKV from a real tape.** `sink_hdvmux.c` writes these live: it decodes every
  GOP to check it, passes undamaged GOPs through, re-encodes damaged ones from the decoded
  pictures, repeats lost pictures, puts silence where audio is lost, and counts as errors
  the frames it re-encoded or silenced ([hdv.md](hdv.md#mov-and-mkv-written-live-damaged-gops-repaired)).
  Tested in code: `test_hdv_repair`, `test_hdv_stages`, `test_replay_hdv_mov`,
  `test_pin_closer`. It has not run with a camera. Check:
  - a clean tape: no errors, files as long as the recording, decoding from the first frame;
  - a worn tape with dropouts: the damaged GOPs repaired, the error count matching what is
    visible on a raw `.m2t` of the same stretch;
  - the files in DaVinci Resolve (and Final Cut or Premiere if at hand): the `hdv3` sample
    description in MOV, the sequence header as CodecPrivate in MKV;
  - the CPU it takes on a slow computer and on a phone (Android offers it). A host that
    cannot keep up stops the capture with an error (`reencode_behind`);
  - the HDV tab's aspect setting (`aspect_hdv`) in the file.
- **Scene split on a real tape** (`--split` on raw `.m2t`; MOV and MKV always split). A
  break is a GOP timecode jump or a jump of the camera clock (the DTS of the video PES that
  opens each GOP), settled over three GOPs and cut at the first picture of the new
  recording ([hdv.md](hdv.md#scene-split---split)). Tested in code: `test_replay_hdv_split`,
  `test_pin_scene`, `test_pin_holdback`, `test_replay_hdv_mov`. Record a tape with several
  short recordings, some paused and resumed, and check:
  - **zero GOP timecode**: this camera writes 00:00:00:00 in every GOP header
    ([deck-control.md](deck-control.md)), and a standing zero timecode never splits
    (`test_hdv_zero_tc_never_cuts`). On its tapes only the camera clock can split. Unknown:
    whether the clock jumps at a new recording on tape. If it runs on, nothing splits.
    Also check whether the timecode is zero both in camera mode and when playing a tape;
  - **closed GOPs**: unknown whether a recording starts with a closed GOP or sets
    `broken_link`. An open GOP's leading B pictures do not decode at the start of a new raw
    `.m2t` (MOV and MKV make the first GOP closed);
  - GOPs without a sequence header (the file then decodes from the next one), and the
    three-GOP debounce against real dropouts.
- **The aux stream (PID `0x811`).** `hdv_aux_scan_datetime()` reads a recording date and
  time and the drop-frame and colour-frame flags from it. Its reading of the packs is a
  best guess (`hdv_aux.h`), tested only on a hand-built pack (`test_hdv_aux`), and no
  capture code calls it. Keep a short capture as an optional fixture in `tests/data`. If
  the date is there, it could split scenes on cameras with a zero timecode.
- **Title in a raw `.m2t`.** The title goes in as the DVB service name, an SDT packet
  behind the PAT and PMT at each file start (`hdv_emit_psi()`); libavformat reads it back
  in `test_replay_mixed_kind`. Check a camera's `.m2t` in MediaInfo ("Service name") and
  that editors open it normally with the extra packet.
- **A real EP 0x88 dump.** The generated dumps follow `make_ep88`'s model of the endpoint
  ([protocol.md](protocol.md)), and no test runs on a real one. Keep a short HDV dump
  (and a DV one, 2.1) as optional fixtures.
- **Deck control in camera mode**: what the transport commands answer when the camera is
  not in VCR mode (NOT_IMPLEMENTED, see 5).
- **DV on the same camera**: if it records DV as well, it makes the mixed DV/HDV tape of
  [3.1](#31-dv-and-hdv-on-one-tape) and a DV source for 2.1.

### 1.2 HDV 1080i/29.97: a 60 Hz HDV camera

Never seen from a camera. 1080i/29.97 and bottom field first exist only as hand-built
headers in `test_pin_vidfmt`, not as a stream. Check:

- the status shows 1080i/29.97 (15-picture GOPs), and the field order is right;
- raw `.m2t`, MOV (`hdv2`) and MKV: size, rate and field order in the file;
- **drop-frame GOP timecode**: the label shows `;`; the scene detector's drop-frame
  correction (`tc_frame_count()` in `pin_scene.c`) across a minute boundary;
- the deck's TIME CODE in drop frame, and `--wait timecode=` against it (the NTSC
  drop-frame deck path);
- the PIDs and PMT layout, if the camera is not a Canon (1.3).

Also from a sender: ffmpeg can make 1080i/29.97 streams, top and bottom field first.

### 1.3 HDV 720p: a JVC ProHD or HDV camera or deck

Never seen from a camera. 720p/59.94 from ffmpeg is tested in code (status, square
samples, progressive, MOV and MKV); 720p/50 has no stream test. Check:

- **PIDs and the PMT.** The only camera PIDs seen are `0x810` / `0x814`. The PIDs are
  read, not hard-coded (`hdv-720p.ts` uses `0x100` / `0x101`, PMT `0x1000`), but the
  PAT/PMT parse is single-packet and single-shot (`hdv_scan_pat_pmt()` in `hdv_aux.c`): a
  PSI section split over two TS packets is cut short and never re-read. Only video
  `stream_type 0x02` and audio `0x03` / `0x04` are recognised. The PAT and PMT kept for the
  start of each file (`ts_handle_packet()`, `hdv_psi_scan()`) are one packet each, so a
  multi-packet PMT would leave such files without one. This is the main risk for JVC.
- **The start gate** (`ts_gate_try_open()` in `dv_reassembler.c`) wants `00 00 01 B3` in
  the PES-start packet of the video. A long PES header or user data before the sequence
  header pushes it into the next packet: the gate never opens, nothing is written, and
  there is no message.
- **One picture per PES** is assumed (`ts_handle_packet()`). `hdv_error.c` reads only the
  first picture of a unit, and a second PES start in a unit may count a false
  `pes_errors`.
- 720p/50 and 720p/59.94 in the status, raw `.m2t`, MOV (`hdv1` / `hdv5` / `hdv9` /
  `hdva`) and MKV.
- **GOP timecode at 50 and 60 fps.** The frame field runs to 49 or 59. Unknown how HDV
  cameras number it. The scene detector's drop-frame correction applies only at a nominal
  30 fps (`pin_scene.c`), so 59.94 drop frame is counted without it.
- **720p24 and 720p30**: if carried as a 60p or 50p stream with repeat flags (unverified),
  the rate shown is double the real one (1.4).
- Deck control on a JVC camera or deck (BR-HD50): section 5.
- A short capture as an optional fixture.

Also from a sender: 720p/50 and 720p/59.94 with any PIDs, a PMT across two packets, long
PES headers.

### 1.4 HDV 24p, 25F and 30F

`repeat_first_field` is not read. 24p carried with 2:3 pulldown flags (Sony, Canon 24F)
shows as 1080i/29.97, and frame modes such as 25F and 30F as 1080i. `captured_seconds()`
(the length in the stop message), `unit_seconds()` and the scene split count pictures at
the coded rate, and HDV MOV / MKV time each picture as one frame ([hdv.md](hdv.md)).
Unknown what JVC, Sony and Canon write in these modes. With a tape in each mode, check the
status, the length in the stop message against the real one, the scene split, video and
audio length in MOV and MKV, and that the raw `.m2t` plays right. What to do about 24p is
an open decision (14).

Also from a sender: ffmpeg can make a TS with pulldown flags.

## 2. DV sources

### 2.1 Any DV camcorder or deck

- **Record the make and model** of the DV camcorder used so far, and of each new one
  (section 5 lists what else to record).
- **Error classification on a live tape.** The frame error rules (`dv_error.c`,
  [usage.md](usage.md)) were checked against files only. The Sony (muting by zeros) and
  Samsung (every block STA 14) quirks come from files, not cameras. Compare a worn tape's
  error count with what is visible.
- **A real EP 0x88 dump** as an optional fixture (1.1).
- PAL and NTSC tapes, to raw `.dv`, DV AVI, DV MOV and H.264 / HEVC.

### 2.2 16:9 DV

16:9 is read from VAUX DISP codes 2 and 7 only; `test_dv_subcode` covers only `0x02` and
a missing pack, and there is no 16:9 DV fixture. Check:

- a 16:9 recording: the status, the preview and each format's aspect (raw `.dv` carries
  the camera's flag; DV AVI, MOV and H.264 / HEVC store their own);
- **a tape with 4:3 and 16:9 recordings.** A change counts after 5 frames and starts a new
  DV AVI / MOV / H.264 / HEVC file at its first frame, unless `--aspect` forces one; raw
  `.dv` and a pipe carry it per frame (how a change is handled: section 3). Tested in code
  only in `test_pin_fmtwatch`.

Also from a sender: ffmpeg makes 16:9 DV.

### 2.3 DV with 32 kHz 12-bit or 4-channel audio

LP recordings and many consumer cameras' 12-bit mode. Nothing in the code or docs treats
LP specially; LP tapes generally show more dropouts. ffmpeg's DV muxer writes 16-bit
stereo only, so none of this can be made synthetically. Check:

- 32 kHz 12-bit nonlinear audio in every DV format, and in the preview;
- 4-channel recordings (the second stereo pair);
- **a tape whose audio mode changes** (12-bit and 16-bit sections, a second stereo pair
  added later). A change of rate, channels or bits starts a new DV AVI / MOV / H.264 /
  HEVC file; raw `.dv` goes on. Tested in code: 48 kHz then 32 kHz, both 16-bit stereo, as
  DV AVI (`test_replay_mixed_dv`). The preview resamples per frame (perhaps a click);
- keep a short 32 kHz 12-bit recording as the optional fixture `tests/data/dv-ntsc-32k.dv`
  (`tests/data/README.md`). With it, mixed-audio dumps can be built for code tests.

### 2.4 DV 24p

Panasonic DVX100 24p and 24pA, Canon 24F in DV. Such tapes are 29.97 DV with 2:3 or
2:3:3:2 pulldown. They are captured as 29.97i bottom field first (`open_sink_for_scene()`);
no VAUX bits beyond the aspect are read, and removing the pulldown is not offered.
Capturing them as plain 29.97 loses nothing and is probably the right default, but
[usage.md](usage.md) does not say so. Check that such tapes capture cleanly.

Also from a sender: ffmpeg can telecine 24p material into 29.97 DV.

### 2.5 DVCAM, DVCPRO25, Digital8

Never tested, as sources or as decks. Check capture in every DV format and deck control
(section 5). DVCAM and Digital8 decks may answer AV/C slowly (the fixed timeouts in 5).

## 3. Tapes that mix DV and HDV, or PAL and NTSC

How a capture follows a change ([usage.md](usage.md#running-a-capture),
[hdv.md](hdv.md#tapes-that-mix-dv-and-hdv-or-hdv-formats)):

- **Reassembler.** It switches between DV and HDV after 256 data packets of the other
  format in a row (`DV_FMT_SWITCH_PACKETS`); a shorter run is dropped and counted
  (`fmt_dropped_packets`). Each DV frame is sized by its own evidence (`std_switches`
  counts changes of the confirmed standard). A new HDV video PID is followed after two
  GOPs (`ts_pid_switches`); a picture past 4 MB is dropped (`ts_pic_overflows`).
- **Session.** `pin_fmtwatch` watches the kind; DV standard, aspect, audio rate, channels
  and bits; HDV size, frame rate and `progressive_sequence`. A change counts after 5 DV
  frames or 3 HDV GOPs in a row and is back-dated to its first unit (`pin_holdback`).
  `pin_format_change_action()` then decides:

| Output | New file on |
|---|---|
| raw `.dv` | PAL <-> NTSC only (aspect and audio are per frame) |
| DV AVI / MOV / H.264 / HEVC | standard, aspect (unless `--aspect` forces one), audio rate, channels or bits |
| HDV `.m2t` / `.ts` / MOV / MKV | size, frame rate, interlaced/progressive |
| any file | DV <-> HDV: a new file in the other kind's format and extension |
| standard output | never a new file; DV <-> HDV ends the capture (`PIN_STOP_ERROR`; the CLI exits 4) |

New files are numbered `name-0002.ext`, ... (with `--split`, the next scene number). The
status shows the confirmed format, and `PIN_EVT_INPUT_FORMAT` is sent on a change. After a
DV <-> HDV change each file takes its own kind's aspect (`aspect` / `aspect_hdv`) and the
title; scene split, idle stop, maximum duration and passes follow the kind the capture
started with. Tested in code with synthetic dumps: `test_reassembler_mixed`,
`test_replay_mixed_kind`, `test_replay_mixed_dv`, `test_pin_fmtwatch`. Never with a real
tape.

### 3.1 DV and HDV on one tape

A tape recorded in both modes, played in an HDV camera or deck (the Canon, if it records
DV). Check:

- **what the camera sends at the switch**: a bus reset (which ends the capture with
  `PIN_STOP_CAMERA_LOST`, section 6), a gap, or a burst of garbage. Unknown. Note: how long the gap
  is, whether empty packets of the new format follow, and when the oPCR payload and the AV/C
  OUTPUT PLUG SIGNAL FORMAT answer change;
- **MarvinPlayout guesses this** (`src/playout/pout_switch.c`, [playout.md](playout.md#tapes-that-mix-formats)):
  DV <-> HDV: no packets for 1.0 s, then 0.2 s of empty packets of the new format, no bus reset, the
  IT context restarts. **Once a real camera has been observed (bus reset? gap length? garbage?
  oPCR/format change timing?), change the playout's defaults to match it** (and use `--switch` to try
  a measured value first);
- the files: each in its tab's format, with its tab's aspect and the title;
- to a pipe: the capture ends at the switch with the reason, the CLI with exit code 4.

Also from a sender: send DV, then HDV, on the same channel.

### 3.2 PAL and NTSC on one DV tape

Rare, but it is what a multi-standard deck or a re-used tape produces. Check the new file
at the change in every DV format, and a pipe that goes on through it. Untested even in
code: damaged frames right at the switch, which take the confirmed standard.

MarvinPlayout guesses this too: no gap, 0.5 s of empty packets of the new standard, no bus reset,
no restart (`pout_switch.c`, `std`). Once a real deck has been observed (does a multi-standard deck
reset the bus, stop the stream, change the FDF at once?), change the playout's defaults to match it.

Also from a sender: PAL then NTSC DV back to back.

### 3.3 An HDV format or PID change within a tape

Unknown whether any camera changes the video PID, or switches between 1080i and 720p,
within a tape. If one does: the picture in progress and the new PID's first GOP are lost,
and a new file starts at the GOP where the change began, with PAT and PMT in front.

MarvinPlayout guesses this too: no packets for 0.5 s, then 0.2 s of empty packets, no bus reset, no
restart (`pout_switch.c`, `hdv-format`). Once a real camera has been observed, change the playout's
defaults to match it.

Also from a sender: 1080i then 720p on another PID (`ep88-hdv-pid.bin` models it).

## 4. Tapes with gaps, blank stretches and stills

Any DV or HDV camera, with a tape that has unrecorded gaps, pauses and stills. Check:

- **DV at a gap**: a partial frame is emitted zero-padded (counted damaged) when data
  resumes.
- **HDV at a gap**: a partial picture is glued to whatever arrives after the gap (a
  picture past 4 MB is dropped). After a gap of more than 1 s, `dv_on_unit()` only
  restarts the HDV error tracking. The start gate is re-armed per pass, not per gap.
- **Blank tape**: a capture that starts on blank tape never gets a first frame and stays
  "capturing"; the end of the tape shows only through the no-signal timeout
  ([deck-control.md](deck-control.md)). The no-signal timeout does not run before the
  first frame (by design: the user may still have to press play); Stop works while
  waiting (`test_replay_stop_waiting`).
- **DV frames lost at a dropout**: whole frames the DBC says never arrived count as
  dropped, partial ones as error frames (`test_reassembler_loss`, `test_replay_dv_loss`).
  Check the counts against a real dropout. A hole of about 26 NTSC frames or more wraps the
  8-bit DBC and is miscounted.
- **HDV at a blank stretch or dropout of more than 1 s**: the scene detector expects the
  camera clock and GOP timecode to jump, which starts a new file with `--split` and in HDV
  MOV / MKV. Check that a real tape does this. A jump that does not split (a scene under
  the 1 s / 1 MB minimum, or raw `.m2t` without `--split`) is taken up without a gap and
  not counted (`sink_hdvmux.c` `gop_decide()`).
- **Scene split around REC START and stills**: a recording with REC START but no timecode
  and no date (probably never splits), a frozen timecode at pause or still, small forward
  timecode jumps. Unknown what cameras send there.

Also from a sender: pause and resume the stream, cut it mid-picture.

## 5. Deck control: cameras and decks from other makers

Tested: one Canon HDV camcorder and one DV camcorder. Never tested: Sony, Panasonic, JVC,
Samsung, Sharp; dedicated decks (Sony DSR and GV-D, Panasonic AG-DV, JVC HR-DVS, Sony
HVR-M15 / M25, JVC BR-HD50). The deck code is in `pin_deck.c` and the deck parts of
`pin_session.c` ([.claude/skills/deck-capture-flow](../.claude/skills/deck-capture-flow/SKILL.md)).

- **Known fault: NOT_IMPLEMENTED is treated as "busy" and ends as success.** For transport
  opcodes a `0x08` reply is retried up to 5 times, 0.7 s apart, and the async path then
  returns DONE: only `0x0a` REJECTED fails (`pin_deck_async_poll()`). The "busy" reading
  comes from one Canon. On a deck that really does not implement a command (FF or PLAY in
  camera mode), the app waits about 3.5 s and then believes it was sent: a rewind-first
  capture waits for a rewind that never started, a STOP is reported done.
- **Transport-state mapping** (`pin_deck_state_from_avc()`). Five codes have been seen;
  `c3 6d`, `c3 65` and `c4 45` are guesses. Any other `c3` mode reads as PLAYING (slow,
  reverse, frame advance); `c1` (no tape) and `c0` as UNKNOWN. A wrong "stopped" ends
  rewind-first or multi-pass early.
- **The VCR subunit is fixed at `0x20`.** SUBUNIT INFO and UNIT INFO are never sent. A
  device whose tape subunit is elsewhere ignores every command, which then times out.
- **Camera mode vs VCR mode** is not detected.
- **Timeouts.** Sync commands wait 3 s (10 s after INTERIM); async commands fail after 3 s
  with no resend, so a lost STOP stays lost. Replies are matched by subunit and opcode,
  not ctype. A deck-state query that keeps failing is ignored: there is no "deck not
  responding" status (the poll in `dv_tick()`).
- **Timecode from the deck.** ABSOLUTE TRACK NUMBER is not used, so a deck that reports
  only ATN or a counter gives no timecode; `--wait timecode=` against such decks. No seek
  (neither tested camera implements `51 20` or `52 20`): check whether decks do.
- **The camera matrix.** For each new camera or deck, record in
  [deck-control.md](deck-control.md): the UNIT INFO and SUBUNIT INFO answers; the
  transport-state bytes in every mode; the TIME CODE answers in camera and VCR mode; the
  oMPR / oPCR values; a `--debug` AV/C log; a short capture of each format it records
  (kept as optional fixtures). Priority: a Sony DV or DVCAM deck; a Panasonic and a JVC
  camcorder; a Sony HDV deck (HVR-M15 / M25); a JVC 720p HDV camera; a Digital8
  camcorder.

Also from a sender: a scripted fake deck (NOT_IMPLEMENTED, odd transport states, INTERIM,
slow replies, another subunit address).

## 6. The FireWire bus

Any FireWire source; some items need a second device on the bus.

- **Bus reset during a capture** ends it with `PIN_STOP_CAMERA_LOST` ("the FireWire
  connection to the camera was interrupted") and restarts the stream as when idle
  ([hardware.md](hardware.md)). Never seen: move the cable, switch the camera off and on,
  load a tape, let it power-save. A capture still waiting for its first frame keeps
  waiting.
- **Plugs and channels.** Only `oPCR[0]` is connected (compare-swap, up to 4 retries).
  Nothing is allocated from an isochronous resource manager. If no node answers the plug
  read, the device listens on broadcast channel 63 (`pinnacle_stream_start()`). A camera
  that wants an allocated connection, has several output plugs, or moves channel after
  connecting has not been seen.
- **Two nodes on the bus** (a camera and a deck, a camera and a PC). Camera discovery
  takes the first node that answers oMPR (`p1394_find_camera()`).
- **The CIP check** (`iso_header_ok()` in `dv_reassembler.c`) accepts DV, and MPEG-TS
  with DBS 6 and a payload in 192-byte source packets. FN, SPH and QPC are not checked. A
  sender with another layout is dropped as noise, with no message. S200 and S400 senders
  (S400 HDV cameras) have never been seen.

## 7. A FireWire PC or a second Marvin as the source

A sender exercises the real USB, FPGA and 1394 path with input we control and can compare
byte for byte, which a real tape never allows. Setup: [section 15](#15-setting-up-a-sender).
What only a sender can show:

- **Stream-only capture on channel 63**, with no plug emulation. The simplest setup;
  check it first.
- **The real path against `make_ep88`'s model**: the mixed tapes the code tests use
  (DV / HDV / DV, PAL / NTSC, 48 kHz / 32 kHz, the HDV PID change), sent in real time and
  compared byte for byte with the input. `reassembler_ep88_*` and `test_reassembler_mixed`
  already do this on the synthetic dumps, so the same checks carry over.
- **Formats nobody owns a camera for**: 1080i/29.97 and bottom field first (1.2), 720p/50
  and 720p/59.94 (1.3), 1080p/25 and 1080p/30, 24p with pulldown in HDV and DV (1.4, 2.4),
  16:9 DV (2.2).
- **Odd transport streams** (1.3): a PSI section across two TS packets, a PMT version
  change, a new audio PID on the same video PID, other stream types, a sequence header
  outside the PES-start packet, two pictures in one PES, a PES length of 0.
- **HDV audio other than 48 kHz stereo MP2**: 44.1 and 32 kHz, mono, more than two
  channels, MPEG-2 audio. Only MPEG-1 Layer II is recognised. The monitor
  (`pin_hdv_audio.c`) handles 44.1 / 32 kHz and mono but has no test. The sinks are told
  48 kHz stereo (`open_sink_for_scene()`); HDV MOV / MKV read the rate and channels from
  the MP2 frames; raw files keep the audio as sent.
- **Timing**: pauses, gaps and a stream cut mid-picture (4); a sender that stops and
  restarts (bus reset, 6).
- **Plug emulation** (oMPR / oPCR registers): `p1394_connect()`, the channel taken from
  the plug, a plug that changes under us.
- **A fake deck** answering AV/C (5): NOT_IMPLEMENTED, odd transport states, INTERIM,
  slow replies, another subunit address.
- **S200 / S400**, if the sender allows it (6).

## 8. Analog sources

- **Standards never tested**: NTSC S-video, PAL-M, PAL-N, PAL-60, NTSC-4.43, NTSC-J,
  SECAM. The vendor's NTSC audio packet size is unknown; ours is 1600 samples, and any
  size works ([analog.md](analog.md#not-done-yet)).
- **The standard changes during a capture** (a VCR tape with PAL and NTSC). The analog
  restart happens only in READY (`analog_tick()`); geometry and audio packet size are fixed
  at the start. With `--std auto` the status shows the new standard while the file keeps
  the old geometry. The format watch of section 3 covers DV and HDV only.
- **The signal drops or returns mid-capture** ([analog.md](analog.md)). Unknown whether
  audio keeps coming while video stops. The assembler fills lost video black (a gap of a
  second or more; 250 frames or more only while audio ran on) and lost audio with silence,
  and leaves a gap in both streams unfilled (`test_analog_assembler`). Check on the device
  that a signal loss is not filled and a host stall is, and that A/V stays in sync.
- **Weak or jittery sources** (worn VHS) with `--std auto`.
- **Past the 16-bit packet counters' wrap** (every 65,536 frames: 43.7 min PAL, 36.4 min
  NTSC; [usage.md](usage.md)): a capture of an hour or more, audio checked to the end. No
  test covers the wrap.
- **The 4 GiB FAT32 limit** (9): an analog capture stops there with `DISK_FULL` and the
  file finished; no test runs it.
- **A recorded analog EP dump** as an optional fixture. The analog stream code
  (`pinnacle_analog.c`, `pinnacle_stream.c`) has no tests and no replay; with a dump, its
  repair and reassembly could be tested in code.

## 9. Drives and long captures

Any input; DV is the easiest.

- **A FAT32 USB stick.** A DV or HDV capture goes on in a new file before the open one
  would pass 4 GiB (keeping 64 MiB for the index); analog stops (8). The output check
  says so beforehand. Tested in code with `PIN_TEST_FAT32_LIMIT`: `test_replay_fat32`,
  `test_pin_stop`. Untested: the detection itself (`fat32_and_free()`).
- **A nearly full drive, a USB stick pulled while writing, a slow network drive**: what
  each OS reports. Tested in code for DV and pipes: `test_replay_disk`, `test_cli_pipe`.
  Not for analog AVI, which is unreadable by many tools without a clean close (R18), nor
  for analog FFV1.
- **An 8-hour capture**, once per OS.
- **DV AVI past FFmpeg's 1 GiB RIFF segments** (`sink_rewrap`): a DV AVI over about
  5 minutes, read back whole by libavformat and an editor.
- **OpenDML past 1024 segments** (an analog AVI of about 14 hours) is tested in code
  (`test_avi_odml`).

## 10. Other Pinnacle units

- **700-USB (`0212`) and 710-USB / MovieBox Plus (`0224`)**: supported, untested. Their
  extra I2C chip and PHY ports are not driven.
- **MovieBox Deluxe (`0206`)**: the FX2 firmware download, the re-enumeration wait and the
  GUID read are best guesses ([hardware.md](hardware.md)). If the GUID read fails, a fixed
  fallback GUID is used, so two such units on one bus would collide.
- **Cold start**: the warm-start shortcuts (500-USB and 510-USB) are not verified on a
  true power-up.

## 11. Hosts, operating systems and USB

- **Android on a phone**: never run against the box. Only the API 34 emulator with the
  replay device has run ([handoff/android-plan.md](handoff/android-plan.md)). Open: USB
  permission and the attach intent, firmware upload, unplug, thermal behaviour, Android
  11-13 and 15-16, 16 KB page devices, the MediaCodec encoder (the emulator's stopped
  after 18 s, "took no picture for 10 s"), HDV MOV / MKV on a phone's CPU (1.1).
- **Windows on Arm and Intel macOS**: built, never run with the device.
- **USB host controllers** other than Intel and Apple: AMD, ASMedia, Renesas.
- **Hubs**: the hub warning is logic only; Android reports no hub depth, so it never
  fires there.
- **USB stalls**: the 256-transfer queue and 64 MB ring were tuned by hand
  ([usage.md](usage.md)). Check under a busy disk, behind a hub, on a slow host.
- **Unplug** during bring-up, firmware upload, a deck command or while a file is
  finalised; an Android detach. Tested in code: unplug mid-capture through the replay
  device (`test_replay_unplug`, DV and HDV).
- **The CLI on Windows when its console is closed, or at logoff or shutdown**: the handler
  waits until the files are finished, within the OS limit of about 5 s. Unbuilt and
  untested there; a large writer backlog may not fit in that time.
- **Sleep, resume and USB autosuspend**: nothing handles them. A laptop that sleeps
  mid-capture leaves an undefined state (decision: 14).
- **Hardware encoders** (H.264 / HEVC): Media Foundation and VideoToolbox run in the
  ctest suite on Windows and macOS with the replay device; Linux tests use FFV1 as a
  stand-in. Check a live capture with each, and MediaCodec on a phone.

## 12. The GUIs

No capture device needed (the replay device will do). The GUIs have no automated tests.
Build each (Windows, macOS, Android) and check:

- the aspect drop-downs: disabled with the core's tooltip (`pin_format_aspect_help()`)
  for raw `.dv` and `.m2t`, enabled for AVI, MOV and MKV, including the HDV tab's for HDV
  MOV / MKV;
- a DV <-> HDV capture gives each file its tab's aspect and the title;
- the name field's tooltip (`pin_format_title_help()`), the HDV format drop-down's help
  (`pin_format_kind_help()`), and the scene-split option shown checked and disabled for
  HDV MOV / MKV (`pin_format_scene_split_help()`);
- **Windows, built and run** (the code is unbuilt): logoff, restart and shutdown during a
  capture show "A capture is in progress" and block; "Shut down anyway" finishes the file
  before the process ends (`Controls/SessionEndGuard.cs`); without a capture the app exits
  cleanly;
- a file that cannot be written completely at the end of a capture (unplug the drive just
  before Stop) shows the stop dialog and a red taskbar on Windows, the alert on macOS, the
  "Capture stopped" notification on Android, also when quitting during the capture.

## 13. No hardware: in code

- **Deck and bus.** There is no seam for scripted AV/C replies or bus events: the deck
  code calls `p1394_avc*` directly, replay sessions skip deck control, and
  `test_pin_deck.c` covers only the byte builders and parsers. A small fake link layer (a
  function table or a scripted `p1394_avc_*` stub) would let these run on canned replies:
  NOT_IMPLEMENTED (5); a table test of the transport states, with unknown modes never
  reading as stopped; SUBUNIT INFO parsing; timeouts, resends and ctype matching;
  `pcr_adjust` against a fake register pair (a concurrent change, count 0 or 63, a failed
  lock); a bus reset (a SelfIDCount change).
- **HDV streams.** A PSI section split across packets, a PMT version change, another
  audio PID (`hdv_scan_pat_pmt()`); a sequence header outside the PES-start packet; two PES
  starts in a unit, a PES length of 0; 1080i/29.97 and bottom-field-first streams through
  a replay; 720p/50, 1080p/24, 25 and 30; pulldown once decided (14); `pin_hdv_audio.c`
  (no test: 44.1 / 32 kHz, mono); HDV MOV / MKV through a capture at 720p and 1080i/29.97
  (`test_replay_hdv_mov` runs 1080i/25 only); `aspect_hdv` in an HDV MOV / MKV file.
- **Timecode.** GOP timecode at 50 / 60 fps and 59.94 drop frame in the scene detector;
  DV timecode accepts frame numbers up to 29 for PAL (`dv_subcode.c`); REC START only, a
  frozen timecode and small forward jumps (`test_pin_scene.c`); the deck's timecode parse
  edge cases.
- **Mixed formats end to end.** The session's HDV format cut on `ep88-hdv-pid.bin` (only
  `test_reassembler_mixed` reads it); a 4:3 / 16:9 DV change (ffmpeg makes 16:9 DV);
  VAUX DISP code 7; `--split` together with a format change (the detector restarts at
  each format cut, `format_cut_begin()`); DV MOV, H.264 and HEVC, and a pipe, across a
  change; the CLI's exit code 4 when a pipe meets a DV <-> HDV change.
- **Gaps**: gaps and mid-picture cuts in a replay (`make_ep88`).
- **Drives**: HDV onto a full or failing drive; DV AVI past 1 GiB; the FFV1 stall
  watchdog (10 s) has no test.
- **Analog**: no replay and no fixture (8); the SAA7113 register table per standard; the
  counter wrap (needs a fake USB layer or a recorded dump).
- **USB**: no seam for libusb (the FX2 download of 10, unplug at each phase, an injected
  sleep error); no stress test of the transfer queue and ring, nothing under TSan.
- **Encoders**: a stuck encoder, for the 10 s watchdog in `sink_hwenc.c` (a failing one is
  covered: `PIN_HWENC_FAIL_AFTER`).
- **Replay**: a raw EP 0x88 dump is paced at the DV rate for every unit, HDV pictures
  included (`replay_run()`). Replay timing only.
- **Modules without tests**: `pin_hdv_audio.c`, `pin_update_net.c`; the CLI's exit codes
  and status output beyond its pipe paths (`test_cli_pipe`); `pin_preview.c` on HDV and
  analog (tested on DV only).
- **CI** runs only on manual dispatch, and the ctest suite only when "Run the ctest suite"
  is ticked ([ci.md](ci.md)). Android never runs tests, and there are no sanitizer builds.
  Wanted: a Linux job on push that runs ctest, plus ASan / UBSan and TSan builds (the core
  runs USB, writer, closer and preview threads).

## 14. Open decisions

- **NOT_IMPLEMENTED** from a deck: busy, or not supported (5)?
- **A zero GOP timecode**: whether to use the aux stream's recording date for the split
  (after 1.1).
- **24p and pulldown** in HDV and DV: detect and show it, time HDV MOV / MKV by it, offer
  removing the pulldown (1.4, 2.4)?
- **A bit-depth-only DV audio change** (12-bit <-> 16-bit at the same rate and channels)
  starts a new DV AVI / MOV / H.264 / HEVC file, though those files hold 16-bit PCM either
  way.
- **The scene number counts format cuts**: without `--split` the status reads scene 2
  after a format change.
- **An analog standard change mid-capture**: new file, stop, or carry on (8)?
- **Sleep and resume**: stop with "device lost"?

## 15. Setting up a sender

### A FireWire PC

A computer with a FireWire port, connected to the MovieBox's 1394 port, stands in for a
camera.

**What it has to do, from our side:**

- **Stream only.** If no node answers the oMPR read, we listen on broadcast channel 63
  anyway (`pinnacle_stream_start()`). A sender that transmits DV (IEC 61883-2) or MPEG-TS
  (IEC 61883-4) on channel 63 should be captured with no plug emulation.
- **With plug emulation.** If the sender exposes oMPR / oPCR registers, we connect to it
  like a camera, which also tests `p1394_connect()` and the channel taken from the plug.
- **Deck control.** Software on the PC that answers AV/C FCP commands like a tape
  subunit, ideally scriptable.

**Candidate software. None of it has been tried.**

- **Linux on the PC or an old Mac** (most flexible; a live USB is enough): `libiec61883`
  (with `plugctl` / `plugreport`) transmits DV and MPEG-2 TS and can emulate local plug
  registers; its examples `test-dv` and `test-mpeg2` have a transmit mode. The kernel's
  `firewire-core` character device can take FCP requests, so a small scripted AV/C
  responder (a fake deck) is feasible.
- **An old Mac (OS X)**: Apple's FireWire SDK had AVCVideoServices, with DV and MPEG-2
  transmitter samples and virtual tape-device samples that answer AV/C. Older editors'
  "print to video" also sends DV.
- **Windows**: the inbox DV driver (MSDV) sends DV through DirectShow (editors' "export to
  tape", WinDV-style tools). HDV out and AV/C target emulation are much harder; use it for
  DV only.

**The content:** ffmpeg makes all of it, and `tests/make_fixtures.cmake` has the commands
for the pieces the tests use. DV: PAL and NTSC, 4:3 and 16:9, 32 and 48 kHz, 24p
telecined to 29.97. HDV-like TS: 1440x1080 or 1280x720, chosen PIDs, `frame_rate_code`
and progressive flags, MP2 audio. Mixed tapes are files joined at the frame or TS packet,
in the order `make_ep88` takes them.

**Limits:** no real tape errors, dropouts or camera quirks, and a PC's timing, not a
camera's. Use it for formats, switches and protocol logic; real cameras and tapes for
compatibility and error behaviour.

### A second Marvin

Two MovieBoxes joined by a FireWire cable, one sending. None of this has been tried.

**Why it should be possible:**

- The FPGA design is a standard OHCI-1394 controller ([hardware.md](hardware.md)), and
  OHCI has isochronous transmit (IT) contexts as well as receive contexts.
- The USB interface has an OUT endpoint for it: EP 0x06 OUT in alt 1, the DV/HDV setting;
  alt 2 is "output to the camera (not used)".
- The vendor driver exposes the unit to Windows as an ordinary 1394 bus
  (`MarvinBus64.sys`, [windows-driver.md](windows-driver.md)), so the Windows DV stack can
  send through it. The analog "Render" bitstream goes to a TV, not 1394.

**Why it would beat a FireWire PC:**

- Two USB devices on one host: it could run unattended on the Linux host, from a script,
  as a hardware regression test.
- The sender would be our own C code, sending exactly what the tests need.
  `tests/make_ep88.c` already builds the IEC 61883 packets a camera puts on the bus (DV in
  packets of 6 DIF blocks with an empty packet after every 15, TS as 192-byte source
  packets, a continuous DBC across inputs). Missing: pacing to the 8 kHz cycle and real
  SYT / SPH timestamps (`make_ep88` writes SYT `0xFFFF` and synthetic SPH, which our
  receiver ignores but a camera may not).
- The receiving unit's output should equal the input files byte for byte, so the
  reassembler and mixed-format tests would run on hardware.
- Our link layer already answers incoming async requests (the AR request context,
  `p1394_answer_owed`), so the sending unit could answer oMPR / oPCR reads and AV/C
  commands from a script: the fake deck of section 5, with no other software.
- Transmit is also what a future "write back to tape" feature needs.

**What is unknown, cheapest first:**

1. **Does the bitstream implement IT contexts?** Write all ones to `IsoXmitIntMaskSet` and
   read the mask back: one register poke on the Linux host.
2. **How do data and descriptors reach the device?** On receive, descriptors in device RAM
   point at a buffer, and address bit 23 routes it to EP 0x88 ([startup.md](startup.md)).
   Transmit presumably mirrors that, with packet data written on EP 0x06 and
   OUTPUT_MORE / OUTPUT_LAST descriptors reading it (a guess). Read it in the Ghidra
   output of `MarvinBus64.sys` (IT register writes around OHCI offset 0x200, the EP 0x06
   pipe), or in a USB trace of the vendor driver sending.
3. **A trace of the vendor driver sending.** On Windows with the Pinnacle driver, any
   DV-out program sends through the Marvin; record it with USBPcap. Windows only offers
   output to a DV device it has found on the bus, so the trace needs a real camera on that
   unit, once.
4. **The sender itself**: a transmit loop in `src/core` (with `make_ep88`'s packet
   builders moved there) and a `--transmit file.dv|file.ts` (or test-only) mode, with the
   receiving unit chosen by `--device`.

With a camera attached, a Windows PC with the Pinnacle driver is itself a "FireWire PC",
but it sends DV only to a camera it has found, not to another Marvin.

**Bus and topology:**

- Each 500-USB wires only PHY port 0, so two units take the one cable between them. A
  camera on the same bus as well needs a FireWire hub or repeater (a 700-USB may have more
  ports; untested).
- Both units make themselves cycle master in link init (`p1394_link_init()`). That takes
  effect only on the root node, so one of the two will be; check that this is stable.
- Each unit sees a second node with no camera behind it (two nodes, 6).
- Two DV streams use about 7 MB/s of USB, well within USB 2.0; separate host controllers
  are still safer.

**Limits:** both ends are the same FPGA and PHY, so a fault common to both could cancel
out, and nothing about other makers' 1394 chips is learned. S100 only. Real cameras and
tapes stay necessary for compatibility and tape errors.

## 16. Suggested order

1. **The Canon HDV camcorder** (1.1): HDV MOV / MKV from a clean and a worn tape, in
   Resolve; scene split on a tape with several recordings (does the camera clock jump? do
   recordings start on a closed GOP?); the aux stream and real EP 0x88 dumps as fixtures;
   if it records DV, a DV/HDV tape (3.1).
2. **Code**: the fixes in 17 (first R15); the fake AV/C link and the deck items; the end-to-end gaps in 13; a CI job on
   push with sanitizer builds.
3. **Decide** the points in 14.
4. **A sender** (first check whether a second Marvin can transmit, step 1 of 15):
   stream-only on channel 63; the mixed streams `make_ep88` models, through the real path,
   byte for byte; 720p, 1080i/29.97, 24p, other PIDs, multi-packet PSI and gaps; a fake
   deck.
5. **Other cameras and tapes**: the camera matrix (5), at least one Sony and one
   Panasonic or JVC; a JVC 720p source (1.3); a 60 Hz HDV camera (1.2); 16:9, LP, 12-bit
   and 4-channel DV (keep short recordings as fixtures); a PAL/NTSC tape (3.2); 24p tapes
   (1.4, 2.4).
6. **Drives, hosts and units**: a FAT32 stick, a full and a slow drive, an 8-hour run per
   OS; unplug, bus reset and sleep; Android on a phone; the untested Pinnacle units.

## 17. Code review: data loss and wrong counts

Found by reading the code (November 2026), MarvinCapture only (core, GUIs, CLI, scripts;
not MarvinPlayout). Nothing here has been seen on hardware yet. Items marked
"reproduced" were run in a scratch harness that feeds synthetic data to the static
functions. The others come from reading the code alone. Each item is marked:

- **Fix required**: the code does the wrong thing for that input. Needs a fix and a test
  in `tests/engine`.
- **Testing required**: whether it goes wrong depends on what real equipment, an OS or a
  drive does.

Severity is high, medium or low, judged by how much data can go missing and how quietly.

### 17.1 Frames lost without being counted

- **R3. `find_header()` accepts false DIF headers.** Medium. Fix required (reproduced).
  - `is_dif_header()` matches `1F ?7 00` at any byte offset and trusts the 4-bit dseq. A rescan happens only after a loss.
  - A false match with dseq 13 builds a 14-sequence frame, which the session then drops as damaged.
  - A false dseq 0 emits an extra bogus frame that is counted as normal.
  - Fix: validate a candidate (the next block is a `3F` header, the next header sits 12000 bytes on, the offset is a multiple of 80 from the last good header, dseq is below 10 or 12).
- **R4. Resync takes 12000 bytes blindly.** Low-medium. Fix required (reproduced).
  - After a resync, `dif_feed()` takes a full sequence without looking for a header inside it.
  - One lost 480-byte packet in sequence S therefore corrupts S (shifted data) and zero-pads S+1.
- **R11. Analog audio lead-in loss.** Low. Testing required.
  - `sink_lazy.c` `lazy_write_audio()` drops audio until the first video unit has opened the file.
  - If the audio packet for frame F arrives before video frame F, the file starts with the audio of F+1: about 40 ms missing and a constant lag. Measure on the device.

### 17.2 Files not finalised, or errors not reported

- **R15. A writer start failure goes unnoticed.** Medium. Fix required.
  - `open_sink_for_scene()` does not check `pin_writer_start_ex()`. It returns NULL when the 64 / 256 / 512 MiB ring cannot be allocated (32-bit host, low-memory Android, Windows commit limit) or the thread cannot start.
  - With a NULL writer, `writer_commit()` and the analog callbacks skip every push without a counter, and `capture_guard()` returns early. The capture "runs" and ends as "No video received".
  - Fix: fail the open with `PIN_ERR_NOMEM`, or retry with a smaller ring.
- **R18. An AVI is only valid after a clean close.** Low-medium. Testing required.
  - `avi_writer.c` writes the RIFF sizes, the avih / strh / dmlh totals, `indx` and `idx1` only in `close_segment()` and `avi_close()`.
  - After ENOSPC, a pulled drive, a kill or a power loss, the file has RIFF size 0, zero frame counts and no index. ffmpeg can scan it; many players and editors cannot.
  - Multi-hour analog AVIs are exposed for their whole length. A periodic patch of the sizes and totals would limit the damage.
  - Section 9's pulled-stick test has been done for DV and pipes only; do it for analog AVI.
- **R20. Damaged units counted as written; empty files left behind.** Low. Fix required.
  - `sink_raw.c` (a DV unit that is not 10 or 12 sequences) and `sink_nut.c` (a short video frame) drop the unit and return `PIN_OK`. `sink_lazy.c` counts every OK as written.
  - Effects: `units_written`, FILE_OPENED and `stop_no_video` are wrong for a file with no video, and `lazy_close()` never removes such an empty file.
  - `units_damaged` is read nowhere in the engine.
  - In NUT, a skipped video frame does not advance `vpts`, so audio drifts against video.
- **R21. Android: finalising judged from a stale status.** Low. Fix required.
  - `CaptureService.closeSession()` takes `wasCapturing` from the last tick's snapshot, up to 1 s old. If a capture started within that time, the service leaves the foreground and releases the wake lock while `Core.close` is still finalising.
  - `onDestroy()` shuts the core thread down without waiting for it.
  - `goForeground()` swallows a `startForeground` failure (Android 12+ background-start limits), so the OS can kill a background capture with nothing shown to the user (11).

### 17.4 Wrong numbers on screen

- **R24. The misnamed counters `ts_errors` and `frames_damaged`.** Low. Fix required.
  - `ts_errors` is documented as HDV continuity errors. It counts adaptation-field `discontinuity_indicator` flags (`hdv_ts_discontinuity_count()`).
  - `frames_damaged` is documented as zero-padded DV sequences, but it counts only units that cannot be parsed.
- **R25. `hdv_error_analyze()` counts a flagged discontinuity as an error.** Low. Fix required.
  - `hdv_error.c` counts the CC error before it checks `discontinuity_indicator`; the branch for that check is empty.
  - The reassembler honours the flag, so the two disagree on a legitimate CC jump. Does the Canon set it at recording boundaries (1.1)?
- **R26. Free-space estimate.** Low-medium. Fix required.
  - The core memsets the status, so `est_seconds_left` is 0 when there is no estimate. `pin_api.h` says "0 if no hint" and `NativeStructs.cs` says "< 0 = unknown".
  - `pin_format_storage_free()` and `pin_format_storage_detail()` test `>= 0`, so they show "0 min left". `test_pin_ui_api.c` sets -1 by hand and misses it.
  - When `fat32_and_free()` fails (folder not yet created, network volume gone), free space is 0. `disk_low` is then set, and the GUIs show a false "Low disk space" warning and a yellow bar.
- **R27. Total and clip dropped counts disagree.** Low, cosmetic. Fix required.
  - The total adds `write_dropped`, but `clip_frames_dropped` does not. `write_dropped` is reset once per capture, not per file.
  - `write_dropped` counts audio blocks too, so "Dropped" mixes frames and audio blocks.
  - In practice only a burst before a DISK_SLOW end.
- **R28. Byte totals are wrong at file cuts and for encoder sinks.** Low. Fix required.
  - `hand_over_file()` adds the old file to `bytes_closed`, but `s->bytes_written` is refreshed only in the next `dv_tick()`. For up to 50 ms the total counts the previous file twice.
  - `close_job()` reads the bytes before `sink->close`. Only hdvmux corrects them through `final_status`. For ffv1 and hwenc, the queue, the FLAC ring, the encoder lookahead and the trailer are missing, and a short DV H.264 clip can report 0 bytes.
- **R30. The event queue drops the oldest events.** Low. Fix required.
  - `pin_session.c` pushes PIN_EVT_DECK on every transport-state poll answer (1-2 Hz), not only on a change.
  - The queue holds 256 events (`PIN_EVQ_CAP`) and drops the oldest of any kind. A consumer that does not poll for about 4 minutes therefore loses FILE_OPENED, FILE_CLOSED and CAPTURE_ENDED.
  - All three GUIs keep polling while minimised or in the background, so this needs a stalled consumer.
  - Fix: push on change only, or keep slots for important events.
- **R31. Thread status read without a lock.** Low. Testing required.
  - `hand_over_file()`, `file_bytes_due()`, `capture_guard()` and `pin_session_get_status()` read `sink->get_status` while the writer thread updates it. Values can be stale or torn; no crash path was found.
  - Relevant to the TSan item in 13.

### 17.5 Stop conditions and scene cuts

- **R33. REC START held over several frames pushes the scene cut late.** Low. Testing required.
  - In `pin_scene.c` `classify()`, every flagged frame restarts the debounce. The cut lands on the last flagged frame, and the first frames of the new scene go into the old file (2).
- **R34. "Total time" is per pass.** Low, docs.
  - The time limit applies per pass. [README.md](../README.md) says "after total time captured", and `NativeStructs.cs` calls it the total capture time limit.
  - With N passes, each pass re-captures the first minutes of the tape. This is by design, but the wording invites mistakes.

### 17.6 Smaller items

- **`dv_audio_extract()`, 12-bit audio not in 4-channel mode** (`dv_audio.c`). Low. Fix required.
  - The code reads `shuffle[i + half_ch]` past the end of the table for `i >= half_ch`, and the result can overwrite pair 1.
  - This affects the meter and the monitor only (2.3).
- **Thread priority inheritance** (Linux). Medium. Testing required.
  - `pin_thread_boost()` sets SCHED_FIFO 10 or nice -10 on the USB thread. The writer and encoder threads are created from that thread with default attributes, so they inherit the boost.
  - CPU-heavy encoders could then delay the USB reaper or starve the GUI.
- **OHCI `xferStatus` is not checked** (`iso_header_ok()`). Low. Testing required.
  - Packets that completed with an error event are accepted as data. Count them on real dumps (normal is `0x8411`).
- **EP 0x84 resubmit loop** (`ep84_cb`). Low.
  - A stalled endpoint spins the loop, because every status but CANCELLED and NO_DEVICE resubmits. A record longer than 512 bytes is lost.
  - At stop, transfers that completed but were not yet delivered (up to about 290 ms) are discarded.
- **The last pending DV frame at a user stop** (`handle_inline_commands()`) is not
  written, and the counters keep running after CAPTURE_ENDED. At most one frame. Low.
- **The stdout pipe treats EAGAIN as fatal** (`pin_stdout_write()`). Low.
  - A non-blocking stdout inherited from the parent ends the capture with WRITE_ERROR.
- **Device locks.** Low. Testing required.
  - Windows: a thread-owned mutex lets the same thread open a device twice. If the opening thread exits, the abandoned mutex is accepted by the next process.
  - POSIX: unlink-then-unlock leaves a race in which two processes can each lock a different inode.
- **Settings save** (`pin_settings_save()`). Low.
  - There is no fsync before the rename, so a power loss can leave a zero-length file.
  - The fixed `.tmp` name lets the CLI and a GUI saving at the same time interleave.
  - After 256 sections, sections are written twice.
- **Unchecked `realloc`** in `avi_writer.c` (`ix_add()`, the idx1 list). Negligible.

### 17.7 Checked, nothing found

- Writer ring wrap and lengths.
- Closer order and drain, and FILE_CLOSED arriving before CAPTURE_ENDED.
- Hold-back and split lookahead: no loss or duplicates.
- AVI and OpenDML layout, segment limits, and the FAT32 margin.
- Analog sequence wrap and rings.
- hdv_demux 33-bit PTS unwrap, and CC / TEI / duplicate handling.
- hdvmux PTS / DTS and the "behind" limits.
- hwenc error propagation and watchdogs.
- Preview: it uses its own copies and never touches writer data.
- JNI indexes and the Windows struct mirror.
- The overwrite prompts in all three GUIs.
- The `rm -rf` targets in `scripts/`.
