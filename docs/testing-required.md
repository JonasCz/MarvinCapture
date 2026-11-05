# What still needs testing (and probably fixing)

Status as of **2026-11-05**. A survey of the docs and code for what has never been
tested, what is only assumed, and where the code is likely wrong. Nothing here has
been run; it is a work list.

## How to read this

Every item is tagged with what it takes to test it:

| Tag | Meaning |
|---|---|
| **HW** | Needs a real camera, deck, tape or Pinnacle unit. |
| **FW-PC** | Can be done with a computer that has its own FireWire port, or possibly a second Marvin, acting as the "camera": it transmits a synthetic DV/HDV stream (or answers AV/C commands) into the MovieBox. See [FireWire PC as a test source](#a-firewire-pc-as-a-test-source). |
| **CODE** | Can be tested in code only: unit tests in `tests/engine`, synthetic DV frames / TS packets, or the replay device. No hardware. |

Many items carry two tags: a synthetic test gives the logic, the real device
confirms what cameras actually send.

Findings are marked **Bug (confirmed in code)** when the code was read and the
fault is plain, **Likely bug** when it follows from reading but was not traced
end to end, and **Untested** when the code may be fine but nothing has exercised it.

**What has been verified on hardware so far** (for reference):

- HDV: one Canon HDV camcorder, 1080i/25 (1440x1080, 25 fps, PIDs video `0x810`,
  audio `0x814`, PMT `0x81`), on a 500-USB and a 510-USB ([hdv.md](hdv.md),
  [deck-control.md](deck-control.md)).
- DV: one DV camcorder (make and model not recorded) with a short tape, on a 510-USB
  under Windows; one camcorder in "AV to DV" passthrough for 60 min.
- Analog: PAL composite, PAL S-video, NTSC composite ([analog.md](analog.md)).
- Devices: 500-USB (`0213`) and 510-USB (`0223`). Hosts: Windows x64, Apple Silicon
  macOS, Linux x86_64.

---

## 1. HDV 720p and other HDV variants

Only 1440x1080 at 25 fps has been seen ([hdv.md](hdv.md) "Not tested: 720p HDV, S400
cameras, PAL/NTSC variants"). Raw `.ts`/`.m2t` is the only HDV output enabled; MOV/MKV
for HDV is disabled as untested (`pin_session.c:1021`), so the sink findings below
only hurt once that is switched on, or once another HDV sink exists.

**1.1 HDV output parameters are fixed at 1440x1080, top field first, 25 fps.**
Bug (confirmed in code). `open_sink_for_scene` (`pin_session.c:1231-1238`) always
hands sinks 1440x1080 interlaced TFF. The fps comes from `is_pal`, and for HDV
`detected_std` is set to `PIN_STD_AUTO` (`pin_session.c:1616`), so `is_pal` is always
true: 1080i60 and 720p60 are described as 25 fps. The real geometry and frame-rate code
(`hdv_seq_w/h`, `hdv_fps_num/den`) are already parsed but not used here. Harmless for
raw TS; wrong for any sink that uses the params. **CODE**: synthetic 720p59.94 and
1080i29.97 TS through the replay device, assert the params a test sink receives.

**1.2 720p gets the wrong pixel aspect ratio.** Bug (confirmed in code).
`sink_sar.c:66-80` computes the HDV SAR with a hard-coded height of 1080. For 1280x720
16:9 that gives 3:2; it must be 1:1. The MPEG-2 `aspect_ratio_information` is not used,
and DAR is hard-coded 16:9 (`pin_session.c:1615`). **CODE**: unit test of
`pin_sink_sar_for(PIN_KIND_HDV, ..., 1280)`; remux a synthetic 720p TS and read back the SAR.

**1.3 The remux marks all HDV video top-field-first interlaced.** Bug (confirmed in code).
`sink_rewrap.c:610` sets `AV_FIELD_TT` unconditionally. Wrong for 720p (progressive) and
for progressive-in-interlaced material (Canon 24F/25F/30F, Sony 24p). **CODE**: remux an
ffmpeg-made progressive 720p `mpeg2video` TS and assert the field order.

**1.4 "Progressive" is decided by height alone, and pulldown is ignored.** Likely bug.
`pin_vidfmt_hdv_interlaced()` is `height != 720` (`pin_vidfmt.c`), so 1080p shows as
1080i. `hdv_error.c` parses the sequence header only, not the sequence extension
(`progressive_sequence`, `frame_rate_extension`) or the picture coding extension
(`repeat_first_field`, `progressive_frame`, `top_field_first`). So:
- 24p carried with 2:3 pulldown flags (Sony, Canon 24F) reports 29.97.
- JVC 720p24/720p30 are, as far as I know, carried in a 60p/50p stream with repeat
  flags (unverified); a rate taken from `frame_rate_code` alone would be double the
  real rate.
- `captured_seconds()` and the time-left estimate use `hdv_fps`
  (`pin_session.c:~1717`) and would be off.

**CODE** (hand-built sequence/picture extensions with RFF and progressive flags),
**FW-PC** (transmit an ffmpeg-made 24p/pulldown TS), **HW** (what JVC, Sony and Canon
really write).

**1.5 PAT/PMT parsing is single-packet and single-shot.** Likely bug.
- `hdv_aux.c:50-118` parses only the PUSI packet. A PSI section split over two TS
  packets is cut short, and `pmt_found` is still set (line 116), so the PMT is never
  re-read.
- Only video `stream_type 0x02` and audio `0x03/0x04` are recognised.
- A PMT version change mid-stream is not followed.
- The reassembler caches PAT/PMT for the file start on the same one-packet assumption
  (`dv_reassembler.c:~380-411`).

The PIDs are not hard-coded, but 0x810/0x814 is the only pair ever seen. This is the
main risk for JVC 720p. **CODE** (TS with other PIDs, e.g. 0x100/0x1000, a PMT split
across packets, a version bump), **FW-PC**, **HW** (a JVC ProHD/HDV camera or deck).

**1.6 The start gate needs the sequence header in the first packet of the video PES.**
Untested. `dv_reassembler.c:~434` looks for `00 00 01 B3` only in the PES-start packet.
A long PES header, or user data placed before the sequence header, would push it into
the second packet; the gate would never open and nothing would be written, with no
message. **CODE**, **FW-PC**.

**1.7 One picture per PES is assumed.** Untested. A unit ends at each new video PES
start (`dv_reassembler.c:~455`). `hdv_error.c` records only the first picture type in a
unit, and may count a false `pes_errors` when a unit has a second PES start. Fine for
the Canon; unknown for others. **CODE**.

**1.8 The CIP check accepts only FMT 0x20, DBS 6, payload a multiple of 192.** Untested.
FN/SPH/QPC are not checked (`dv_reassembler.c:247-252`). A camera with another
source-packet layout is dropped as noise with no diagnostic. S200/S400 senders have
never been seen. **HW**, **FW-PC** (a sender at S400, if the software allows it).

**1.9 HDV audio.** Untested. Only MPEG-1 Layer II is recognised. The monitor path
(`pin_hdv_audio.c`) handles 44.1/32 kHz MP2 and mono (duplicated), but `pin_hdv_audio`
has no test of its own. More than two channels are ignored. The file keeps the TS audio
untouched, so this affects monitoring only. **CODE** with `hdv.ts` and ffmpeg-made MP2
at other rates.

**1.10 HDV scene split and replay pacing assume 29.97 fps.** Bug (confirmed in code).
- The scene detector is configured with `pin_scene_config_defaults(&cfg, 29.97)`
  (`pin_session.c:1625`).
- Replay of a `.ts` paces at 1001/30000 per picture.
- Replay recognises only `.ts`, not `.m2t`.

**CODE**.

**1.11 `hdv_aux_scan_datetime` reads the drop-frame flag from bit 7.** The DV parser
uses bit 6 for the same pack (`dv_subcode.c:154`). One of them is wrong, very likely
this one: in the DV/AAUX timecode pack bit 7 is the colour-frame flag. Only
`test_hdv_aux.c` calls this function today, so the impact is nil until it is used.
**CODE**.

## 2. 24p and other frame-rate modes

**2.1 DV 24p (Panasonic DVX100 24p/24pA, Canon 24F in DV) is neither detected nor
documented.** Untested. Such tapes are 29.97 DV with 2:3 or 2:3:3:2 (advanced)
pulldown. The app treats them as 29.97i BFF (`pin_session.c:1223-1226,1473`), and no
VAUX bits beyond the aspect code are parsed. Capturing them as plain 29.97 loses
nothing and is probably the right default. But:
- nothing in [usage.md](usage.md) says so;
- nobody has checked that such tapes capture cleanly;
- removing the pulldown is not offered.

**HW** (a DVX100 24p/24pA tape, a Canon 24F tape), **FW-PC** (ffmpeg can make 24p
material with telecine to 29.97 DV), **CODE** (if 24pA detection or reverse telecine
is ever wanted, the cadence detection is pure logic).

**2.2 HDV 24p/25p/30p and 720p50/60.** See 1.1, 1.3, 1.4 and 1.10. **CODE**,
**FW-PC**, **HW**.

**2.3 GOP timecode at 50/60 fps and drop frame.** Untested. The timecode label is
built from the GOP picture count (`pin_session.c:~1594`), and drop frame is shown as
`;` whatever the rate. At 720p50/60 the frame field runs to 49/59. The deck-timecode
fallback (`hdv_tc_live`) has been seen on one Canon only. **CODE**, **HW**.

**2.4 DV timecode accepts frame numbers up to 29 for PAL.** Untested
(`dv_subcode.c`). Out-of-range values are not flagged. **CODE**.

## 3. Tapes with a mix of standards and formats

Nothing tests a mid-stream format change at any layer. All fixtures are
single-format.

**3.1 DV then HDV (or HDV then DV) on one tape: the capture stalls.**
Bug (confirmed in code).
- The reassembler locks the format on the first data packet
  (`dv_reassembler.c:255`, `if (locked != DV_FORMAT_UNKNOWN && f != locked) return 0;`)
  and never unlocks it while the session runs.
- After a switch, every packet is rejected and counted as a resync; no frames come
  out.
- The capture ends with "no signal" only if the no-signal timeout is on. Otherwise
  it sits in "capturing" with no message naming the cause.

Unknown: whether real cameras reset the 1394 bus when they switch format. A bus reset
gives `PIN_STOP_CAMERA_LOST`, which is itself untested (5.6). Steps:
- **CODE**: feed DV CIP packets then HDV ones through `dv_reassembler`.
- **FW-PC**: transmit DV, then HDV, on the same channel.
- **HW**: a tape recorded in both modes, in an HDV camera or deck.

**3.2 If the stream kind does change within a session, the open file is wrong.**
Likely bug.
- `dv_on_unit` flips `stream_kind` (`pin_session.c:~1464`), but the sink was opened
  for the old kind and no new file is started.
- DV frames would go into a raw-TS sink, or the reverse.
- `sink_raw` checks DV lengths only when it was opened for DV (`sink_raw.c:80`).

Needs a decided policy first (new file, or stop with a message), then a **CODE** test.

**3.3 PAL then NTSC mid-tape (DV) corrupts every later frame.** Bug (confirmed in code).
- `handle_sequence` raises `system_seq_count` to 12 the first time it sees DIF
  sequence 10 or 11, and never lowers it (`dv_reassembler.c:122-125`).
- After a switch to NTSC, each 10-sequence frame is padded with two empty sequences
  and emitted as a 144,000-byte "PAL" frame.
- `dv_error` counts every one as damaged, and the audio is unshuffled with the PAL
  table.
- NTSC then PAL works (the count goes up).

**CODE**: concatenate the NTSC and PAL fixtures through `dv_reassembler`, assert frame
lengths and `sequences_dropped`. **FW-PC**, **HW**: a PAL/NTSC mixed tape (rare, but it
is what a deck with a multi-standard tape, or a re-used tape, produces).

**3.4 Sinks freeze geometry, fps, aspect and audio format at open.** Likely bug.
- `open_sink_for_scene` builds the params from the first frame
  (`pin_session.c:1208-1242`), and nothing reopens on a change.
- `sink_rewrap` writes NTSC frames into a PAL-declared file, with audio timing from
  the open-time fps.
- `sink_hwenc` drops other-standard frames and counts them as damaged (`:593`,
  `:1283`).
- `sink_raw` writes mixed-length frames.

Decide a policy: a new file on a standard, aspect or audio change. Scene split (`--split`)
already makes new files, so this mostly matters without it. **CODE**.

**3.5 4:3 / 16:9 change mid-capture.** Likely bug. Aspect is taken at file open
(`pin_session.c:1204`). Without `--split`, the rest of the file keeps the first value;
with it, the next file picks it up up to ~1 s late. 16:9 detection uses VAUX DISP codes
2 and 7 only; the test covers only `0x02` and a missing pack (`test_dv_subcode.c`).
**CODE**, **FW-PC**, **HW** (a tape with both).

**3.6 DV audio mode change (32 kHz 12-bit to 48 kHz 16-bit, or 2 to 4 channels).**
Likely bug. `sink_rewrap.c:~262-300` fixes the audio track's rate and channel count
from the first audio frame. Later frames at another rate are fitted as if they were the
old rate, and a second stereo pair that appears later is dropped with a one-time
warning. Preview resamples per frame and is probably fine (maybe a click).
- Typical source: an LP recording or a 12-bit section mid-tape.
- **CODE**: concatenate `dv-ntsc-32k.dv` and `dv-ntsc.dv`, check the A/V length.
- **FW-PC**, **HW**.

**3.7 HDV 1080i to 720p, or a PID change, mid-stream.** Likely bug, speculative on what
cameras do.
- The reassembler pins the video PID at gate open. If the PID changes, pictures are
  never closed and `ts_pic_buf` grows without bound.
- Width and height updates are silent: no `PIN_EVT_INPUT_FORMAT`
  (`pin_session.c:1608-1616`).

**CODE** (splice a TS with another video PID into `hdv.ts`), **FW-PC**, **HW**.

**3.8 HDV scene split cannot work as designed.** Likely bug.
- The FIFO holds about `debounce_window+1` (~6) units. In HDV a unit is one picture,
  but a timecode only arrives once per GOP (12-15 pictures), so the cut is confirmed
  after several GOPs.
- By then the cut picture has left the FIFO, and the new file starts at an arbitrary
  picture, seconds late. That picture has no sequence header, PAT or PMT, so the file
  may not decode from its start.
- The 1 s minimum segment uses 0.5 s per HDV unit (`pin_session.c:~842`).
- `test_replay_hdv.c` only asserts "no spurious split".

**CODE**: a synthetic TS with a GOP timecode jump; assert the second file starts on a
sequence header with PAT/PMT.

**3.9 HDV scene split never fires on cameras that write a zero GOP timecode.**
Bug (confirmed in code). The Canon always writes `00:00:00:00`. The live-timecode path
guards on that (`hdv_gop_time_nonzero`, `pin_session.c:1589`), but the scene record sets
`tc_valid` whenever a GOP header is found (`pin_session.c:1634`), so every picture looks
continuous. HDV recording date/time from the aux stream is not used for splitting. Fix:
apply the same zero guard, and consider the aux-PID recording date. **CODE**; **HW** to
see what other cameras write.

**3.10 Scene detector fps is set once per pass.** Untested (`pin_session.c:1524`). After
a PAL/NTSC change the first comparisons use the wrong nominal rate. A drop-frame to
non-drop-frame flip is measured with the old flag. Probably harmless, because such a
change also breaks timecode. **CODE**.

**3.11 Gaps and blank sections.** Untested.
- DV: a partial frame at a gap is emitted zero-padded (counted damaged) when data
  resumes.
- HDV: a partial picture is glued to whatever arrives after the gap.
- The gate is re-armed per pass, not per gap.
- After a gap, `hdv_err_reset` only resets error tracking (`pin_session.c:~1599`).

Documented hardware behaviour ([deck-control.md](deck-control.md)):
- A capture that starts on blank tape never gets a first frame and stays "capturing".
- The end of tape is seen only through the no-signal timeout.

**CODE** (insert gaps and mid-picture cuts in a replay), **FW-PC** (pause the sender),
**HW** (a tape with unrecorded gaps).

**3.12 Timecode edge cases not in `test_pin_scene.c`.** Untested.
- REC START only, with no timecode and no date: probably never splits.
- A frozen timecode at pause or still.
- Small forward jumps.

What cameras do around REC START and still frames is unknown. **CODE**, **HW**.

**3.13 LP recordings.** Untested. Nothing in the code or docs mentions LP. LP means
32 kHz 12-bit audio (see 3.6) and generally more dropouts. **HW**.

**3.14 Analog: the standard changes during a capture.** Untested. The restart only
happens in READY (`analog_tick`, `pin_session.c:~2519`); geometry and audio packet size
are fixed at start. With `--std auto` the status shows the new standard while the file
keeps the old geometry. [analog.md](analog.md) also lists "what happens at the moment the
signal drops or returns mid-capture" as open. **HW** (a VCR tape with a PAL/NTSC change).

## 4. Cameras and decks from other manufacturers

Tested: one Canon HDV camcorder and one unnamed DV camcorder. Never tested:
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
  (`pin_deck.c:219-226`).
- The "busy" reading comes from one Canon.

On another camera (for example FF or PLAY in camera mode), the app waits ~3.5 s and
then believes the command was sent. A rewind-first capture would wait for a rewind that
never started, and a STOP would be reported as done. **CODE** (with the fake link), then
**HW** / **FW-PC** to see what real decks answer.

**4.2 Transport-state mapping is narrow.** Untested (`pin_deck.c:41-60`).
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
reject or do not implement transport commands; that feeds 4.1. **HW**.

**4.5 Fixed timeouts, no resend.** Untested. Sync commands wait 3 s (10 s after INTERIM);
async commands fail after 3 s with no resend, so a lost STOP stays lost. Responses are
matched by subunit and opcode only, not ctype, so a late reply to an earlier command
could be taken for the reply to a newer one with the same opcode (speculative). A
deck-state query that keeps failing is ignored silently: there is no "deck not
responding" status (`pin_session.c:~2150-2170`). **CODE**, **HW** (slow decks: DVCAM,
Digital8).

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
  (`pinnacle_stream.c:244-249`).

A camera that wants a properly allocated point-to-point connection, has several output
plugs, or moves channel after connecting, has not been seen. **CODE** (`pcr_adjust`
against a fake register pair: concurrent change, count 0/63, failed lock), **FW-PC**
(a sender with plug emulation, see below), **HW**.

**4.8 Camera discovery takes the first node that answers oMPR**
(`pinnacle_1394.c:~1095-1109`). Two devices on the bus (a camera and a deck, or a PC) are
not handled deliberately. **FW-PC** (PC plus camera on one bus), **HW**.

**4.9 Bus reset during a capture** ends with `PIN_STOP_CAMERA_LOST`; documented as
untested. A capture still waiting for its first frame keeps waiting. Some cameras may
reset the bus on a mode switch, tape load or power save. **CODE** (fake link injecting
a SelfIDCount change), **FW-PC** (unplug the PC's cable or restart its sender), **HW**.

**4.10 Collect a camera matrix.** For each new camera or deck, record in
[deck-control.md](deck-control.md):
- the UNIT INFO and SUBUNIT INFO answers;
- the transport-state bytes in every mode;
- the TIME CODE answers in camera and VCR mode;
- the oMPR/oPCR values;
- a `--debug` AV/C log;
- a short capture of each format the camera records.

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

**5.3 Disk full, slow disk, write error.** Only the message texts are tested
(`test_pin_stop.c`). The free-space check (`pin_session.c:54`) and the writer's overflow
path are not exercised; neither is the file being left finalised. **CODE**: replay onto a
small tmpfs or `/dev/full`, plus a fault-injecting writer and a deliberately slow sink.

**5.4 Long captures and file-size limits.**
- Nothing tests the OpenDML segment rollover (1000 MiB segments, 1024 super-index
  entries, `avi_writer.c:28-29`).
- Nothing tests the 32-bit `dwLength`, multi-hour captures (the 8-hour goal) or FAT32's
  4 GiB stop.
- Nothing tests the 16-bit counter wrap at 65,536 frames, which was a real bug before
  (the audio loss at 21m50s).

**CODE**: make the segment limit configurable for tests and write several segments;
push ~70k synthetic frames through the reassembler; use sparse files for >4 GiB.
**HW**: one real 8-hour run per OS.

**5.5 Unplug.** Covered only through the replay device (`test_replay_unplug.c`). Not
covered: a real libusb unplug during bring-up, firmware upload, a deck command or file
finalising; an Android detach. **CODE** (replay failures at each phase), **HW**.

**5.6 Sleep/resume and USB autosuspend.** Nothing handles them. A laptop sleeping
mid-capture leaves an undefined state. Decide the behaviour (stop with "device lost"),
test it with an injected error (**CODE**), confirm on each OS (**HW**).

**5.7 USB stall and drop behaviour.** The 256-transfer queue and 64 MB ring were tuned
once by hand ([usage.md](usage.md)). There is no stress test of a stalled writer.
**CODE** (writer stall with a bounded ring, run under TSan), **HW** (busy disk, hub).

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

## 6. Test infrastructure gaps (all CODE)

- **CI runs nothing automatically.** The workflow is manual and ctest is off by default
  ([ci.md](ci.md)). Android never runs tests. There are no sanitizers. Suggested: a
  Linux push job that runs ctest, plus ASan/UBSan and TSan builds. The core is
  thread-heavy: USB, writer, closer, preview.
- **Fixtures are not committed, and tests that need them skip silently**
  ([../tests/data/README.md](../tests/data/README.md)). A green ctest may have skipped
  most replay tests.
  - The `dv-*.dv` and `hdv.ts` fixtures can be generated with ffmpeg in CMake or CI.
  - The `ep88-*.bin` dumps cannot be regenerated (the raw dump is `#if 0`), so commit
    them (2 MiB each) or keep them in LFS.
  - Under CI, missing fixtures should fail the run.
- **A synthetic HDV TS generator** (from the helpers in `test_hdv_aux.c` and
  `test_hdv_error.c`, or from ffmpeg) would cover most of sections 1 to 3. Needed
  variants:
  - 720p50/59.94, 1080i29.97, 1080 at 24/25/30p;
  - other PIDs, split PSI, a PMT version change;
  - long PES headers, a zero PES length;
  - a GOP timecode jump.
- **Untested modules:**
  - `pin_hdv_audio.c` and `pin_update_net.c` have no tests;
  - `marvin_capture_cli.c` has no ctest (exit codes, `--wait`, option parsing, stdout);
  - `pin_preview.c` is tested only on DV and only with fixtures;
  - the GUIs have no automated tests, which the core-first rule partly offsets.
- **Replay misreads some NTSC `.dv` files as PAL.** Bug (confirmed in code). The replay
  path tries 144,000-byte frames first (`pin_session.c:~2947`). An NTSC file whose frame
  count is a multiple of 6 is a multiple of 144,000 bytes too, for example the 150-frame
  `dv-ntsc-32k.dv`. This affects replay pacing only. Use the DSF bit of the first frame
  instead.
- **HDV remux under damage.** HDV to MOV/MKV is disabled because the remux "does not cope
  with tape errors". `test_hdv_rewrap.c` uses clean data only. Before re-enabling it, add
  a test with dropped TS packets, CC errors and GOP damage.

## A FireWire PC as a test source

A computer with a FireWire port, connected to the MovieBox's 1394 port, can stand in
for a camera. This sits between "pure code" and "real camera". It exercises the real
USB, FPGA and 1394 path with input we control, so we can produce formats nobody owns a
camera for:
- 720p, 24p, pulldown;
- unusual PIDs;
- mid-stream DV/HDV and PAL/NTSC switches;
- 4:3/16:9 and audio-mode changes;
- gaps.

**What the sender has to do, from our side:**

- **Stream only.** If no node answers the oMPR read, we listen on broadcast channel 63
  anyway (`pinnacle_stream.c:244-249`). So a sender that transmits DV (IEC 61883-2) or
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

**Making the content:** ffmpeg can produce all of it:
- DV: PAL and NTSC, 4:3 and 16:9, 32 kHz and 48 kHz, 24p telecined to 29.97.
- HDV-like MPEG-2 TS: 1440x1080 or 1280x720, with set PIDs, set
  `frame_rate_code` and progressive flags, MP2 audio.

Mixed tapes are files concatenated at the frame or TS-packet level. Each output can be
checked against the input byte for byte, which a real tape never allows.

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
4. **Then the sender itself:** a transmit loop in `src/core` and a `--transmit
   file.dv|file.ts` (or test-only) mode, with the receiving unit selected by
   `--device`.

The vendor-driver route also gives a no-code shortcut. With a camera attached, a
Windows PC with the Pinnacle driver is itself a "FireWire PC". But it can only send
DV to a camera it has found, not to another Marvin, so it does not replace step 4.

**Bus and topology notes:**
- Each 500-USB wires only PHY port 0, so two units take the one cable between them.
  A camera on the same bus as well needs a FireWire hub or repeater (a 700-USB may
  have more ports; untested).
- Both units set themselves cycle master in link init (`pinnacle_1394.c:991`). That
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

1. **CODE, cheap, finds real bugs:**
   - the confirmed bugs in 1.1, 1.2, 1.3, 3.3, 3.9, 4.1 and the replay NTSC/PAL guess;
   - the fake AV/C link;
   - the synthetic HDV TS generator;
   - CI with generated fixtures and sanitizers.
2. **Decide policies:** what a format, standard, aspect or audio change does to the
   open file (3.1-3.7), and what NOT_IMPLEMENTED means (4.1).
3. **FW-PC** (first check whether a second Marvin can transmit, step 1 of that section):
   - check stream-only capture on channel 63;
   - then 720p, 24p and mixed DV/HDV and PAL/NTSC streams;
   - then a scripted fake deck, if available.
4. **HW:**
   - the camera matrix (4.10), at least one Sony and one Panasonic/JVC;
   - a JVC 720p source;
   - a DVX100 24p/24pA tape and a Canon 24F tape;
   - a real mixed DV/HDV tape;
   - LP and 32 kHz tapes;
   - an 8-hour run;
   - unplug, bus reset and sleep;
   - the untested Pinnacle models and hosts.
