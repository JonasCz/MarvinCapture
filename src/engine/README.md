# src/engine — hardware-free core modules

Pure C11 + libc (plus a few guarded platform calls for settings paths and
atomic file replace). No libusb, no FFmpeg. Built as the static library
`pinnacle_engine_pure` (see `CMakeLists.txt`); unit-tested with plain C test
executables in `tests/engine/` (no framework, nonzero exit = failure).

- **`dv_subcode.[ch]`** — parses one reassembled DV frame's DIF blocks into
  timecode (SSYB pack 0x13), recording date/time (VAUX packs 0x62/0x63),
  16:9 aspect (VAUX pack 0x61) and audio format (AAUX pack 0x50), voting
  across every redundant copy of each pack in the frame. Also a best-effort
  "rec start" flag (AAUX pack 0x51) — see the file header comment for the
  research trail and its confidence level.

- **`hdv_aux.[ch]`** — minimal MPEG2-TS parsing for HDV: PAT/PMT PID
  discovery, the latest PAT/PMT packets to start a file with (`hdv_psi_scan()`),
  MPEG-2 GOP header time_code decoding (solid, spec-derived),
  TS discontinuity_indicator detection, and a best-effort scan of the HDV
  AUX private stream for DV-style date/time/timecode packs.

- **`dv_error.[ch]`**, **`hdv_error.[ch]`** — per-frame / per-picture error
  classification behind the "frames with error" counters (dvrescue-style DV
  STA / audio-fill / missing-block checks with camera quirks from dvmerge;
  HDV TS error indicator, continuity gaps, PES/picture header checks and
  damage propagation through the GOP). Tests: `test_dv_error`,
  `test_hdv_error` (synthetic frames plus the `tests/data` fixtures).

- **`pin_estimate.[ch]`** — capture size estimates: default data rates (DV and
  HDV 13 GB/h, analog AVI computed from the picture size, FFV1 30 GB/h), the
  FFV1 rate learned from the last 10 minutes of a capture
  (`core.ffv1_bytes_per_hour` in the core settings) and the rolling window that
  measures the live rate. Test: `test_pin_estimate`.

