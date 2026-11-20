
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

### 17.6 Smaller items

- **OHCI `xferStatus` is not checked** (`dv_reassembler.c` `ring_feed()`). Low. Testing required (real EP 0x88 dumps).
  - A packet's payload is used as it streams in, before the trailer with its `xferStatus` arrives, so a packet that completed with an error event is taken as data. Test: on DV and HDV dumps from both devices (including one with dropouts), count the trailers whose status is not `0x8411`. If there are any, hold each packet's payload until its trailer (and count a refused one as a DBC hole); if there are none, delete this item.
- **N11 (rest). Analog: a long gap still has to be written** (`pin_writer_push_repeat()`, `sink_avi` / `sink_ffv1` / `sink_hwenc`). Low. Testing required (slow drives).
  - The stand-ins no longer take room in the writer queue, but the sink writes each one, and the frames behind them wait: a gap of N seconds needs about N x 21 MB/s / (drive or encoder rate) to drain, and more than the queue's ~25 s of it still ends the capture as DISK_SLOW. Test: on a USB 2 stick or SD card, unplug the S-Video cable for 30 s during an uncompressed AVI and an FFV1 capture (the audio runs on only if the device keeps sending it; otherwise there is no fill) and check that it goes on.
- **AV/C TIME CODE drop-frame bit** (`pin_deck_parse_timecode()`). Low. Testing required (an NTSC drop-frame tape).
  - Read from bit 6 of the frame byte, as SMPTE 12M and the DV TC pack (IEC 61834) have it (bit 7 is the colour-frame flag); the docs have no AV/C byte layout. Test: play an NTSC drop-frame tape and check that the TIME CODE reply's frame byte has bit 6 set and the display shows `;`.
