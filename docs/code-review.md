
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


### 17.4 Wrong numbers on screen

- **R25. TEI or a bad sync byte on any PID taints the following pictures.** Low. Testing required.
  - `own = tei || sync_lost || ...` counts a TEI or bad sync byte on any PID, so one on an audio or PSI packet taints the following P and B pictures. The skill file says an audio-only gap does not spread; that holds for CC gaps only. Probably never set on FireWire HDV. Testing required.
- **R27. Encoder sinks: a writer-ring drop shifts A/V.** Low. Testing required.
  - In the encoder sinks a writer-ring drop also shifts A/V. Video frames and audio blocks are dropped independently, and the sinks ignore the unit index, so each dropped video frame moves later video one frame early against audio (and the reverse for audio). Same burst-before-DISK_SLOW window.
- **R31. Thread status read without a lock.** Low. Testing required.
  - `hand_over_file()`, `file_bytes_due()`, `capture_guard()` and `pin_session_get_status()` read `sink->get_status` while the writer thread updates it. Values can be stale or torn; no crash path was found.
  - Relevant to the TSan item in 13.

### 17.5 Stop conditions and scene cuts

- **R33. REC START held over several frames pushes the scene cut late.** Low. Testing required.
  - In `pin_scene.c` `classify()`, every flagged frame restarts the debounce. The cut lands on the last flagged frame, and the first frames of the new scene go into the old file (2).

### 17.6 Smaller items

- **Thread priority inheritance** (Linux). Medium. Testing required.
  - `pin_thread_boost()` sets SCHED_FIFO 10 or nice -10 on the USB thread. The writer and encoder threads are created from that thread with default attributes, so they inherit the boost.
  - CPU-heavy encoders could then delay the USB reaper or starve the GUI.
- **OHCI `xferStatus` is not checked** (`iso_header_ok()`). Low. Testing required.
  - Packets that completed with an error event are accepted as data. Count them on real dumps (normal is `0x8411`).
- **EP 0x84 resubmit loop** (`ep84_cb`). Low.
  - A stalled endpoint spins the loop, because every status but CANCELLED and NO_DEVICE resubmits. A record longer than 512 bytes is lost.
  - At stop, transfers that completed but were not yet delivered (up to about 290 ms) are discarded.
  - Second pass: the completed-but-undelivered transfers discarded at stop are those of the EP 0x88 read loop (`pinnacle_stream_read_loop_ex()`), not EP 0x84.
