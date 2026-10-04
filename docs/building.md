# Building

One script per platform does everything and leaves a runnable tree in
`build/<os>-<arch>/dist`:

- **Windows**: `scripts\build.ps1` (core, CLI, tests and the GUI)
- **macOS and Linux**: `scripts/build.sh` (core, CLI and tests; there is no
  macOS/Linux GUI yet)

```powershell
scripts\build.ps1                 # FFmpeg (first time), core, tests, GUI, dist
scripts\build.ps1 -SkipTests      # faster
scripts\build.ps1 -SkipGui        # native core and CLI only
scripts\build.ps1 -Clean          # start from an empty build\windows-x86_64\
scripts\build.ps1 -Config Debug
```

```sh
scripts/build.sh                  # FFmpeg (first time), core, tests, dist
scripts/build.sh --skip-tests     # faster
scripts/build.sh --skip-gui       # accepted for symmetry; there is no GUI to skip yet
scripts/build.sh --clean          # start from an empty build/<os>-<arch>/
scripts/build.sh --config Debug
```

`-Clean` / `--clean` only remove the current platform's directory, so builds for
several platforms can share one checkout.

## Prerequisites

No device is needed to build or to run the tests.

### Windows

- **MSYS2** with the UCRT64 toolchain (default location `C:\msys64`; use
  `-Msys2 <root>` otherwise):
  `pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,libusb,pkgconf,nasm,make,diffutils}`
- The **.NET 10 SDK** (only for the GUI). NuGet packages come from nuget.org.

### macOS

Apple Silicon (arm64) only for now; `scripts/build.sh` stops on an Intel Mac.

- The **Xcode Command Line Tools** (`xcode-select --install`): clang, make.
- **Homebrew**: `brew install cmake ninja nasm libusb pkgconf`
  (nasm is only used for x86 FFmpeg builds, but is harmless to have).

### Linux

gcc (or clang), make, cmake, ninja, pkg-config and the libusb-1.0 development
package (and nasm on x86_64), e.g. on Debian/Ubuntu:
`sudo apt install build-essential cmake ninja-build pkg-config libusb-1.0-0-dev nasm`.

## Output

Everything goes into a per-platform directory, named like the FFmpeg build in
`third_party/` (`windows-x86_64`, `macos-arm64`, `linux-x86_64`, `linux-arm64`):

```
build/
  <os>-<arch>/
    core/          CMake build tree: static libraries, the core library, the CLI, the test executables
    gui/           GUI build intermediates (Windows only for now)
    dist/          what you run and ship
```

`dist/` on Windows (`build\windows-x86_64\dist`):

```
MarvinCaptureGUI.exe, *.dll, *.xbf ...   the GUI (self-contained, unpackaged)
marvin-core.dll, libusb-1.0.dll          the native core
libwinpthread-1.dll                      runtime DLL the core imports
MarvinCaptureCLI.exe                     the command-line program (shares the DLL and firmware\)
firmware\                                FPGA bitstreams
```

`dist/` on macOS and Linux (`build/macos-arm64/dist`, `build/linux-x86_64/dist`, ...):

```
MarvinCaptureCLI                         the command-line program
libmarvin-core.dylib                     the native core (libmarvin-core.so on Linux)
libusb-1.0.0.dylib                       macOS only: libusb, bundled from Homebrew
firmware/                                FPGA bitstreams
```

The GUI and the CLI find `firmware/` next to the core library, and
`MarvinCaptureCLI` finds the core library next to itself (rpath `@loader_path`
on macOS, `$ORIGIN` on Linux), so `dist/` can be copied anywhere.

libusb: on macOS the build copies Homebrew's `libusb-1.0.0.dylib` next to the
core and points the core at that copy (`@loader_path`, re-signed ad hoc), so
`dist/` runs without Homebrew; Homebrew's libusb is only needed to build. On
Linux the core uses the distribution's `libusb-1.0` (install the runtime package,
e.g. `libusb-1.0-0`).

## What the scripts do

1. **FFmpeg**, the first time and whenever `scripts/build-ffmpeg.sh` changed
   (its sha256 is kept in `third_party/ffmpeg-<os>-<arch>/build-script.sha256`):
   `scripts/build-ffmpeg.sh` fetches FFmpeg 8.1.3, checks its sha256 and builds a
   minimal LGPL static slice into `third_party/ffmpeg-<os>-<arch>/`. See
   [../third_party/README.md](../third_party/README.md).
2. **Core**: `cmake -G Ninja -B build/<os>-<arch>/core` then `cmake --build`.
   Targets: `pinnacle_core` (hardware layer), `pinnacle_engine` /
   `pinnacle_engine_pure` (session engine), `pinnacle_sinks` (file writers),
   `marvin-core` (the shared library, `src/api/pin_api.h`) and `MarvinCaptureCLI`.
3. **Tests**: `ctest --test-dir build/<os>-<arch>/core --output-on-failure`.
4. **dist**: copies the CLI, the core library, libusb (Windows `libusb-1.0.dll`,
   macOS `libusb-1.0.0.dylib`; Windows also
   `libwinpthread-1.dll`) and `firmware/` into `dist/`.
5. **GUI** (Windows only): `dotnet build gui\windows\MarvinCaptureGUI -c Release
   -p:Platform=x64 ...` against `build\windows-x86_64\core`, output into `dist\`.
   Details in [../gui/windows/README.md](../gui/windows/README.md).
6. **Check**: fails if an expected file is missing from `dist/`, else prints
   `Done: <dist>`.

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
src/cli/       MarvinCaptureCLI
gui/windows/   WinUI 3 app
tests/         unit and replay tests; tests/data/ = local fixtures
firmware/      FPGA bitstreams (see firmware/README.md)
scripts/       build.ps1 (Windows), build.sh (macOS/Linux), build-ffmpeg.sh,
               extract-bitstreams.py, driver-status.ps1
third_party/   vendored FFmpeg, built from source
docs/          documentation
build/         all build output, one directory per platform (git-ignored)
```

The source is portable C11. Windows is the platform that is built and tested
regularly, with the GUI. On macOS (Apple Silicon) `scripts/build.sh` builds the
core and MarvinCaptureCLI and the whole ctest suite passes (not yet tried
against the device there). On Linux the same CMake build passes ctest and the
CLI has been run against the device. There is no macOS or Linux GUI yet.
