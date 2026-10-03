---
name: test-with-hardware-device
description: Test the Pinnacle 500-USB driver against the real attached device (and deck/camera) with the CLIs pinlist, pinctl, pincli, pinanalog, pindeck; read debug logs; diagnose "USB error" / device-not-ready. Manual hardware testing, NOT the automated ctest suite.
---

# Testing with the real hardware

This is **manual testing against the physical device**. It is separate from the
automated code tests (`ctest`, replay tests), which need no device; see the
`build-and-test` skill for those and for building. Build first
(`scripts\build.ps1`), then work from `build\dist\cli`.

## Rules of the road

- **One owner at a time.** The GUI and the CLIs cannot hold the device together.
  `pinlist` shows `IN USE (pid N, ...)`; ask the user to close the GUI, don't kill it.
- **Don't rebuild `build\dist` while the GUI is open**, and don't run heavy jobs
  during a latency-sensitive capture.
- **Replug rule:** if the device ends up needing a physical replug / power
  cycle, stop and ask the user. A wedged command channel usually recovers on the
  next full bring-up (bitstream reload); try one normal run first.
- Don't hardcode unit data (GUID etc.); other Marvin models are expected later.
- Needs the WinUSB driver bound to `USB\VID_2304&PID_0213` (`docs/windows-driver.md`).
- Bring-up takes ~5.5 s before data flows; a window shorter than that looks empty.
- A camera/deck in tape mode with the tape stopped sends zero bytes, which looks
  like a hardware fault. Put it in play (or camera mode) first.

## Setup

```powershell
cd build\dist\cli          # CLIs read firmware\ relative to the cwd (or pass -b)
```

The exes need `libwinpthread-1.dll` and `libusb-1.0.dll` next to them (the build
script copies them). Exit code `-1073741511` (0xC0000139) = a wrong
`libwinpthread` was found on PATH; rebuild with the script.

## The tools

| tool | use |
|---|---|
| `pinlist` | list units: `usb:1-8  Pinnacle Studio 500-USB  2304:0213  READY` or `IN USE (pid, state)` |
| `pinctl` | the session API as a CLI; what the GUI uses. Best for end-to-end tests |
| `pincli` | raw DV/HDV capture straight from FireWire; logs to stderr (best for bring-up debugging) |
| `pinanalog` | analog composite/S-video capture to AVI, decoder status |
| `pindeck` | AV/C deck control (play, stop, rew, timecode...) |

### pinctl (device id from `pinlist`, e.g. `usb:1-8`)

```
pinctl list | watch
pinctl status <id>                      # e.g. "Closed (no signal)"
pinctl capture -d <id> -i dv|svideo|composite [-s pal|ntsc|auto] [-f format] -o <path>
               [--title T] [--split] [--passes N] [--idle-min M] [--duration S]
               [--aspect auto|4:3|16:9] [--start-deck] [--rewind-first]
pinctl deck <id> play|pause|stop|ff|rew|state|timecode
pinctl preview-dump <id> [-n N] [-o prefix]     # save preview frames
pinctl preview-rate <id> [seconds]              # preview frame rate
pinctl monitor <id> [seconds] [T:in=dv|svideo|composite] [T:std=pal|ntsc]
pinctl actions <id> --actions a,b,c [...]
```

On failure it prints `pinctl: device not ready: <engine error text>`. Its log
lines are swallowed into API events, so for bring-up detail use `pincli`.

### pincli (DV/HDV)

```
pincli -o out.dv -t 5          # .dv for DV, .ts for HDV (format auto-detected)
```
`-b <bitstream>` (default `firmware/fpga-ohci.bin`), `-t` seconds (else Ctrl+C).

### pinanalog

```
pinanalog -i svideo -s auto -o out.avi -t 10
pinanalog --status             # decoder lock/standard, then exit
```
Also `-b` (default `firmware/fpga-capture.bin`), `--brightness 0..255 --contrast 0..127
--saturation 0..127 --hue -128..127 --sharpness 0..3 --tv-mode`, `--raw FILE`.

### pindeck

```
pindeck state play wait:5 stop state     # steps run in order
```
Steps: `play pause stop ff rew state timecode subunits wait:<sec> raw:<hex> reg:<offset>`;
`-v/-vv` verbose, `-r raw.bin`, `-b bitstream`. See `docs/deck-control.md`.

## Debug logging

Env vars (set in the same shell before running):

| var | effect |
|---|---|
| `PINNACLE_DEBUG_1394=1` (or `2`) | 1394 link trace; 2 also hex-dumps EP 0x84. Shown by `pincli`, `pindeck` |
| `PINNACLE_PROBE=1` | read OHCI registers after start-up |
| `PINNACLE_DEBUG_EP84` / `_EP88` | endpoint traffic debug |
| `PINNACLE_RAW_DUMP`, `PINNACLE_VIDEO_QUEUE`, `PINNACLE_VIDEO_XFER` | raw dump / URB queue tuning |
| `PIN_REPLAY=<file>` | virtual replay device (no hardware) |
| `PINNACLE_LOG_LEVEL=0` | GUI/engine: include debug lines |

To read the GUI's console log, launch `build\dist\MarvinCaptureGUI.exe` yourself
with stdout redirected to a file.

## Diagnosing "USB error" / device not ready

The engine error text names the failing step (`stream_fail` in
`src/core/pinnacle_stream.c`, step markers in `src/core/pinnacle_1394.c`).

- `FireWire link init failed while waiting for the FireWire bus to come up (no
  valid node ID)`; `pincli` log shows `NodeID 0x0000ffff`: the link controller works
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
