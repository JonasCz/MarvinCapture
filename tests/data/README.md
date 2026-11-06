# Test fixtures

The recordings the replay, reassembler and sink tests run on are **made at test time**,
not committed. The ctest fixture-setup test `pin_test_data` runs
[`tests/make_fixtures.cmake`](../make_fixtures.cmake), which makes them with the `ffmpeg`
command-line tool and [`tests/make_ep88.c`](../make_ep88.c) into the build tree,
`build/<os>-<arch>/core/tests/data/` (not this folder). ctest runs it before any test that
needs the files; it makes them again only when the script, `make_ep88` or the ffmpeg path
changed. It takes about a second.

| File | What it is | Used by |
|---|---|---|
| `dv-pal.dv` | PAL DV, 50 frames (2 s), 48 kHz 16-bit audio | `test_replay_hwenc` |
| `dv-ntsc.dv` | NTSC DV, 30 frames (1 s), 48 kHz 16-bit audio; 3,600,000 bytes, also a whole number of PAL frames, so only the DSF bit tells it is NTSC | `test_dv_audio_vs_libavformat`, `test_replay_unplug`, `test_hwenc_sink`, `test_dv_error`, the source of `ep88-ntsc.bin` |
| `dv-pal-32k.dv` | PAL DV, 14 frames (0.56 s), 32 kHz 16-bit audio | `test_dv_audio_vs_libavformat`, the source of `ep88-pal.bin` |
| `hdv.ts` | HDV-like MPEG-2 TS, 2 s: 1440x1080 interlaced (top field first), 25 fps, 12-picture GOPs with a GOP timecode from 00:10:00:00, BT.709, MP2 audio; the PIDs of a real camera (PMT 0x81, video 0x810, audio 0x814) and a clock that starts at 834 s | `test_replay_hdv`, `test_replay_unplug`, `test_hdv_rewrap`, `test_hdv_error`, `test_pin_vidfmt` |
| `hdv-720p.ts` | HDV 720p-like MPEG-2 TS, 0.3 s: 1280x720 progressive, 59.94 fps, 16:9 (square samples), 6-picture GOPs, MP2 audio; ffmpeg's own PIDs (PMT 0x1000, video 0x100, audio 0x101) | `test_replay_hdv`, `test_hdv_rewrap` |
| `hdv-split-a1.ts`, `-x.ts`, `-a2.ts`, `-b.ts` | Pieces encoded as `hdv.ts`, each with its own GOP timecode, the clock running on from one to the next: a1 34 pictures from 01:00:00:00, x one GOP with a stray timecode (03:00:00:00), a2 36 pictures from 01:00:01:21 (a1's timecode going on), b 48 pictures from 05:00:00:00; the test joins them into one tape | `test_replay_hdv_split` |
| `hdv-repair.ts` | As `hdv.ts`, 6 s, with a 997 Hz tone (a whole number of 1 kHz periods fits in an MP2 frame, so a 1 kHz tone a frame early or late would look right); clock at 834 s; open GOPs | `test_hdv_repair` (damaged there in many ways) |
| `hdv-repair-closed.ts` | The same, 3 s, closed GOPs, clock at 900 s | `test_hdv_repair`, `test_replay_hdv_mov` |
| `hdv-repair-jump.ts` | The same, 1.5 s, clock at 5000 s, timecode from 00:20:03:00 (where `hdv-repair-closed.ts`'s ends): joined after it, only the camera clock jumps | `test_hdv_repair`, `test_replay_hdv_mov` |
| `ep88-pal.bin` | Raw EP 0x88 dump of `dv-pal-32k.dv` (2 MiB, short like a cut-down dump of a real camera: the replay tests count on it ending within a second) | `reassembler_ep88_pal`, the PAL replay tests (`test_replay_*`, `test_pin_script_run`, `test_pin_check_output`), `test_dv_rewrap`, `test_hwenc_sink` |
| `ep88-ntsc.bin` | Raw EP 0x88 dump of `dv-ntsc.dv` | `reassembler_ep88_ntsc`, `test_dv_rewrap` |
| `ep88-pal-ntsc.bin` | Tape that changes standard: `dv-pal.dv` then `dv-ntsc.dv` in one dump | `test_reassembler_mixed`, `test_replay_mixed_dv` |
| `ep88-dv-hdv-dv.bin` | DV, HDV, DV: `dv-pal-32k.dv`, `hdv.ts`, `dv-ntsc.dv` | `test_reassembler_mixed`, `test_replay_mixed_kind` |
| `ep88-dv-stray.bin` | `dv-pal-32k.dv`, the first 300 TS packets of `hdv.ts` (too short a burst to switch), `dv-pal-32k.dv` | `test_reassembler_mixed` |
| `ep88-hdv-pid.bin` | `hdv.ts` then `hdv-720p.ts`: the video PID (0x810 to 0x100), PMT and format change | `test_reassembler_mixed` |
| `ep88-pal-48k-32k.bin` | Audio mode change: `dv-pal.dv` (48 kHz) then `dv-pal-32k.dv` (32 kHz) | `test_replay_mixed_dv` |
| `ep88-hdv-dv.bin` | A capture that starts on HDV: `hdv.ts` then `dv-pal.dv` | `test_replay_mixed_kind` |

The DV and TS files are ffmpeg's `testsrc2` pattern and a tone (the exact commands are in
`make_fixtures.cmake`). The `ep88-*.bin` dumps are what the device would send for those
DV files: `make_ep88` wraps each DV frame into IEC 61883 packets, OHCI isochronous-receive
records and type-9 messages, with a few hundred bytes of junk in front as when a read joins
a running stream (its header comment has the details). Reassembled, a dump gives back its
`.dv` file byte for byte, which is what the `reassembler_ep88_*` tests check.

`make_ep88 [--no-junk] <out.bin> <in.dv|in.ts>[@N]...` takes several inputs, wrapped one
after the other into one stream, each detected by its content: whole DV frames (PAL or NTSC
per frame, from the DSF bit) or an MPEG-2 TS (as IEC 61883-4 source packets). `@N` takes
only an input's first N DV frames or TS packets.

**Without ffmpeg** (configure says so with a warning; `-DPIN_TEST_FFMPEG=OFF` configures as
if there were none) the tests that need these files print `SKIP` and pass, except when the
environment variable `CI` is set (GitHub Actions sets `CI=true`): then they fail, so a CI
run cannot pass by skipping. Install ffmpeg (docs/building.md, or `scripts/install-deps.sh
--tests`, `scripts\install-deps.ps1 -Tests`) and configure again.

## What ffmpeg cannot make: optional recordings in this folder

This folder (`tests/data/`, not committed apart from this file) can hold a recording that
the generated files cannot stand in for. Tests use it when it is there and say
"not present ... not checked" otherwise:

| File | What it is | Used by |
|---|---|---|
| `dv-ntsc-32k.dv` | A camera's NTSC DV with 32 kHz **12-bit nonlinear** audio, 4 channels (ffmpeg's DV muxer writes 16-bit stereo only, and 32 kHz only at 25 fps) | `test_dv_audio_vs_libavformat` (the only check of `dv_audio_12to16()` against FFmpeg), `test_dv_error` |

Also not covered by any generated file, so only by a real HDV camera: the aux stream on
PID 0x811 (recording date/time). `test_hdv_aux` checks `hdv_aux_scan_datetime()` on a
hand-built pack only; no test reads a camera's real aux data.

Make a recording with `MarvinCaptureCLI --capture file.dv`; cut it short, a couple of
MiB is plenty.
