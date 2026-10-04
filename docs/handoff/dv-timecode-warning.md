# DV "Detected timecode is invalid" warning - investigation

## Verdict

Not a data bug on our side (confidence: high, ~90%), and the 559d294 filter is
logically correct. The most likely reason the user still sees it: **the binary in
use predates the fix.** `build/windows-x86_64/{core,dist}/marvin-core.dll` is
timestamped 2026-10-04 15:50:24; commit 559d294 is 15:51:41 (and the
later pin_preview.c commit f4f170e is 16:09). `build/dist/` (older GUI/CLI copy) is from
08:05. Only the GUI exe was relinked at 16:00; the core DLL (where the FFmpeg and
the filter live) was not. Rebuild the core and retest before changing anything.

If it still appears after a rebuild, the secondary suspect is that FFmpeg's message is
emitted before our callback is installed or through a path where the callback is not
installed (see "Callback coverage"); that is a code hole worth closing anyway.

## Exact message and emitting sites

`Detected timecode is invalid` at AV_LOG_ERROR, logged with `s` (the AVFormatContext) as context.
Single site in the bundled FFmpeg 8.1.3:

- third_party/ffmpeg-src/ffmpeg-8.1.3/libavformat/dv.c:569, in `dv_read_timecode()`
  (dv.c:546). Called from `dv_read_header()` at dv.c:622, only when
  `s->pb->seekable & AVIO_SEEKABLE_NORMAL`.
- It reads 3 DIF blocks (header + 2 subcode blocks) and calls `dv_extract_timecode()`
  (dv.c:336) -> `dv_extract_pack(frame, DV_TIMECODE)` (dv.c:99), which for
  DV_TIMECODE checks exactly ONE offset: `80*1 + 3 + 3` = byte 86 = DIF sequence 0,
  subcode block 0, SSYB slot 0, pack byte; passes only if it equals 0x13.
  Otherwise the error is logged. The result only fills a `timecode` metadata tag.

No other emitter: the dv decoder, dvenc muxer, mov/avi muxers and
libavutil/timecode.c do not contain that string. (grep of the whole ffmpeg-src tree.)

## Our call paths that trigger it

Only one: src/sinks/sink_rewrap.c `rewrap_dv_extract_audio()` (line 190), called
for EVERY frame from `dv_write_unit()` (line 456) for the AVI/MOV/etc. DV
sinks. It builds a fresh in-memory AVIOContext with a seek callback
(`mem_seek`, line 144), so the context is seekable, and calls
`avformat_open_input(..., dv_fmt)` (line 205) -> `dv_read_header` ->
`dv_read_timecode`. So one error per frame, per capture to a rewrap DV container.
Not triggered by: sink_raw (.dv passthrough), HDV (`hdv_remux` opens a TS, line 547),
the preview (uses the decoder only, not the demuxer), scene split, or dv_subcode.c
(our own parser). No `avformat_find_stream_info` is used on this path.

## Callback coverage (does the filter apply?)

- `av_log_set_callback` is called from exactly one place: pin_preview.c:125
  `av_log_install()`, via pthread_once from `pin_previewer_create()` (line 129),
  which `pin_session_open` calls unconditionally (pin_session.c:2841). The
  callback is process-global in FFmpeg (no per-context callback), so it covers
  the sink_rewrap demuxer too, which runs in the same DLL/process.
- pv_av_log (pin_preview.c:101): `fmt` is the raw format string
  "Detected timecode is invalid\n"; `strstr(fmt, "timecode is invalid")` matches,
  level becomes AV_LOG_DEBUG+1 and is routed to pin_logf at PIN_LOG_DEBUG. So
  the filter works for this message, given a build containing it.
- Gaps (real but secondary): (1) any FFmpeg message emitted before the first session is created, or from a
  sink used without a session, goes to FFmpeg's default stderr callback; (2) the
  callback and filter live in the preview module, so a build/config without the
  previewer would lose it; (3) the fix is a text match on a format string, which
  breaks if FFmpeg is updated and rewords it.

## What 559d294 changed

Only src/engine/pin_preview.c, +6 lines: in `pv_av_log`, if the format contains
"timecode is invalid", level = AV_LOG_DEBUG + 1. Nothing else.