- **EP 0x84 failure retry** (`ep84_retry()`). Low. Testing required (device).
  - A failed EP 0x84 transfer is resubmitted from the read loop every 100 ms (after `libusb_clear_halt` on a stall), up to 20 failures in a row. Test: stall EP 0x84 during a DV capture (unplug the camera's FireWire cable for a moment) and check that the stall clears and PLAY / STOP / TIME CODE keep working.
- **Device locks (Windows).** Low. Testing required (Windows, MSYS2).
  - The lock is a named event in `Global\` (falls back to `Local\` if refused), thread-independent, with the status record in a `Local\` mapping. Test: build with MSYS2, run `test_lock` (cross-thread case), and check that a second logged-on user opening the device gets BUSY.
  - 2026-11-19: `pinnacle_lock` (test_lock) passes in an MSYS2 UCRT64 build; a second process of the same user gets "device is in use by another process" (exit 2) and the device table shows `in use (pid N)`. Left: a second logged-on user.

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

### 18.6 Checked, nothing found (second pass)

- Fixes 4a49606 (loss counting order, first and last frame at input switch, close and unplug), 5f66c1e and 5e2317f (close and trailer errors in all sinks), b43f2f6 (held DV frames flushed in `dv_close()`, the provisional file replaced under its own name), ebedc07 (counter wrap, thresholds, black and repeat fill, padding at stop), 6f94979, b712741 (`SessionEndGuard`; except N22).
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

- **Z1. Windows: UTF-8 and long output paths, not built or run on Windows.** Testing required (Windows, MSYS2).
  - All file access in the core now goes through `src/core/pin_fs.c` (UTF-16 calls, `\\?\` / `\\?\UNC\` prefix for full paths of 240 or more characters), the CLI takes its arguments from `GetCommandLineW()`, and the collision check lists with `FindFirstFileExW` and folds non-ASCII case. Tested on macOS only. Build with MSYS2, run `test_pin_fs`, `test_pin_naming`, `test_lazy_sink`, and capture (GUI and CLI) into a `Café` folder and to a path over 260 characters, with the "UTF-8 for worldwide language support" switch on and off and with `LongPathsEnabled` off. Also new and untested on Windows: read-only opens share `FILE_SHARE_DELETE` (`pin_fs_fopen`, so the settings rename over a file being read works), and `pin_exe_dir()` / `lib_dir()` use a growing wide buffer (run from a folder over 260 characters). Run `test_pin_settings` (concurrent updates) there too.
  - 2026-11-19 (Windows 10 x64, MSYS2 UCRT64): `test_pin_fs`, `test_pin_naming`, `test_lazy_sink`, `test_pin_settings` pass. CLI replay captures (DV and DV AVI) into `Café ü 日本\café.dv` and to 273/274-character paths are written complete. Run with `LongPathsEnabled` 1 and the UTF-8 code page on only; left: those switched off, the GUI, `FILE_SHARE_DELETE`, running from a folder over 260 characters.

### 19.2 Files not finalised, errors not reported, sessions stuck

- **E7. Android output volume.** Low. Testing required (Android 11-15). The write test now creates every capture extension (`.dv`, `.m2t`, `.avi`, `.mov`, `.mkv`, `.nut`) and a capture is refused when the chosen SD card or USB drive is gone; what is left to test on devices is whether MediaProvider accepts all of them in `Movies/MarvinCapture` and that a file left by a previous install (not writable by the new one) is handled.
- **T4. `pin_cond.h` timed wait on winpthreads.** Testing required (Windows). The wait now resumes until the monotonic clock says the time is up (Windows uses a wall-clock deadline per slice, no `pthread_condattr_setclock`), so an early ETIMEDOUT cannot fire the FFV1 / hwenc watchdog. Run `test_pin_cond` in an MSYS2 build.
  - 2026-11-19: `pin_cond` passes in an MSYS2 UCRT64 build on Windows 10. Done.
- **U1 / U2. Owed AV/C write responses while capturing.** Testing required (a camera that answers with FCP writes acked pending). They are now sent from `p1394_async_service()` on every read-loop tick without blocking or reading EP 0x84, before a queued AV/C command; the command's ack is checked (ack_busy resent, others fail the command); a full owed table is logged and counted (`owed_dropped`).
- **T2 / U4, config replies: left to check on hardware.** Low. A failed receive start now stops the stream and disconnects; the disconnect is tried even when the receive stop failed; a lost compare-swap response is followed by a read of the oPCR to learn whether the swap happened. Config replies must echo the request (late ones are read past), short 05/06 / `cfg_op` replies are errors, and an I2C write that is not acknowledged (`01 01 08`) now fails: check that analog bring-up sees no NAKs on the 500-USB and 510-USB.
  - 2026-11-19: analog bring-up (composite and S-video) on both units: no NAK, no config-reply error. **But the stricter match breaks the GUID read**, see G1 below.
- **G1. The `80` (GUID) reply never matches.** **Done 2026-11-20.** Medium. Found on hardware 2026-11-19 (500-USB and 510-USB). `pinnacle_cfg_reply_matches()` wants the reply to `80 03 08` to repeat `03 08`; the units answer `80 03 01 00` + 8 GUID bytes + 8 more bytes (20 bytes). Every GUID read is read past and times out: serial `-` in the device table, warm start never taken (every run reloads the FPGA), ~6.9 s lost in "Checking what the FPGA holds", and every unit publishes the same fallback GUID in its config ROM (two equal GUIDs on one bus with two Marvins). Matching only `reply[1] == req[1]` (and n >= 12) was tried on both units and fixes all of it; the test for the matcher needs the real reply as a case. Done that way, with the real reply as a test case (`tests/test_p1394.c`).
- **G2. `scripts\build.ps1` stops on Windows.** **Done 2026-11-20.** Low. `tests/engine/test_pin_closer.c` calls `setenv` / `unsetenv`, undeclared under MSYS2 UCRT64 (`-Wimplicit-function-declaration` is an error there). Use `_putenv_s` on Windows (or a small helper).
- **G3. Playout: no cycle master when the other node is root.** **Done 2026-11-20.** High. Found on hardware 2026-11-20. `pout_link_open()` keeps whatever root the bus elected; when that is an idle Marvin (or any root that is not cycle master) no cycle starts are sent, IT0 never runs and EP 0x06 stalls after ~1 KB. Fixed (checked on hardware): after link init, if NodeID bit 30 is clear, PHY register 1 \|= `0xc0` (RHB \| IBR), re-read the topology. [playout.md](playout.md#second-session-2026-11-20-playout-works) item 1.
- **G4. Playout: the s bit stalls the stream.** **Done 2026-11-20.** High. `pout_it_init()` sets `status_every` 64; any s bit on an OUTPUT_LAST stops EP 0x06 (11776 bytes taken, every run). The vendor sets s only on the last packet of a client buffer that wants completion. Fixed (checked on hardware): `status_every` 0. The unit test that expects the s bit every 64 packets changes with it.
- **G5. Playout: EP 0x84 not drained while the IT queue is full.** **Done 2026-11-20.** High. `take_free_locked()` (`pout_hw_link.c`) sleeps; a bus reset during the stream then wedges EP 0x06 and EP 0x02 until the next bring-up. Fixed (checked on hardware): read EP 0x84 (`p1394_pump(&k->l, 1)`) in that wait; the queued requests are answered by the next `pout_link_pump()`. A reader thread or async EP 0x84 transfers (as the vendor keeps) would be cleaner.
- **G6. Playout: bus resets seen too late; a fresh plug connection is wiped.** **Done 2026-11-20.** High. `pout_link_bus_generation()` uses SelfIDCount, which lagged the reset by ~1.5 s; the controller reconnects within ms of its reset, so `check_bus_reset()` runs after the new lock and `pout_link_rescan()` clears it (virtual deck: PLAY, 0 frames sent). Fixed (checked on hardware): IntEvent bit 17 (`0x080`, clear by `0x084` = `0x20000`), checked before dispatching each batch of incoming requests, the later session-level rescan keeping the CSR state for a reset already handled.
- **G7. Playout: hangs after an EP 0x06 failure.** **Done 2026-11-20.** Medium. In script mode MarvinPlayout did not exit after "EP 0x06 write failed" (killed by `timeout` after 40–60 s) instead of ending with exit 2. Cause: `pout_link_it_stop()` polled ContextControl 20 times at 2 s per timed-out EP 0x02 transfer; now it stops at the first failed transfer, waits longer than one write timeout so a failing last write is seen, and `pout_hw_run()` turns such a failure into exit 2.
- **G8. Playout or capture: the last tape frame is missing.** Accepted. Low. Virtual-deck captures had 499 of 500 (PAL) and 598 of 599 (NTSC) frames; all others identical. Not yet known which side drops it.
- **G9. Two ctests failed on Windows.** **Done 2026-11-20.** Low. `test_pin_platform_api` set `MARVIN_UPDATE_URL=` to turn the update check off, but the Windows C runtime removes a variable set empty, so the default address applied; those two checks are now POSIX only (turning the check off by an empty variable is not possible on Windows). `test_replay_status_numbers` expected no free-space estimate for a folder that does not exist, but `GetVolumePathNameW` answers for such a path; `fat32_and_free()` now returns -1 for a missing path on Windows too, as `statvfs` does. All 89 ctests pass under MSYS2 UCRT64.

### 19.3 Wrong numbers on screen

- **V4. HDV continuity in the demux aliases at 16 lost packets.** Low. Testing required (real HDV dumps). The CC test misses a loss of exactly 16k packets of a PID (`hdv_demux.c` `ts_packet()`, `dv_reassembler.c` `ts_handle_packet()`, `hdv_error.c` the same way). The session's own count sees such a hole from the DBC (the picture it fell into is a frame with error), so raw HDV is covered; into HDV MOV / MKV the file counts its frames itself and passes the damaged picture through unrepaired, because the reassembler's DBC hole does not reach the sink's demux (it sits behind the writer queue; it would need a loss marker unit carried through the hold-back FIFO, the writer and `sink_hdvmux`). Test: in HDV dumps with dropouts from both devices, count the DBC holes after which no PID shows a CC gap. If there are any, add that marker; if there are none, delete this item.

### 19.5 Checked and refuted in this pass

- **D4 (macOS: failed finalise at "Stop and quit" is lost): refuted.** `capture_report_end()` (2371-2404) drains the closer first and turns a failed close into `PIN_STOP_WRITE_ERROR` (2402-2403), which is abnormal; the CAPTURE_ENDED event carries it. macOS sets `quitEndError` (WindowModel 1312-1320) and stays open; Windows does the same (`MainWindow.xaml.cs`). D5 (banner overwritten after a failed finalise) follows from the same path: the abnormal CAPTURE_ENDED raises the alert, so only the live banner is short-lived.
- Android: a failed finalise of the last file is not lost (WRITE_ERROR, shown by `reportProblem`).

### 19.6 Checked, nothing found (third pass)

- USB: in-order delivery and head indexing with a partial queue, slot accounting, bitstream chunk sizes (78422), FX2 block casts, POSIX lock inode recheck, `pinnacle_enum_build_id`, thread boost order.
- 1394 / analog: config ROM layout and CRC lengths, packet and batch bounds, AT descriptors, tlabel rotation, FCP ring, analog deliver ring, weave offsets, the stand-in fill, drain after the last seq, gain re-application.
- Reassembly: `frame_buf` reuse (all consumers copy), NTSC / PAL sizing and the 16-sequence bound, `frames_between` modular fit, `dif_feed` arithmetic, `ts_feed` alignment, `es_feed` cut and PTS assignment, 33-bit PTS unwrap, hdv_decode flush order, hdv_aux bounds.
- Engine: hold-back push / grow / pop and the OOM path, split counters, closer accounting, `pin_freeprobe` buffers, `dv_error.c` against the skill, `hdv_error.c`, audio helpers (monitor only), `capture_unit` size-cut arithmetic, `reset_frame_counters()`, event queue gap closing, `pin_session_close` ordering, `status_copy_out`, monitor ring.
- Sinks: writer ring wrap and scratch copy, `byte_tail` after consume, avi_writer patch offsets, idx1 / ix offsets, segment rollover, padding, NUT trailer error, `sink_lazy` ownership, `pin_naming` build and validate, hdvmux PASS DTS / PTS after `gop_drop_lead`, `span_close` slots, `frames_held` accounting, shutdown order, hwenc trailer order, audio ring math, `hwenc_convert` strides (widths up to 2048), ffv1 close order.
- API / CLI: all status-format buffer widths and 64-bit counters, `pin_format_opts` sizes, event queue wrap, `PIN_EVT_DONE` ordering.
- GUIs: Windows struct and enum mirrors against `pin_api.h` field by field; macOS has no mirror (it imports the header); Android index tables (ST_, SL_, SD_, SS_, OPT_), 64-bit counters, UTF-8 conversion, local references, fd ownership (Java keeps the connection until `pin_close` returns), synchronous audio / preview detach before close.

### 19.7 Suggested order for fixes and tests

1. **Windows build (MSYS2):** Z1 (UTF-8 and long paths, a `Café` folder), T4 (`test_pin_cond`), device locks (`test_lock`, a second user).
2. **Device and camera:** U1 / U2 (owed AV/C write responses), T2 / U4 (config replies, I2C NAKs on the 500-USB and 510-USB), EP 0x84 failure retry, the TIME CODE drop-frame bit (an NTSC drop-frame tape), N11 (a 30 s analog dropout onto a slow drive).
3. **Real EP 0x88 dumps:** OHCI `xferStatus`, V4 (DBC holes no CC check sees).
4. **Android 11-15 devices:** E7.