- **Device locks.** Low. Testing required.
  - Windows: a thread-owned mutex lets the same thread open a device twice. If the opening thread exits, the abandoned mutex is accepted by the next process.
  - Windows: the `Local\` mutex is per logon session, so a second logged-in user is not excluded.

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

## 18. Code review, second pass: data loss and wrong counts

A second reading of the code (November 2026), after the fixes in 4a49606, 5f66c1e, 5e2317f,
b712741, 6f94979, ebedc07 and b43f2f6. MarvinCapture only (core, GUIs, CLI, scripts; not
MarvinPlayout). Same marks as section 17: **Fix required** or **Testing required**, severity
high, medium or low. "Reproduced" means a scratch harness that feeds synthetic data to the
static functions showed it. The rest comes from reading the code. Additions to section 17
items are marked "Second pass" under those items.

### 18.1 Frames lost, mangled or put in the wrong file

- **N6. fmtwatch provisional start moves real old-format units** (`pin_fmtwatch_feed()`: `*start = provisional ? first_start : cand_start`, from b43f2f6). Low. Testing required.
  - A capture that starts on the last 1..4 frames (DV) or 1..2 GOPs (HDV) of recording A, followed by B: the cut is placed at `first_start`, so A's units go into B's file. For DV -> HDV `commit_unit_fi()` drops them silently (still counted in `frames`); for PAL -> NTSC, PAL frames land in an NTSC file.
  - Fix idea: when 2 or more first units agree, cut at `cand_start`.
- **N7. HDV audio: a frame cut by lost packets is glued onto the next PES** (`hdv_demux.c` `ts_packet()` / `audio_pes_done()`, `acarry`). Low. Testing required.
  - A CC gap that ends exactly on the first packet of the next PES is recorded at `apes_loss_at == apes.n`, which passes the "nothing lost" test. The truncated frame is carried into the next PES: one garbage MP2 frame, the real first frame lost to the resync, about 48 ms of audio, not counted as an error.
- **N8. Analog: video fill follows the frame counter, audio fill follows device time** (`pinnacle_analog.c` `video_gap_to_fill()` and `on_audio()`, from ebedc07). Medium. Testing required (reproduced in a harness).
  - If the device clock jumps while the 16-bit video counter runs on (what the device does across a signal drop is unknown, section 8), video fills nothing and audio fills the whole time gap. Harness: a +120 s clock jump gives 20 frames against 3020 audio blocks; audio is 120 s ahead of video for the rest of the file. If the audio packet happens to be polled before the video frame, the same event is judged a restart and nothing is filled, so the result depends on poll order.
  - Smaller: the `agrees` window uses a nominal 10 MHz while the device runs at about 10.001 MHz, so a real loss of about 13,800 frames or more is judged a restart; after a host stall up to about 5 frames can be processed before audio is polled, and a real loss of 250+ frames can then be judged a restart.
  - Fix direction: decide the video fill from device time as well and use the counter as a check, so both streams fill the same span.
- **N9. Analog: short audio packets are not corrected** (`on_audio()`: `samples = (n - 12) / 4`). Low-medium. Testing required (does the device ever send one?).
  - A packet with fewer than 1920 samples is written as is; the deficit rounds to no fill (1000 of 1920: 920 samples missing, no error) or to a whole extra block (900 of 1920: 780 samples too many). Each event shifts audio against video by up to about 20 ms, breaking the "exactly 1920 samples per frame" promise in [analog.md](analog.md). Fix: zero-pad any short packet to a full frame and count it.
- **N11. Analog gap fill can overflow the fatal writer ring** (`analog_video_cb()` -> `pin_writer_push()`, ring `PIN_WRITER_ANALOG_CAPACITY` 512 MiB with `PIN_WRITER_OVERFLOW_FATAL`). Medium. Testing required.
  - `fill_frames()` can add up to 0x7fff stand-ins in one burst, pushed at memcpy speed. After about 640 PAL frames (about 26 s of gap) the next push overflows, and `capture_guard()` ends the capture as DISK_SLOW although the disk is fine; everything after the gap is lost.
  - Fix: pace fillers, cap the fill per call, or let fillers wait for the writer.

### 18.2 Files not finalised, or errors not reported

- **N17. The FFV1 / hwenc 10 s watchdog turns a disk stall into a failed capture** (`ffv1_write_video()` / `ffv1_write_audio()`, hwenc `queue_push()` / `ring_push()`, from 5e2317f). Medium-low. Testing required.
  - The encoder thread muxes straight to the file. A drive stall over 10 s (spin-up, USB reset, network share, antivirus) fills the 8-frame queue in about 0.3 s, and the sink fails with `PIN_ERR_CODEC` although the 64-512 MiB writer ring in front could have absorbed it. Before 5e2317f the wait was unbounded.
  - After the watchdog fires, `ffv1_close()` / `hwenc_close()` still `pthread_join` a thread that may be stuck in a kernel write (dead share, pulled disk); the session then stays STOPPING until the OS returns.
- **N22. Windows forced shutdown: `pin_close` runs in `WM_ENDSESSION` without a time bound** (`SessionEndGuard.WndProc`). Medium-low. Testing required.
  - A slow drive with a full 256 / 512 MiB ring, or an encoder tail, can take longer than the shutdown kill timeout; the process is then killed mid-flush. Window geometry is also not saved on logoff (`SaveWindowGeometry` only in `Closing` / `Closed`).
- **N23. Windows: the final `pin_close` can run late on the preview render thread** (`D3DPreview.Dispose` joins for 500 ms; the render loop holds a `DangerousAddRef` on the session handle). Low. Testing required.
  - If the join times out, the real `pin_close` runs later on the render thread, and the process may exit first. On a device replug the old session and its device lock are released up to 100 ms late, so the reopen can fail as busy.
- **N25. A rewind the deck refuses is taken as done** (`pin_deck.c` `pin_deck_async_poll()`: after 5 `08` NOT_IMPLEMENTED answers, `status = (rl >= 4 && resp[0] != 0x0a) ? DONE : FAILED`). Medium. Testing required (needs a deck that answers 08 to REW; open decision in 14).
  - `dv_tick()` sets `deck_cmd_done_s` without checking the answer; the tape never moved, so the poll reads STOPPED, and 3 s later "rewind finished" sends PLAY: the capture starts where the tape is, or pass 2 starts from the end. Fix: FAILED after exhausted retries, with a message.
- **N26. Deck commands during the rewind between passes hijack the rewind detection** (`pin_session_deck()` rejects only CAPTURING; the rewind-finished test in `dv_tick()` does not know who stopped the deck). Low. Testing required (the GUIs may hide the deck buttons then).
  - A user Stop during the rewind is overridden 3 s later by PLAY for pass 2; PLAY or FF leaves the session in REWINDING with no idle or time-limit check until Stop capture.
- **N29. A partly failed initial URB submit ends the capture at the first wrap** (`pinnacle_stream_read_loop_ex()`). Low-medium. Testing required (usbfs ENOMEM, a small `usbfs_memory_mb`).
  - A failed submit marks only that slot; later slots stay submitted, and in-order delivery ends the capture with a USB error when `head` reaches the failed slot. Only `submitted == 0` is handled. Fix: shrink the queue to the first failing slot. `pinnacle_analog.c` `queue_init()` has a similar fault: a failed submit leaves `done = 0`, and `queue_free()` waits 2 s and leaks the queue.
- **N30. Any non-zero return of `libusb_handle_events_timeout_completed()` ends the capture** (DV read loop and `pinnacle_analog_read_loop()`), LIBUSB_ERROR_INTERRUPTED included. Low. Testing required (a signal on the USB thread). DV reports it as a USB failure; analog skips the drain.
- **N32. `close_capture()` in the CLI can wait 30 minutes, not interruptible** (`pin_session_capture_busy()` stays true while a sink is open after `set_error()` without a capture end). Low. Testing required.

### 18.3 Wrong numbers on screen

- **N33. `lost_blocks` is wrong for holes of 256 or more blocks** (`check_dbc()` sees the DBC modulo 256). Low-medium (it is the only loss figure for HDV). Fix required (reproduced).
  - 256 or more lost packets wrap (300 -> 44); exactly 256 is not seen at all, and two missing frames go uncounted. DV frame counting is not affected (`frames_between()` resolves the wrap from the DIF numbering); HDV has no such check.
- **N38. DV audio error detection reads 16-bit words in 12-bit mode** (`dv_error.c` `dv_error_analyze()`). Low. Testing required.
  - The 12-bit error fill 0x800 (bytes `80 80 00`) is not uniform in 16-bit words, so `audio_err` stays 0 on 32 kHz / 12-bit tapes. A block of 36 identical non-zero words (silence with a DC offset, all 0xFFFF) counts as an error on every frame of the passage.
- **N39. HDV damage can leave a damaged reference marked clean** (`hdv_error.c` propagation). Low-medium. Testing required.
  - An I picture that loses its tail charges the CC gap to the next unit, an open-GOP leading B, which updates no reference state, so the following P and the rest of the GOP read clean. An I picture that loses its PES start never becomes a unit, so one error frame is counted instead of about 15.
- **N41. AV/C PLAY operands 0x65 and 0x6d look swapped** (`pin_deck_state_from_avc()`). Low (status text and script conditions only). Fix required once a camera confirms: reverse play shows Paused, reverse pause shows Playing.

### 18.4 Options, names and stop conditions

- **N45. The name-collision check compares non-ASCII names byte-wise, and its Windows folder listing is untested** (`pin_naming_collides()`). Low. Testing required (Windows, MSYS2).
  - The Windows listing now uses `_wopendir` / `_wreaddir` with UTF-8 conversion; try it in a folder like `Café`. An unreadable folder now makes the check "unknown" (the GUIs ask "Capture anyway?", the CLI refuses without `--overwrite`).
  - The fold is ASCII-only: non-ASCII case (`ÉCRAN` vs `écran`) and NFC / NFD on APFS compare unequal, so those collisions are not seen.
- **N46. Scene detector: a date break outvotes a continuous timecode** (`pin_scene.c` `classify()`, `state_advance()`). Low. Testing required.
  - A frame just after midnight with the new time but no date pack leaves {old date, 00:00:00} as the baseline; the next intact frame is 86400 s away and a spurious split is confirmed.
- **N47. Standard change and Record can race** (`analog_tick()` sets `analog_restart`, unlocks, then `handle_inline_commands()`). Low. Testing required. A capture started in that window restarts at the new standard and continues the same file with a different frame size.

### 18.5 Smaller items

- **I2C write NAK ignored, config replies not matched** (`pinnacle_cfg.c` `pinnacle_i2c_write()`, `cfg_xfer()`): after one reply timeout every later exchange is one reply off. Testing required.
- **`pin_deck_parse_timecode()`** reads drop frame from bit 7 of the frame byte; a camera using the DV convention (bit 6) shows non-drop. Testing required.
- **`hdv_aux_scan_datetime()`** matches 0x62 / 0x63 / 0x13 at any byte offset. It has no caller today; it needs pack framing before it is used for splits.

### 18.6 Checked, nothing found (second pass)

- Fixes 4a49606 (loss counting order, first and last frame at input switch, close and unplug), 5f66c1e and 5e2317f (close and trailer errors in all sinks; except N17), b43f2f6 (held DV frames flushed in `dv_close()`, the provisional file replaced under its own name; except N6), ebedc07 (counter wrap, thresholds, black and repeat fill, padding at stop; except N8, N9 and N11), 6f94979, b712741 (`SessionEndGuard`; except N22).
- Reassembler chunking: 200 random lossy PAL and NTSC streams give identical output fed whole or in 1-byte to 5000-byte pieces. URB buffer reuse, ring order, teardown and URB status handling; `ts_buf` bounds; HDV source-packet alignment.
- Analog: field weave and line offsets across transfers, the delivery ring (the producer waits, nothing dropped), drain at stop, 64-bit device time, the resampler (32 and 44.1 kHz within one sample over 32 M samples).
- Writer ring, `pin_stdout` (short writes, EINTR, EPIPE, SIGPIPE), avi_writer layout, hdv_demux PTS unwrap and PES / PSI bounds, hdvmux PASS timestamps and thread shutdown.
- FFV1 and hwenc: no silent drop when a queue is full, complete flush at close, pts from frame and sample counts with no cumulative rounding, pixel conversion strides.
- Status struct mirrors (C#, Swift, JNI), event queue wrap, text helper buffer sizes and format types, no GUI re-summing counters, 64-bit counters kept 64-bit in all three GUIs.
- Scene detector 24 h wrap (PAL, NTSC, drop frame), holdback and split order, fmtwatch (no flip-flop path), closer job accounting, naming of reserved and long names.
- CLI exit codes: every abnormal stop maps to exit 4, and a failed close becomes WRITE_ERROR before the end is reported. Packaging: firmware in every package, `set -euo pipefail`, version from `VERSION`.
