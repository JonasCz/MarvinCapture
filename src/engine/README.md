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
  discovery, MPEG-2 GOP header time_code decoding (solid, spec-derived),
  TS discontinuity_indicator detection, and a best-effort scan of the HDV
  AUX private stream for DV-style date/time/timecode packs.

- **`dv_error.[ch]`**, **`hdv_error.[ch]`** — per-frame / per-picture error
  classification behind the "frames with error" counters (dvrescue-style DV
  STA / audio-fill / missing-block checks with camera quirks from dvmerge;
  HDV TS error indicator, continuity gaps, PES/picture header checks and
  damage propagation through the GOP). Tests: `test_dv_error`,
  `test_hdv_error` (synthetic frames plus the `tests/data` fixtures).

- **`pin_scene.[ch]`** — dvgrab-style scene-split detector: feeds one
  frame/GOP of metadata at a time, debounces a break over N consecutive
  frames before confirming it, and reports the true first anomalous frame
  so the caller can back-date the cut. Also tells the caller how many
  trailing frames it must hold back before finalising a file.

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
  save and the per-platform default config file path.

- **`pin_cmdline.[ch]`** — the command-line parser shared by every GUI and
  by `pinctl`: one-shot capture presets (optionally seeded from an INI file
  via `--preset`, with explicit flags overriding it) plus an ordered action
  list (`--actions rewind,play,capture`) for a core action sequencer.

## Session engine (needs libusb/FFmpeg: not part of `pinnacle_engine_pure`)

`pin_session.c`/`pin_deck.c`/`pin_preview.c` (built into `pinnacle_engine`,
one level up in `CMakeLists.txt`) are the actual worker-thread state machine
behind `pin_api.h`: device open/prepare, capture start/stop, deck control,
scene splitting (feeding `pin_scene.c` per DV frame or per HDV GOP -- see
`dv_on_unit()`'s two branches) and preview decode.

- **Replay (virtual device)**, `PIN_REPLAY=<file>` or `pin_set_replay_file()`
  (also reachable as `--device <path to an existing file>` on any front end
  built on `pin_cmdline.c`, or by passing that path straight to `pin_open()`):
  plays a recorded source back through the whole pipeline with no hardware,
  so the engine (and any GUI built on it) can be developed and tested end to
  end -- see `pin_session.c`'s `replay_run()`. Its
  `pin_device_info_t.serial` is always the fixed string `"REPLAY"`. Three
  source shapes, picked by extension:
  - `.dv`: a plain sequence of frame-aligned DIF frames (144000 bytes/frame
    PAL, 120000 NTSC), played back frame by frame.
  - `.ts`: a plain MPEG2-TS file (`replay_run_ts()`), split into "picture"
    units at video PES starts (tracking the PAT/PMT via `hdv_aux.h`) exactly
    as a real HDV capture would be, then fed through the same
    `dv_write_cb()`/`dv_on_unit()` path a real capture uses.
  - anything else: treated as a raw EP 0x88 dump and fed through
    `dv_reassembler_feed()` (the same reassembler a real capture uses).
  - Every source loops at EOT, driving the same pass-advance / stop logic
    the real async transport-state poll would on hardware (see
    `replay_handle_eot()`).
- **HDV scene split**: unlike DV (which has per-frame timecode/date/time),
  HDV scene splitting has only the MPEG-2 GOP header's `time_code` per
  picture to go on (`hdv_parse_picture()`); it uses the same debounce FIFO
  and `pin_scene_feed()` as DV, just fed once per GOP instead of once per
  frame.
- Hardware-free regression tests for all of the above live in
  `tests/engine_replay/` (driven only through `pin_api.h` against the
  replay device -- see that directory's own comments), not here.

## Building and testing standalone

This directory is normally built as part of the top-level `CMakeLists.txt`
(`add_subdirectory(src/engine)`), which also builds the session engine
above, `src/sinks` and `pinnacle-oss-core`/`pinctl`:

```
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

`pinnacle_engine_pure` (the hardware-free modules at the top of this file)
can still be exercised on its own from a scratch CMake project that just
`add_subdirectory()`s this directory and calls `enable_testing()`, if you
want the fast subset without libusb/FFmpeg in the loop.
