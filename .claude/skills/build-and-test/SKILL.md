---
name: build-and-test
description: Build the Pinnacle 500-USB driver (core DLL, CLIs, GUI) with scripts/build.ps1, run the tests, and test against the real device with the CLIs. Use when asked to build, rebuild, test, or reproduce a hardware problem from the command line.
---

# Build and test

Full reference: `docs/building.md`. This is the practical, tested path on Windows.

## Build

Always use the script; it builds FFmpeg (first time), the core DLL, the CLIs,
runs ctest, builds the GUI and assembles `build\dist`:

```powershell
scripts\build.ps1               # everything
scripts\build.ps1 -SkipGui      # core + CLIs + tests only (fast, no .NET needed)
scripts\build.ps1 -SkipTests    # skip ctest
```

It needs MSYS2 UCRT64 (`C:\msys64`) and, for the GUI, the .NET 10 SDK. It puts
`C:\msys64\ucrt64\bin` on PATH itself.

Quick native-only iteration (no script): in PowerShell with
`$env:PATH = "C:\msys64\ucrt64\bin;C:\msys64\usr\bin;$env:PATH"`, then
`cmake --build build/core` and `ctest --test-dir build/core`. This does **not**
update `build\dist`; either rerun the script or copy `pinnacle-oss-core.dll` and
the changed exes from `build\core` into `build\dist\cli`.

Output: `build\dist` (GUI + core DLL + `firmware\`) and `build\dist\cli`
(`pincli pinanalog pindeck pinlist pinctl` + `pinnacle-oss-core.dll`,
`libusb-1.0.dll`, `libwinpthread-1.dll`, `firmware\`). Run the CLIs from
`build\dist\cli` (they default to `firmware\` in the cwd).

## Pitfalls

- **Exit code -1073741511 (0xC0000139) from a CLI**: it picked up a wrong
  `libwinpthread-1.dll` from PATH (Git's mingw64, `Desktop\ffmpeg`). The script
  now copies the right one next to the CLIs; if you see this on an old
  `build\dist`, rebuild with the script.
- **The GUI and the CLIs cannot hold the device at once.** `pinlist` shows
  `IN USE (pid N, ...)`. Close the GUI before testing with a CLI; ask the user
  rather than killing it.
- **Edits that silently don't apply**: when patching source with scripts, assert
  the match exists; the files are CRLF in the working tree. Prefer the Edit tool.
- Don't start a hardware test that needs a replug without asking the user.

## Testing against the device (CLIs)

```powershell
cd build\dist\cli
.\pinlist.exe                                   # devices and READY / IN USE
.\pinctl.exe status usb:1-8
.\pinctl.exe capture -d usb:1-8 -i dv -o $env:TEMP\x.dv --duration 3
.\pinctl.exe deck usb:1-8 state
```

`pinctl` runs through the engine API, which swallows log output into events, so
failures only show the session's error text. For the **raw log of the DV/1394
bring-up** use the older, simpler CLI, which logs to stderr:

```powershell
$env:PINNACLE_DEBUG_1394 = "1"                  # 2 = also hex-dump EP 0x84
.\pincli.exe -o $env:TEMP\x.dv -t 3
```

Other debug env vars: `PINNACLE_PROBE`, `PINNACLE_DEBUG_EP88`, `PINNACLE_DEBUG_EP84`
(see `src/core/pinnacle_device.c`).

## Diagnosing "USB error" / device not ready

The engine error text names the failing bring-up step (`pin_session.c`,
`pinnacle_stream.c: stream_fail`, `pinnacle_1394.c: l->step`). Known cases:

- `... waiting for the FireWire bus to come up (no valid node ID)`: the link
  controller works but no 1394 bus reset ever completed (`NodeID 0x0000ffff`).
  The deck's FireWire port/cable is not answering electrically; try another
  cable, power-cycle the deck with the cable plugged in. Not fixable in software.
- `... USB transfer error (LIBUSB_ERROR_...)` at some step: a real USB/driver
  problem; the step name says where.
- `Cannot open the device: ...`: libusb open/claim failed (another process,
  driver). `Device initialisation failed` / `Cannot read FPGA bitstream`: firmware.
