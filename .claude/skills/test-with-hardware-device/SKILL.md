---
name: test-with-hardware-device
description: Test the Pinnacle 500-USB driver against the real attached device (and deck/camera) with MarvinCaptureCLI and its --debug log; diagnose "USB error" / device-not-ready. Manual hardware testing, NOT the automated ctest suite.
---

# Testing with the real hardware

This is **manual testing against the physical device**. It is separate from the
automated code tests (`ctest`, replay tests), which need no device; see the
`build-and-test` skill for those and for building. Build first
(`scripts\build.ps1`), then work from `build\dist`.

## Rules of the road

- **One owner at a time.** The GUI and the CLI cannot hold the device together.
  The device table (`MarvinCaptureCLI` without arguments) shows `in use (pid N)`;
  ask the user to close the GUI, don't kill it.
- **Don't rebuild `build\dist` while the GUI is open**, and don't run heavy jobs
  during a latency-sensitive capture.
- **Replug rule:** if the device ends up needing a physical replug / power
  cycle, stop and ask the user. A wedged command channel usually recovers on the
  next full bring-up (bitstream reload); try one normal run first.
- Don't hardcode unit data (GUID etc.); other Marvin models are expected later.
- Needs the WinUSB driver bound to the device (`docs/windows-driver.md`).
- Bring-up takes ~5.5 s before data flows; a window shorter than that looks empty.
- A camera/deck in tape mode with the tape stopped sends zero bytes, which looks
  like a hardware fault. Put it in play (or camera mode) first.
- Leave the deck stopped and the tape rewound when you are done
  (`--rew --wait`).

## The tool: MarvinCaptureCLI (run from `build\dist`)

It is the only shipped command-line tool (the old pinlist, pincli, pinanalog,
pindeck are gone). It finds `firmware\` next to `marvin-core.dll`; the exe needs
`libwinpthread-1.dll` and `libusb-1.0.dll` next to it (the build script copies
them; exit code `-1073741511` (0xC0000139) = a wrong `libwinpthread` was found
on PATH, rebuild with the script).

```
MarvinCaptureCLI                         # help + device table (id, name, vid:pid, serial, state)
MarvinCaptureCLI --rew --wait            # deck control, left to right
MarvinCaptureCLI --rew --wait --play --capture tape.dv --wait idle,nosignal --rew --wait
MarvinCaptureCLI -d <id|serial|file> -i svideo --capture a.avi --wait +00:00:10:00
MarvinCaptureCLI -i composite --wait +00:00:05:00     # analog bring-up + signal check only
```

Exit codes: 0 ok, 1 usage, 2 device, 3 deck, 4 capture ended abnormally, 130
Ctrl-C. Human output goes to stderr (redirect with `2> log.txt`), stdout is
reserved for `--capture -`. Syntax and semantics: `docs/cli.md`.

Switching between DV and analog is just another invocation (the next run's
bring-up reloads the other FPGA bitstream); no replug needed.

## Debug logging: `--debug`

`--debug` prints every core log level (prefixed `debug:`/`info:`/`warning:`/
`error:`) and the status as one plain line per second. There are no debug
environment variables any more. What it shows:

```
debug: pinnacle: opened Pinnacle Studio 510-USB (2304:0223, usb:2-1), interface 0 claimed
debug: pinnacle: step "Uploading FPGA firmware" (the previous step took 2836 ms)
debug: pinnacle: FPGA bitstream uploaded in 1006 ms
debug: p1394: NodeID 0x8000ffc0 SelfIDCount 0x00010014 -> node 0 of 2
info: pinnacle: camera is node 1 (oMPR 0x3fff0003); connected to oPCR[0] 0x013f007a, channel 63
debug: AV/C -> 00 20 c4 65                      (command frame sent to the camera)
debug: AV/C <- ACCEPTED 09 20 c4 65             (response: ctype name, then the frame)
```

Raw AV/C probing of arbitrary frames or OHCI registers (the old
`pindeck raw:`/`reg:`) is no longer possible from a shipped tool, but the tool
is in git history: `git worktree add ../old febfca6~1`, build the `pindeck`
target there (cmake as in docs/building.md; also pincli, pinanalog, pinlist;
pinctl is in `3614da4~1`), details in docs/usage.md (Diagnostics); the log shows
the traffic of the normal commands (the 1 Hz state/timecode poll appears as two
lines per second). The register probe, raw EP 0x88 dump, per-completion and
EP 0x84 logging live on as `#if 0` blocks in `src/core/pinnacle_stream.c` and
`src/core/pinnacle_1394.c` (each says how to re-enable it; rebuild with the
script). Test hooks that remain: `PIN_REPLAY=<file>` (virtual replay device, no
hardware).

The GUI takes `--debug` too: start `build\dist\MarvinCaptureGUI.exe --debug`
from a console, or with stdout redirected to a file, to read its log.

## Smoke test (what to run after changing the core)

1. `MarvinCaptureCLI` : help and the device table (state `ready`).
2. `MarvinCaptureCLI --debug --rew --wait 2> d.log` : the log has the bring-up
   steps, `AV/C ->` / `AV/C <-` lines and one status line per second.
3. `MarvinCaptureCLI --debug -i composite --wait +00:00:05:00` : analog bring-up,
   "no signal" when nothing is attached, exit 0.
4. `MarvinCaptureCLI --rew --wait` again : the switch back to DV works.

## Diagnosing "USB error" / device not ready

The engine error text names the failing step (`stream_fail` in
`src/core/pinnacle_stream.c`, step markers in `src/core/pinnacle_1394.c`); with
`--debug` the steps before it are in the log.

- `FireWire link init failed while waiting for the FireWire bus to come up (no
  valid node ID)`; the log shows `NodeID 0x0000ffff`: the link controller works
  but no 1394 bus reset completed. The deck/camera FireWire port or cable isn't
  answering electrically. Try another cable, power-cycle the deck with the cable
  attached. Not fixable in software.
- `... USB transfer error (LIBUSB_ERROR_...)` at a step: real USB/driver problem.
- `Cannot open the device: ...`: libusb open/claim failed (another process, driver).
- `Cannot read FPGA bitstream` / `Device initialisation failed`: firmware problem.
- Analog start failures still show the generic "USB error" (not yet detailed).

## Frame-accuracy check (analog)

User plays a QR-coded test DVD (frame number per frame) into S-Video and
captures ~65 min in the GUI; analyze with
`python analyze_qrcode_video.py "<file>.mkv" --max-duration 0` from
`C:\Users\Jonas\Desktop\scripts\analyze-qrcode`. Baseline: 0 missing / 0 dup /
0 undecodable. See `docs/analog.md`.
