---
name: frame-error-stats
description: How the core counts frames with errors (DV STA/audio/missing blocks, HDV TS/GOP damage) and where the counters live in the status struct; traps when changing the classification. Read before touching dv_error.c, hdv_error.c or the frames_error / clip_* status fields.
---

# Frame error stats

Code: `src/engine/dv_error.[ch]` (stateless, per DV frame) and
`src/engine/hdv_error.[ch]` (stateful per HDV picture). Called from
`dv_on_unit()` in `pin_session.c` via `count_frame()`; the counters reset in
`reset_frame_counters()` (capture start, input switch) and the clip counters in
`open_sink_for_scene()` (every new file). Fields: `pin_api.h` (appended after
`duration_remaining_s`), mirrored in `Interop/NativeStructs.cs`.

Traps (all learned the hard way):

- DIF block layout per sequence of 150: 0 header, 1-2 subcode, 3-5 VAUX, then 9
  groups of 16 = 1 audio + 15 video. DBN of video is `group*15 + k-1`. The
  byte-1 low 3 bits are always 1s, so a zero-padded sequence is detected by that
  even for block 0 of sequence 0. STA is the HIGH nibble of byte 3 of a video
  block (the low nibble is QNO, not an error).
- All-zero audio blocks are normal (silence, unused second channel pair of
  32 kHz/4-channel recordings: 34-40 of 45 blocks in a camera's recording). Only a
  PARTIAL zero pattern in the first half of the sequences counts (Sony mute).
- A raw EP 0x88 dump of a real camera contains real losses (shifted blocks): all
  its frames are "with error"; do not assert zero errors on one. The fixtures the
  suite generates (tests/data/README.md: the `.dv` files, `hdv.ts`, and the
  `ep88-*.bin` dumps `make_ep88` wraps around them) are clean and are asserted to be.
- HDV: one unit = one picture (video PES start to the next). A continuity gap on
  the first packet of a unit is charged to that unit (the loss is between two
  pictures). PES_packet_length is 0 for HDV video; only checked if non-zero.
  Continuity tracking must restart after a stream break (`hdv_err_reset`, >1 s
  without data) or every rewind/pause shows a false loss.
- Clip counters count at arrival, not at commit, so units held by the split
  lookahead (and by the hold-back FIFO, `pin_holdback.h`, for a scene cut or a
  format change) are charged to the previous clip.
- A PAL / NTSC change no longer pads NTSC frames to 12 sequences: the reassembler
  sizes each frame by its own DSF bits / sequences 10-11, so a clean mixed tape has
  no error frames (`test_replay_mixed_dv` asserts zero). Units of the other kind
  (DV <-> HDV) are counted at arrival like any unit, even those a file never gets.
- The HDV replay (`replay_run_ts()`) hands what precedes the first video PES
  (SDT, PAT, PMT) to the write callback only, like the reassembler's PAT/PMT
  unit: on `dv_on_unit()` it would count as a picture with a PES error.
- HDV into MOV / MKV is different: its frames are not "with error" at arrival
  (`dv_on_unit()` counts them with err 0); the file's sink counts them itself
  (`sink_hdvmux.c`: re-encoded frames, frames repeated for lost pictures, frames whose
  audio was replaced by silence; `pin_sink_status_t.frames_error` / `frames_repeated`).
  The status adds the closed files' counts (`sinkf_error` / `sinkf_repeated`, topped up by
  the closer from the sink's `final_status`) and the open sink's (asked in the snapshot).
  `frames_dropped` there = the repeated frames. `test_hdv_repair` checks the sink's counts,
  `test_replay_hdv_mov` the status.
- The only shipped views of these counters are the status line / snapshot fields
  of `MarvinCaptureCLI` and the GUI; the old CIP/DBC continuity line of `pincli` is
  gone (`dbc_gaps` / `dbc_joins` still live in `dv_reassembler.h` and the replay
  baseline test checks them; so do the format-change counters `format_switches`,
  `fmt_dropped_packets`, `std_switches`, `ts_pid_switches`, `ts_pic_overflows`,
  printed by `tests/replay_reassembler` and checked by `test_reassembler_mixed`,
  not in the status). Check a change with the replay tests and, for the
  live behaviour, a `MarvinCaptureCLI --debug` capture.
