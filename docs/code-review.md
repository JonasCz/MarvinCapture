
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

- **OHCI `xferStatus` is not checked** (`iso_header_ok()`). Low. Testing required.
  - Packets that completed with an error event are accepted as data. Count them on real dumps (normal is `0x8411`).
- **EP 0x84 failure retry** (`ep84_retry()`). Low. Testing required.
  - A failed EP 0x84 transfer is now resubmitted from the read loop every 100 ms (after `libusb_clear_halt` on a stall), up to 20 failures in a row. Not seen on hardware: check that a stall clears and deck control keeps working.
- **Device locks (Windows).** Low. Testing required.
  - The lock is now a named event in `Global\` (falls back to `Local\` if refused), thread-independent, with the status record in a `Local\` mapping. Not built or run on Windows here: build with MSYS2 and run `test_lock` (cross-thread case), and check that a second logged-on user gets BUSY.

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

- **N22. Windows forced shutdown: `pin_close` runs in `WM_ENDSESSION` without a time bound** (`SessionEndGuard.WndProc`). Medium-low. Testing required.
  - A slow drive with a full 256 / 512 MiB ring, or an encoder tail, can take longer than the shutdown kill timeout; the process is then killed mid-flush. Window geometry is also not saved on logoff (`SaveWindowGeometry` only in `Closing` / `Closed`).
- **N23. Windows: the final `pin_close` can run late on the preview render thread** (`D3DPreview.Dispose` joins for 500 ms; the render loop holds a `DangerousAddRef` on the session handle). Low. Testing required.
  - If the join times out, the real `pin_close` runs later on the render thread, and the process may exit first. On a device replug the old session and its device lock are released up to 100 ms late, so the reopen can fail as busy.

### 18.3 Wrong numbers on screen

- **N38. DV audio error detection reads 16-bit words in 12-bit mode** (`dv_error.c` `dv_error_analyze()`). Low. Testing required.
  - The 12-bit error fill 0x800 (bytes `80 80 00`) is not uniform in 16-bit words, so `audio_err` stays 0 on 32 kHz / 12-bit tapes. A block of 36 identical non-zero words (silence with a DC offset, all 0xFFFF) counts as an error on every frame of the passage.
- **N39. HDV damage can leave a damaged reference marked clean** (`hdv_error.c` propagation). Low-medium. Testing required.
  - An I picture that loses its tail charges the CC gap to the next unit, an open-GOP leading B, which updates no reference state, so the following P and the rest of the GOP read clean. An I picture that loses its PES start never becomes a unit, so one error frame is counted instead of about 15.
- **N41. AV/C PLAY operands 0x65 and 0x6d look swapped** (`pin_deck_state_from_avc()`). Low (status text and script conditions only). Fix required once a camera confirms: reverse play shows Paused, reverse pause shows Playing.

### 18.4 Options, names and stop conditions

- **N46. Scene detector: a date break outvotes a continuous timecode** (`pin_scene.c` `classify()`, `state_advance()`). Low. Testing required.
  - A frame just after midnight with the new time but no date pack leaves {old date, 00:00:00} as the baseline; the next intact frame is 86400 s away and a spurious split is confirmed.

### 18.5 Smaller items

- **`pin_deck_parse_timecode()`** reads drop frame from bit 7 of the frame byte; a camera using the DV convention (bit 6) shows non-drop. Testing required.
- **`hdv_aux_scan_datetime()`** matches 0x62 / 0x63 / 0x13 at any byte offset. It has no caller today; it needs pack framing before it is used for splits.

### 18.6 Checked, nothing found (second pass)

- Fixes 4a49606 (loss counting order, first and last frame at input switch, close and unplug), 5f66c1e and 5e2317f (close and trailer errors in all sinks), b43f2f6 (held DV frames flushed in `dv_close()`, the provisional file replaced under its own name), ebedc07 (counter wrap, thresholds, black and repeat fill, padding at stop; except N8, N9 and N11), 6f94979, b712741 (`SessionEndGuard`; except N22).
- Reassembler chunking: 200 random lossy PAL and NTSC streams give identical output fed whole or in 1-byte to 5000-byte pieces. URB buffer reuse, ring order, teardown and URB status handling; `ts_buf` bounds; HDV source-packet alignment.
- Analog: field weave and line offsets across transfers, the delivery ring (the producer waits, nothing dropped), drain at stop, 64-bit device time, the resampler (32 and 44.1 kHz within one sample over 32 M samples).
- Writer ring, `pin_stdout` (short writes, EINTR, EPIPE, SIGPIPE), avi_writer layout, hdv_demux PTS unwrap and PES / PSI bounds, hdvmux PASS timestamps and thread shutdown.
- FFV1 and hwenc: no silent drop when a queue is full, complete flush at close, pts from frame and sample counts with no cumulative rounding, pixel conversion strides.
- Status struct mirrors (C#, Swift, JNI), event queue wrap, text helper buffer sizes and format types, no GUI re-summing counters, 64-bit counters kept 64-bit in all three GUIs.
- Scene detector 24 h wrap (PAL, NTSC, drop frame), holdback and split order, fmtwatch (no flip-flop path), closer job accounting, naming of reserved and long names.
- CLI exit codes: every abnormal stop maps to exit 4, and a failed close becomes WRITE_ERROR before the end is reported. Packaging: firmware in every package, `set -euo pipefail`, version from `VERSION`.

## 19. Code review, third pass: data loss and wrong counts

A third reading of the code (November 2026), by section: USB and device, 1394 and analog, reassembly and demux, error stats and cuts, the session engine (two halves), writer and DV sinks, HDV / FFV1 / hwenc sinks, API and CLI, and the three GUI layers. MarvinCapture only (not MarvinPlayout). Same marks as sections 17 and 18: **Fix required** or **Testing required**, severity high, medium or low. Nothing was run on hardware. "Reproduced" means an Opus pass confirmed it in a scratch harness that feeds synthetic data to the core; everything else comes from reading the code. Items marked "verified" were traced a second time by a separate reviewer; the verdict is given where it differs from the first reading. Item IDs are new in this section (T, U, V, W, X, Y, Z, A, B, C, D, E, O) and do not clash with R and N.

### 19.1 Frames lost, mangled or put in the wrong file

- **E1. Android: the Library can be opened and used for deleting during a capture.** High if hit (needs a deliberate user action). Fix required (verified).
  - `MainActivity.applyLibraryExtra()` 251-255, reached from `onCreate` 139 and `onNewIntent` 174 through the launcher shortcut, has no `isCapturing()` guard (`applyInputExtra` 245 has one; only `libraryBtn` is disabled, 697). `Storage.captures()` lists the file being written. `LibraryView.confirmDelete` 248-259 runs `f.delete()`: the core keeps writing to an unlinked file and the whole capture is lost.
  - Fix: ignore the extra while capturing, and have the library skip the file named in `SS_CURRENT_FILE`.
- **Z1. Windows: UTF-8 and long output paths, not built or run on Windows.** Testing required (Windows, MSYS2).
  - All file access in the core now goes through `src/core/pin_fs.c` (UTF-16 calls, `\\?\` / `\\?\UNC\` prefix for full paths of 240 or more characters), the CLI takes its arguments from `GetCommandLineW()`, and the collision check lists with `FindFirstFileExW` and folds non-ASCII case. Tested on macOS only. Build with MSYS2, run `test_pin_fs`, `test_pin_naming`, `test_lazy_sink`, and capture (GUI and CLI) into a `Café` folder and to a path over 260 characters, with the "UTF-8 for worldwide language support" switch on and off and with `LongPathsEnabled` off. Also new and untested on Windows: read-only opens share `FILE_SHARE_DELETE` (`pin_fs_fopen`, so the settings rename over a file being read works), and `pin_exe_dir()` / `lib_dir()` use a growing wide buffer (run from a folder over 260 characters). Run `test_pin_settings` (concurrent updates) there too.

### 19.2 Files not finalised, errors not reported, sessions stuck

- **E3. Android: events from `pin_close` are never polled.** Low (verified, narrowed). Fix required. `CaptureService.closeSession()` sets `session = 0` before `Core.close` (339, 352), and the CAPTURE_ENDED / FILE_CLOSED events pushed during `pin_close` are lost with the session; no alert. Reached on the 15 s detach timeout (505), `onDestroy` (188) and the error card (385); normally the core reports DEVICE_LOST first. Files are still finalised. Swapping the order is not the fix (polling during close would be a use after free): stop the capture and wait for CAPTURE_ENDED before closing, or let close return the end status.
- **E2. Android: the "Capture finished" notification can be dropped** (`CaptureService.tick()` 514-525, `flushPendingFinished()` 561, `onDestroy()` 186). With the app swiped away the flush waits one tick, `leaveForeground` stops the unbound service, and `onDestroy` removes the handler messages. Low-medium. Fix required. Files are fine.
- **E5. Android: a service destroyed during a pending open leaks the session and the USB connection** (`onDestroy()` 185-199, `open()` 280-300). The device lock is held until the process dies; a reopen in the same process fails as busy. `onDestroy` can also block the main thread up to 8 s. Low. Fix required.
- **O2. Windows quits silently when a script-driven capture's finalise failed** (`MainWindow.xaml.cs` 856-859: `_quitEndError` is not set while `_scriptRunning`). Low. Fix required. macOS stays open and shows the alert.
- **D7. The macOS GUI has no SIGINT / SIGTERM / SIGHUP handler** (only SIGPIPE, `ConsoleOutput.swift:31`). Medium-low. Fix required. Started from Terminal with script arguments, Ctrl+C or `kill` ends the process with no `pin_close`: AVI and MOV outputs stay unfinalised. The CLI got Ctrl+C handling in 4d97c72; the GUI process did not.
- **D6. macOS `CloseFlow` trusts the lagging `model.isCapturing`** (`CloseFlow.swift` 41, 53; `captureStartPending` is ignored; a minimised window lags up to 1 s). Low-medium. Testing required. Cmd-Q right after a start returns `.terminateNow` without confirmation and `pin_close` runs on the main thread (freeze during an HDV remux). Files should still be finalised.
- **D8. macOS: the CAPTURE_ENDED alert exists only as an event** (`afterEvent` 1310, `closeSession` 843). Low. Testing required. Events are drained only while a session exists; the status `stop_reason` is unused, so a device-list-triggered close can drop an undrained abnormal end.
- **C1. Windows: Done event closes the window inside the event drain loop** (`MainViewModel.Tick` 1058-1061, `VM_EngineEvent` 831-841). Low. Testing required. If `Closed` is raised synchronously `Session` is null in the next `PollEvent`; events queued behind Done are lost and the exit code may change.
- **C4 / D3. A session ERROR delivered by event while `IsCapturing` is still true skips the dead-session close** (`MainViewModel.HandleEvent` 1265, `VM_PropertyChanged` 809-815; macOS `WindowModel` 1282-1286, guard 891, flag 1208). Low. Testing required / Fix required. The dead session stays open until a device-list change. Android has the same shape: `stream_failed` leaves the session in `STATE_ERROR` holding the device until a tap on the no-video card.
- **E4. Android: a launcher input shortcut at cold start changes the saved input while a capture runs** (`applyInputExtra()` 241-248; the guard needs `svc != null`, which is not set before binding). Low. Fix required. The live session stays DV while Settings says analog; the aspect and the next file name follow the wrong kind.
- **E6. Android: `ST_IS_CAPTURING` stays set after `closeSession()`** (:348 resets only `ST_STATE`). Low. Fix required. A reopen before the next status read skips `pushOutputHint`, and the record button acts as Stop.
- **E7. Android output volume.** Low. Testing required (Android 11-15). The write test creates only `.write-test.mkv` in `Movies/MarvinCapture` (`Storage.canCreateIn()` 120-131), while real captures use `.dv`, `.m2t`, `.avi`, `.mov`; MediaProvider may refuse some. `current()` falls back to phone storage silently if the chosen SD card or USB drive is unmounted. A file left by a previous install is not writable by the new one.
- **T4. `pin_cond.h` timed wait on winpthreads.** Testing required (Windows). The wait now resumes until the monotonic clock says the time is up (Windows uses a wall-clock deadline per slice, no `pthread_condattr_setclock`), so an early ETIMEDOUT cannot fire the FFV1 / hwenc watchdog. Run `test_pin_cond` in an MSYS2 build.
- **U1 / U2. Owed AV/C write responses while capturing.** Testing required (a camera that answers with FCP writes acked pending). They are now sent from `p1394_async_service()` on every read-loop tick without blocking or reading EP 0x84, before a queued AV/C command; the command's ack is checked (ack_busy resent, others fail the command); a full owed table is logged and counted (`owed_dropped`).
- **T2 / U4, config replies: left to check on hardware.** Low. A failed receive start now stops the stream and disconnects; the disconnect is tried even when the receive stop failed; a lost compare-swap response is followed by a read of the oPCR to learn whether the swap happened. Config replies must echo the request (late ones are read past), short 05/06 / `cfg_op` replies are errors, and an I2C write that is not acknowledged (`01 01 08`) now fails: check that analog bring-up sees no NAKs on the 500-USB and 510-USB.
- **Analog open question** (low, Testing required): a truncated analog video frame ending on an exact 512-byte multiple (`on_video` `wire % 512`).

### 19.3 Wrong numbers on screen

- **V4. HDV continuity in the demux aliases at 16 lost packets.** Low. Testing required. The CC test misses a loss of exactly 16k packets of a PID (`hdv_demux.c` `ts_packet()`, `dv_reassembler.c` `ts_handle_packet()`, `hdv_error.c` the same way). The session's own count sees such a hole from the DBC (the picture it fell into is a frame with error), so raw HDV is covered; into HDV MOV / MKV the file counts its frames itself, and the reassembler's DBC hole is not passed to its demux: the demux sits behind the writer ring in the sink, so that needs a side channel in the unit stream. Left to judge on hardware how often a hole of a multiple of 16 packets of one PID happens.
- **C3 / D2. Windows and macOS cut a path longer than 1023 bytes silently** (`Utf8Fixed.Set`, `setCString`), so the capture and the overwrite prompt use a different path than the one shown. Android rejects it with `ERR_ARG`.
- **C5. Windows: `AudioPeakText` comes from one status read** (`ApplyStatus` 1108 vs `UpdateMeters` 1011-1036); not present on macOS. Low. Fix required.
- **D9. macOS: a corrupt per-device format setting traps at startup** (`WindowModel.loadSettings()` 1473, `pin_format_t(rawValue: UInt32(f))`; 1461 uses `UInt32(clamping:)`). Low. Fix required.

### 19.5 Checked and refuted in this pass

- **D4 (macOS: failed finalise at "Stop and quit" is lost): refuted.** `capture_report_end()` (2371-2404) drains the closer first and turns a failed close into `PIN_STOP_WRITE_ERROR` (2402-2403), which is abnormal; the CAPTURE_ENDED event carries it. macOS sets `quitEndError` (WindowModel 1312-1320) and stays open; Windows does the same (`MainWindow.xaml.cs` 855-859), apart from O2. D5 (banner overwritten after a failed finalise) follows from the same path: the abnormal CAPTURE_ENDED raises the alert, so only the live banner is short-lived.
- Android: a failed finalise of the last file is not lost (WRITE_ERROR, shown by `reportProblem`).

### 19.6 Checked, nothing found (third pass)

- USB: in-order delivery and head indexing with a partial queue, slot accounting, bitstream chunk sizes (78422), FX2 block casts, POSIX lock inode recheck, `pinnacle_enum_build_id`, thread boost order.
- 1394 / analog: config ROM layout and CRC lengths, packet and batch bounds, AT descriptors, tlabel rotation, FCP ring, analog deliver ring, weave offsets, `fill_frames` loop, drain after the last seq, gain re-application.
- Reassembly: `frame_buf` reuse (all consumers copy), NTSC / PAL sizing and the 16-sequence bound, `frames_between` modular fit, `dif_feed` arithmetic, `ts_feed` alignment, `es_feed` cut and PTS assignment, 33-bit PTS unwrap, hdv_decode flush order, hdv_aux bounds.
- Engine: hold-back push / grow / pop and the OOM path, split counters, closer accounting, `pin_freeprobe` buffers, `dv_error.c` against the skill, `hdv_error.c` apart from N39, audio helpers (monitor only), `capture_unit` size-cut arithmetic, `reset_frame_counters()`, event queue gap closing, `pin_session_close` ordering, `status_copy_out`, monitor ring.
- Sinks: writer ring wrap and scratch copy, `byte_tail` after consume, avi_writer patch offsets, idx1 / ix offsets, segment rollover, padding, NUT trailer error, `sink_lazy` ownership, `pin_naming` build and validate, hdvmux PASS DTS / PTS after `gop_drop_lead`, `span_close` slots, `frames_held` accounting, shutdown order, hwenc trailer order, audio ring math, `hwenc_convert` strides (widths up to 2048), ffv1 close order.
- API / CLI: all status-format buffer widths and 64-bit counters, `pin_format_opts` sizes, event queue wrap, `PIN_EVT_DONE` ordering.
- GUIs: Windows struct and enum mirrors against `pin_api.h` field by field; macOS has no mirror (it imports the header); Android index tables (ST_, SL_, SD_, SS_, OPT_), 64-bit counters, UTF-8 conversion, local references, fd ownership (Java keeps the connection until `pin_close` returns), synchronous audio / preview detach before close.

### 19.7 Suggested order for fixes and tests

1. **Fix required, data in the file:** E1.
2. **Fix required, silent loss or missing report:** D7, E3, E2, O2.
4. **Hardware or OS first:** Z1 (Windows, test in a `Café` folder), T4, U1 / U2, V4.
