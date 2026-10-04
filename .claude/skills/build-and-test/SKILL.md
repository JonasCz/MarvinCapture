---
name: build-and-test
description: Build the Pinnacle 500-USB driver (core library, MarvinCaptureCLI, GUI) with scripts/build.ps1 on Windows or scripts/build.sh on macOS/Linux, and run the automated ctest suite. Use when asked to build or rebuild. For testing against the real device see test-with-hardware-device.
---

# Build and test

Full reference: `docs/building.md`. Windows is the practical, tested path; macOS and Linux are below.
Build output is per platform, `build/<os>-<arch>/{core,dist,gui}` (os = windows,
macos, linux; arch = x86_64, arm64), so builds from different platforms can share one tree.

## Build

Always use the script; it builds FFmpeg (first time), the core DLL, the CLI,
runs ctest, builds the GUI and assembles `build\windows-x86_64\dist`:

```powershell
scripts\build.ps1               # everything
scripts\build.ps1 -SkipGui      # core + CLI + tests only (fast, no .NET needed)
scripts\build.ps1 -SkipTests    # skip ctest
```

It needs MSYS2 UCRT64 (`C:\msys64`) and, for the GUI, the .NET 10 SDK. It puts
`C:\msys64\ucrt64\bin` on PATH itself.

Quick native-only iteration (no script): in PowerShell with
`$env:PATH = "C:\msys64\ucrt64\bin;C:\msys64\usr\bin;$env:PATH"`, then
`cmake --build build/windows-x86_64/core` and `ctest --test-dir build/windows-x86_64/core`. This does **not**
update `build\windows-x86_64\dist`; either rerun the script or copy `marvin-core.dll` and
`MarvinCaptureCLI.exe` from `build\windows-x86_64\core` into `build\windows-x86_64\dist`.

Output: `build\windows-x86_64\dist` holds `MarvinCaptureGUI.exe`, `MarvinCaptureCLI.exe`,
`marvin-core.dll`, `libusb-1.0.dll`, `libwinpthread-1.dll`, the runtime files of
the GUI and `firmware\`. (The old `cli` folder with pincli, pinanalog,
pindeck, pinlist is gone; the script deletes a leftover one. Trees from before the
per-platform layout, `build\core`, `build\dist`, `build\gui`, are no longer used;
`-Clean` does not touch them, delete them by hand.) `MarvinCaptureCLI`
is the only command-line program; run it from `build\windows-x86_64\dist`.

## macOS and Linux

```sh
bash scripts/build.sh                  # core, CLI, ctest
bash scripts/build.sh --config Debug   # Release (default) or Debug
bash scripts/build.sh --skip-tests     # skip ctest
bash scripts/build.sh --clean          # delete only this platform's build/<os>-<arch> first
```

`--skip-gui` is accepted but does nothing yet (there is no macOS/Linux GUI).
Output in `build/<os>-<arch>/dist`: `MarvinCaptureCLI`, the core library
(`libmarvin-core.dylib` on macOS, `libmarvin-core.so` on Linux), on macOS the
bundled `libusb-1.0.0.dylib` (no Homebrew needed at run time), and `firmware/`;
for example `build/macos-arm64/dist/MarvinCaptureCLI`. The CMake tree is
`build/<os>-<arch>/core`.

## Pitfalls

- **Exit code -1073741511 (0xC0000139) from the CLI**: it picked up a wrong
  `libwinpthread-1.dll` from PATH (Git's mingw64, `Desktop\ffmpeg`). The script
  copies the right one next to it; if you see this on an old
  `build\windows-x86_64\dist`, rebuild with the script.
- Don't rebuild `build\windows-x86_64\dist` while the GUI is open (it locks the files).
- **Edits that silently don't apply**: when patching source with scripts, assert
  the match exists; the files are CRLF in the working tree. Prefer the Edit tool.

## Automated tests vs. hardware tests

`ctest` (and `scripts\build.ps1` without `-SkipTests`) is the **automated code
test suite**: unit tests plus replay tests that run on recordings, no device
needed. Testing against the real device, its deck or camera is a separate,
manual activity: use the **test-with-hardware-device** skill (MarvinCaptureCLI,
`--debug` logs, diagnosing "USB error"). The replay tests use the `PIN_REPLAY`
environment variable (the replay device); it is the only variable ctest relies on.
