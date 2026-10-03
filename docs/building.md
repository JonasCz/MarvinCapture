# Building

`scripts\build.ps1` does everything and leaves a runnable tree in `build\dist`.

```powershell
scripts\build.ps1                 # FFmpeg (first time), core, tests, GUI, dist
scripts\build.ps1 -SkipTests      # faster
scripts\build.ps1 -SkipGui        # native core and CLIs only
scripts\build.ps1 -Clean          # start from an empty build\
scripts\build.ps1 -Config Debug
```

## Prerequisites

- **MSYS2** with the UCRT64 toolchain (default location `C:\msys64`; use
  `-Msys2 <root>` otherwise):
  `pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,libusb,pkgconf,nasm,make,diffutils}`
- The **.NET 10 SDK** (only for the GUI). NuGet packages come from nuget.org.

No device is needed to build or to run the tests.

## Output

```
build\
  core\            CMake build tree: static libraries, the DLL, the CLIs, the test executables
  dist\            what you run and ship
    MarvinCaptureGUI.exe, *.dll, *.xbf ...    the GUI (self-contained, unpackaged)
    marvin-core.dll, libusb-1.0.dll    the native core
    firmware\                                FPGA bitstreams
    cli\                                     pincli pinanalog pindeck pinlist pinctl
                                             + marvin-core.dll, libusb-1.0.dll, firmware\
```

The GUI and the CLIs find `firmware\` next to the core library, so `build\dist`
can be copied anywhere. The CLIs also default to `firmware\` relative to the
current directory, so run them from `build\dist\cli`.

## What the script does

1. **FFmpeg**, once: `scripts/build-ffmpeg.sh` fetches FFmpeg 8.1.3, checks its
   sha256 and builds a minimal LGPL static slice into `third_party/`. See
   [../third_party/README.md](../third_party/README.md).
2. **Core**: `cmake -G Ninja -B build\core` then `cmake --build`. Targets:
   `pinnacle_core` (hardware layer), `pinnacle_engine` / `pinnacle_engine_pure`
   (session engine), `pinnacle_sinks` (file writers), `marvin-core` (the
   DLL, `src/api/pin_api.h`) and the CLIs.
3. **Tests**: `ctest --test-dir build\core --output-on-failure`.
4. **GUI**: `dotnet build gui\windows\MarvinCaptureGUI -c Release -p:Platform=x64
   -p:PinnacleCoreDir=build\core -p:OutDir=build\dist\`. Details in
   [../gui/windows/README.md](../gui/windows/README.md).

## Tests

`tests/` has three kinds:

- `tests/engine/`, `tests/test_lock.c`, `tests/sinks/test_analog_sinks.c`: plain
  unit tests.
- `tests/engine_replay/` and the rest of `tests/sinks/`: drive the whole session
  engine through `pin_api.h` against the **replay device** (`PIN_REPLAY=<file>`,
  a virtual device that plays a recording back), covering DV/HDV formats, scene
  splitting, multi-pass, preview frames and output checks.
- `tests/replay_reassembler.c` plus `tests/baseline_sha256.txt`: the DV
  reassembler's output must stay byte-identical.

The replay tests need small recordings in `tests/data/`. They are not committed;
without them those tests skip themselves. See
[../tests/data/README.md](../tests/data/README.md).

## Repository layout

```
src/core/      hardware layer: USB device, 1394 link, streams, DV/HDV reassembly,
               analog, config channel, device lock, AVI writer
src/engine/    session engine: state machine, deck, preview, scene split, settings
src/sinks/     file writers (raw, AVI, MOV/MKV rewrap, FFV1)
src/api/       pin_api.h / pin_api.c, the flat C API the GUI links
src/cli/       pincli, pinanalog, pindeck, pinlist, pinctl
gui/windows/   WinUI 3 app
tests/         unit and replay tests; tests/data/ = local fixtures
firmware/      FPGA bitstreams (see firmware/README.md)
scripts/       build.ps1, build-ffmpeg.sh, extract-bitstreams.py, driver-status.ps1
third_party/   vendored FFmpeg, built from source
docs/          documentation
build/         all build output (git-ignored)
```

The source is portable C11 and the CMake build also works with gcc and libusb on
Linux (`cmake -G Ninja -B build/core && cmake --build build/core`), but Windows is
the platform that is built and tested regularly.