- **`pin_scene.[ch]`** — dvgrab-style scene-split detector: fed one DV
  frame or HDV picture of metadata at a time (HDV: a timecode only on the
  picture with the GOP header), debounces a break over N consecutive
  timecoded records (DV 5 frames, HDV 3 GOPs) before confirming it, and
  reports the true first anomalous frame so the caller can back-date the cut.
  Also tells the caller what it must hold back before finalising a file (a
  window of frames, or from the pending break's first frame).

- **`pin_holdback.[ch]`** — the hold-back FIFO in front of the split
  lookahead: units not written yet, released by a fixed window or from the
  earliest index a pending cut may still land on, and a cut that lands on a
  unit a file may start with (an HDV picture that opens a GOP). Pure
  (callbacks), unit tested in `tests/engine/test_pin_holdback.c`.

- **`pin_fmtwatch.[ch]`** — the DV / HDV format on a tape that mixes formats:
  a watcher fed what each unit says (kind; DV standard, aspect, audio rate,
  channels, bits; HDV size, frame rate, progressive from a sequence header),
  that confirms a change only after it persists (DV 5 frames, HDV 3 GOPs; units
  without the value are neutral) and reports its first unit, and the decision
  what a confirmed change does to the open file (`pin_format_change_action()`:
  keep it, a new file, or end a pipe). Pure, unit tested in
  `tests/engine/test_pin_fmtwatch.c`.

- **`pin_split.[ch]`** — split lookahead: a scene cut only becomes a new
  file once the new segment has >= 1 MB and >= 1 s; until then its units are
  held, and a later cut or the end of the capture/pass writes them to the
  previous file instead. Pure (callbacks), unit tested in
  `tests/engine/test_pin_split.c`.

- **`pin_naming.[ch]`** — output filename generation (`base.ext`,
  `base-0001.ext`, `base-pass-2.ext`, `base-pass-2-0001.ext`) and a
  collision check against existing files, Windows- and POSIX-path aware.

- **`pin_settings.[ch]`** — tiny INI settings store (load/save,
  string/int/double/bool accessors) with an atomic (write-temp-then-rename)
  save and the per-platform default config file path, plus
  `pin_settings_device_key` (the `dev_<GUID>.` per-device key prefix).

- **`pin_script.[ch]`, `pin_script_eval.[ch]`** — the command-line language of
  [docs/cli.md](../../docs/cli.md), shared by every front end: the parser
  (settings and actions left to right into a script, all validation that needs
  no device), timecode/duration parsing, the file-extension to format table,
  the help text, and the pure wait-condition evaluation (`idle`, `signal`, `nosignal`,
  `timecode`, `wallclock`, `captured`; driven by a made-up clock in the tests).
  The sequencer that runs a script against a session is `pin_script_run.c`
  (below).

## Session engine (needs libusb/FFmpeg: not part of `pinnacle_engine_pure`)

`pin_session.c`/`pin_deck.c`/`pin_preview.c` (built into `pinnacle_engine`,
one level up in `CMakeLists.txt`) are the actual worker-thread state machine
behind `pin_api.h`: device open/prepare, capture start/stop, deck control,
scene splitting and format changes (`dv_on_unit()` reads each DV frame or HDV
picture, `capture_unit()` holds it, feeds `pin_scene.c` and `pin_fmtwatch.c`
and cuts) and preview decode.

- **Script sequencer** (`pin_script_run.c`, `pin_script_run()` / `pin_script_cancel()`):
  a helper thread per session that performs a parsed script. It waits after each
  deck command until the worker took it and `deck_busy` cleared (the command
  mailbox has one slot), closes an open capture before any transport action,
  recognises *its* capture ending (`capture_end_seq` in the session; the
  snapshot's `stop_reason` can still be the previous capture's) and aborts with
  exit code 4 on an abnormal end, and ends with `PIN_EVT_DONE` (`a` = exit
  code, `text` = reason). `pin_session_close()` cancels and joins it.
  The collision check for `--capture` looks at the file name of every kind that
  could arrive (DV and HDV on the DV input: a tape can change between them).

- **Replay (virtual device)**, `PIN_REPLAY=<file>` or `pin_set_replay_file()`
  (also reachable as `--device <path to an existing file>` on any front end
  built on `pin_script.c`, or by passing that path straight to `pin_open()`):
  plays a recorded source back through the whole pipeline with no hardware,
  so the engine (and any GUI built on it) can be developed and tested end to
  end -- see `pin_session.c`'s `replay_run()`. Its
  `pin_device_info_t.serial` is always the fixed string `"REPLAY"`. Three
  source shapes:
  - `.dv`: a plain sequence of frame-aligned DIF frames (144000 bytes/frame
    PAL, 120000 NTSC, by the DSF bit of the first header block), played back
    frame by frame.
  - an MPEG2-TS file (0x47 at the start of every 188-byte packet, or named
    `.ts` / `.m2t`; `replay_run_ts()`), split into "picture"
    units at video PES starts (tracking the PAT/PMT via `hdv_aux.h`) exactly
    as a real HDV capture would be, then fed through the same
    `dv_write_cb()`/`dv_on_unit()` path a real capture uses.
  - anything else: treated as a raw EP 0x88 dump and fed through
    `dv_reassembler_feed()` (the same reassembler a real capture uses).
  - Every source loops at EOT, driving the same pass-advance / stop logic
    the real async transport-state poll would on hardware (see
    `replay_handle_eot()`).
- **HDV scene split**: unlike DV (which has per-frame timecode/date/time),
  HDV scene splitting has only the MPEG-2 GOP header's `time_code` to go on
  (`hdv_parse_picture()`), in the picture that opens each GOP. It feeds
  `pin_scene_feed()` once per picture (the others without a timecode) with
  `pin_scene_config_hdv()` (3 GOPs, `sparse_tc`), holds every picture from a
  pending break's first one in the hold-back FIFO (`pin_holdback.h`) and
  cuts on that GOP's first picture; the new file starts with the latest
  PAT/PMT (`hdv_psi_scan()` in `dv_write_cb()`). See [docs/hdv.md](../../docs/hdv.md).
- **Format changes** (a tape that mixes DV and HDV, PAL and NTSC, aspect or
  audio modes): the status shows the confirmed format (`pin_fmtwatch.h`), so a
  single odd frame changes nothing. During a capture every unit goes through the
  hold-back FIFO; a confirmed change is cut at its first unit
  (`capture_format_change()`): a new file with the new format (`format_cut_begin()`,
  numbered `-0002`... after the first without `--split`), the same file when it
  carries the change (raw DV aspect / audio), or the end of a pipe on DV <-> HDV.
  `file_kind` is the open file's kind; units of another kind never reach it
  (`commit_unit_fi()`). See [docs/usage.md](../../docs/usage.md).
- Hardware-free regression tests for all of the above live in
  `tests/engine_replay/` (driven only through `pin_api.h` against the
  replay device -- see that directory's own comments), not here.

## Building and testing standalone

This directory is normally built as part of the top-level `CMakeLists.txt`
(`add_subdirectory(src/engine)`), which also builds the session engine
above, `src/sinks` and `marvin-core`/`MarvinCaptureCLI`. The build scripts
(`scripts/build.sh` on macOS/Linux, `scripts\build.ps1` on Windows) do all of
it; after one script run, iterate in the CMake tree they leave in
`build/<os>-<arch>/core` (e.g. `build/macos-arm64/core`):

```
cmake --build build/macos-arm64/core
ctest --test-dir build/macos-arm64/core --output-on-failure
```

`pinnacle_engine_pure` (the hardware-free modules at the top of this file)
can still be exercised on its own from a scratch CMake project that just
`add_subdirectory()`s this directory and calls `enable_testing()`, if you
want the fast subset without libusb/FFmpeg in the loop.
