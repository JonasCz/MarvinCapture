---
name: build-and-test
description: Build the Pinnacle 500-USB driver (core DLL, CLIs, GUI) with scripts/build.ps1 and run the automated ctest suite. Use when asked to build or rebuild. For testing against the real device see test-with-hardware-device.
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
- Don't rebuild `build\dist` while the GUI is open (it locks the files).
- **Edits that silently don't apply**: when patching source with scripts, assert
  the match exists; the files are CRLF in the working tree. Prefer the Edit tool.

## Automated tests vs. hardware tests

`ctest` (and `scriptsbuild.ps1` without `-SkipTests`) is the **automated code
test suite**: unit tests plus replay tests that run on recordings, no device
needed. Testing against the real device, its deck or camera is a separate,
manual activity: use the **test-with-hardware-device** skill (CLIs, debug logs,
diagnosing "USB error").