## Are WE producing frames with slot 0 = 0xFF? No.

- src/core/dv_reassembler.c copies each 12000-byte DIF sequence verbatim into
  `frame_buf + dseq*12000` (lines 141, 162); only the frame buffer is zeroed on
  resync (line 140). No subcode is rewritten or synthesised anywhere in
  src/core or src/sinks (sink_raw and sink_rewrap pass the buffer through).
  Sequence/block order is the camera's own (DIF ID bytes below are
  consistent: sequence number in nibble, block 0x00/0x01).
- tests/data samples show cameras differ: dv-ntsc-32k.dv has `13 00 80 80 c0`
  in slot 0 (FFmpeg works); dv-ntsc.dv has 0xFF in all slots (FFmpeg errors).

### Dump of f.dv (PAL, 250 frames, 144000 B/frame, header 1f 07/27/47.. 00 3f)

Subcode block layout: 3-byte DIF ID + 6 SSYBs x 8 bytes (3 ID bytes + 5-byte pack);
pack ID = SSYB byte 3. Frame 0, sequence 0, subcode block 0 (offset 80):

    id=3f0700  [0] 8790 ff ffffffffff   <- offset 86 = ff  (what FFmpeg reads)
               [1] f001 ff ffffffffff
               [2] f002 ff ffffffffff
               [3] f793 ff 13 46 00 00 00   TC pack 0x13  (frame 46? bytes 46 00 00 00)
               [4] f004 ff 14 00 00 00 00   pack 0x14
               [5] f005 ff 13 46 00 00 00   TC pack 0x13

Identical structure in subcode block 1 and in all 12 sequences of all frames
(sequences 5-9 of the first frame use SSYB ID bytes 0x70/0x00 instead of
0xf0/0x80, i.e. the second half of the track, same layout, pack IDs 0x13/0x14 in
slots 3-5, slots 0-2 = ff). Stats over all 250 frames for seq 0 and 6:
slot 0,1,2 = 0xFF in 250/250 frames; slot 3 = 0x13 in ~91%, slot 5 = 0x13 in ~54%,
slot 4 = 0x14 in ~53% (the rest 0xFF; slot 3 hit in ~91% of frames, so the TC
can also be missing in a given sequence). The TC counts continuously
(46,47,48,49 ...) and there is no difference between sequence 0 and the others, so
the "first sequence differs" hypothesis is false. Header DIF: DSF=0 (PAL), APT=0,
TF bits 13/15 as expected. FFmpeg reading slot 0 of seq 0 block 0 therefore
always sees 0xFF for this camera, while dv_subcode.c (which votes across all
sequences/slots, dv_subcode.c:242) finds the timecode.

Conclusion: the data is a faithful copy of what the camera sends; FFmpeg's
timecode reader is simplistic (single hard-coded slot) and this camera does not
use slot 0. Not a driver bug. (Scripts: scratchpad an.py, not committed.)

## Recommended fix

1. Rebuild the core (`scripts/build.ps1`) so marvin-core.dll contains 559d294/f4f170e
   and verify the warning is gone (it should then only show with debug logging).
2. Make the suppression robust and not dependent on message text/the previewer:
   - Move the av_log callback install out of pin_preview.c into a small
     shared init (e.g. called from `pin_session_open` before anything else, pin_session.c:2841,
     or a `pin_ffmpeg_log_init()` in src/engine) so every FFmpeg user is covered.
   - Better: avoid the call altogether in src/sinks/sink_rewrap.c:190-205 by
     bypassing `dv_read_timecode`: make the in-memory AVIOContext non-seekable
     (pass `seek = NULL` to `avio_alloc_context`, sink_rewrap.c:200; dv.c:621 then skips
     `dv_read_timecode`). Check that read-ahead behaviour stays the same (the
     function relies on seeking only for the AVSEEK_SIZE/header probing; test with
     tests/data/dv-ntsc-32k.dv and ep88-ntsc.bin audio checks). This removes the
     per-frame pointless 240-byte read+seek and the message at the source.
     Alternative: set `in->flags`/`AVFMT_FLAG_` or temporarily lower the context level
     with `av_log_set_level` is not per-context and is not recommended.
3. Do NOT patch/synthesise subcode slot 0 in captured frames; the frames are correct.
