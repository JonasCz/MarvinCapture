# Building

One script per platform does everything and leaves a runnable tree in
`build/<os>-<arch>/dist`:

- **Windows**: `scripts\build.ps1` (core, CLI, tests and the GUI)
- **macOS**: `scripts/build.sh` (core, CLI, tests and the GUI, `MarvinCapture.app`)
- **Linux**: `scripts/build.sh` (core, CLI and tests; there is no Linux GUI yet)
- **Android** (cross-built on Linux or macOS): `scripts/build-android.sh` (the native
  part with `scripts/build-android-native.sh`, then the app with Gradle), see
  [Android](#android)

```powershell
scripts\build.ps1                 # FFmpeg (first time), core, tests, GUI, dist
scripts\build.ps1 -SkipTests      # faster
scripts\build.ps1 -SkipGui        # native core and CLI only
scripts\build.ps1 -Clean          # start from an empty build\windows-x86_64\
scripts\build.ps1 -Config Debug
```

```sh
scripts/build.sh                  # FFmpeg (first time), core, tests, GUI (macOS), dist
scripts/build.sh --skip-tests     # faster
scripts/build.sh --skip-gui       # native core and CLI only (no MarvinCapture.app on macOS)
scripts/build.sh --clean          # start from an empty build/<os>-<arch>/
scripts/build.sh --config Debug
```

`-Clean` / `--clean` only remove the current platform's directory, so builds for
several platforms can share one checkout.

## Prerequisites

No device is needed to build or to run the tests.

`scripts/install-deps.sh` (Debian/Ubuntu with apt, macOS with Homebrew) and
`scripts\install-deps.ps1` (Windows, into an existing MSYS2; `-Arch x64|arm64`) install
everything listed below except MSYS2 itself, the .NET SDK and the Xcode Command Line
Tools, skipping what is already there. CI uses the same scripts.

For the tests, also the **ffmpeg command-line tool** (`scripts/install-deps.sh --tests`,
`scripts\install-deps.ps1 -Tests`; or `apt install ffmpeg`, `brew install ffmpeg`, MSYS2
`mingw-w64-ucrt-x86_64-ffmpeg` / `mingw-w64-clang-aarch64-ffmpeg`): the ctest suite makes
its test recordings with it ([Tests](#tests)). Without it the build works and those tests
skip. This is a separate program from the minimal static FFmpeg the core links (built
from source into `third_party/`); the core loads none of ffmpeg's own libraries or DLLs.

### Windows

- **MSYS2** with the UCRT64 toolchain (default location `C:\msys64`; use
  `-Msys2 <root>` otherwise):
  `pacman -S make diffutils mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,libusb,pkgconf,nasm}`;
  for the installer also `mingw-w64-ucrt-x86_64-nsis`
- Windows on Arm (`-Arch arm64`, the default on an ARM64 machine; output in
  `build\windows-arm64\`): the CLANGARM64 toolchain instead, as MSYS2 has no gcc there:
  `pacman -S make diffutils mingw-w64-clang-aarch64-{clang,cmake,ninja,libusb,pkgconf}`
  (and `mingw-w64-ucrt-x86_64-nsis` for the installer).
  Built by CI, not tested on hardware.
- The **.NET 10 SDK** (only for the GUI). NuGet packages come from nuget.org.
- For the installer only: **Visual Studio 2022 or later, or its Build Tools**, with the C++
  build tools for x64/x86 and for ARM64, to build libwdi's driver tool (see
  [../packaging/windows/README.md](../packaging/windows/README.md)).

### macOS

Apple Silicon (arm64) or Intel (x86_64); the build is native (no cross-compiling), so it
produces `macos-arm64` on an Apple Silicon Mac and `macos-x86_64` on an Intel Mac.
Intel builds come from CI and have not been tried against the device.

- The **Xcode Command Line Tools** (`xcode-select --install`): clang, make, and
  the Swift 6.x toolchain and `iconutil` for the GUI. Xcode itself is not needed.
  macOS 15 or later is needed to run the GUI.
- **Homebrew**: `brew install cmake ninja nasm libusb pkgconf`
  (nasm is only used for x86 FFmpeg builds, but is harmless to have).

### Linux

gcc (or clang), make, cmake, ninja, pkg-config and the libusb-1.0 development
package (and nasm on x86_64), e.g. on Debian/Ubuntu:
`sudo apt install build-essential cmake ninja-build pkg-config libusb-1.0-0-dev nasm`
(and `dpkg-dev` for the `.deb`).

## Output

Everything goes into a per-platform directory, named like the FFmpeg build in
`third_party/` (`windows-x86_64`, `macos-arm64`, `linux-x86_64`, `linux-arm64`):

```
build/
  <os>-<arch>/
    core/          CMake build tree: static libraries, the core library, the CLI, the test executables
    gui/           GUI build intermediates (Windows and macOS)
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
MarvinCapture.app/                       macOS only: the GUI (see below), ad-hoc signed
MarvinCaptureCLI                         the command-line program
libmarvin-core.dylib                     the native core (libmarvin-core.so on Linux)
libusb-1.0.0.dylib                       macOS only: libusb, bundled from Homebrew
firmware/                                FPGA bitstreams
```

`MarvinCapture.app` is self-contained (bundle id `jonascz.MarvinCapture`):

```
MarvinCapture.app/Contents/
  Info.plist                             generated by build.sh (version 0.1.0, build = git commit count)
  MacOS/MarvinCapture                    the Swift executable
  Frameworks/libmarvin-core.dylib        the core, found through the rpath @executable_path/../Frameworks
  Frameworks/libusb-1.0.0.dylib          bundled libusb
  Resources/firmware/                    FPGA bitstreams (not in Frameworks: non-code files there break codesign)
  Resources/AppIcon.icns                 the logo (gui/macos/Resources/AppIcon.iconset), packed with iconutil
  _CodeSignature/                        ad-hoc signature
```

The CLI and the core in `dist/` find `firmware/` next to the core library, and
`MarvinCaptureCLI` finds the core library next to itself (rpath `@loader_path`
on macOS, `$ORIGIN` on Linux), so `dist/` can be copied anywhere. The app brings
its own copies of the core, libusb and firmware. A downloaded copy of the app is
quarantined by Gatekeeper; see [../gui/macos/README.md](../gui/macos/README.md#distribution).

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
5. **GUI**:
   - Windows: `dotnet publish gui\windows\MarvinCaptureGUI -c Release
     -p:Platform=x64 ...` against `build\windows-x86_64\core`, output into `dist\`
     (trimmed, ReadyToRun; see [windows-size.md](windows-size.md)). With
     `-Config Debug` a plain `dotnet build` instead. `dist\` is emptied first.
     Details in [../gui/windows/README.md](../gui/windows/README.md).
   - macOS: `swift build` (SwiftPM, `gui/macos`, intermediates in
     `build/macos-arm64/gui`, linked against `build/macos-arm64/core` with
     `-Xlinker -L`), the app icon (generated and cached), then `dist/MarvinCapture.app`
     is assembled, the dylibs and the bundle are signed ad hoc and verified.
     Needs only the Command Line Tools. Details in
     [../gui/macos/README.md](../gui/macos/README.md).
   - Linux: none yet.
6. **Check**: fails if an expected file is missing from `dist/`, else prints
   `Done: <dist>`.

## Android

Two scripts, on Linux or macOS: `scripts/build-android-native.sh` cross-builds the
native libraries with the Android NDK, and `scripts/build-android.sh` runs it and then
builds the app (`gui/android`, see [its README](../gui/android/README.md)) with
Gradle into one APK:

```sh
scripts/build-android.sh                                  # release APK, arm64-v8a
scripts/build-android.sh --debug --abi x86_64             # debug APK for the emulator
scripts/build-android.sh --abi arm64-v8a --abi x86_64     # both ABIs in one APK
scripts/build-android.sh --skip-native                    # natives as already built

scripts/build-android-native.sh                   # native part only: arm64-v8a and x86_64
scripts/build-android-native.sh --abi arm64-v8a   # one ABI (repeatable)
scripts/build-android-native.sh --clean           # start from an empty build/android-<abi>/
```

Prerequisites: the **Android NDK 29.0.14206865** (pinned in `scripts/android-ndk.sh`;
GitHub's ubuntu-24.04 runner has it under `$ANDROID_HOME/ndk/`; elsewhere
`sdkmanager "ndk;29.0.14206865"`), found through `ANDROID_NDK_HOME` or
`ANDROID_HOME`/`ANDROID_SDK_ROOT`; cmake, ninja, make, pkg-config, curl, and nasm for
x86_64. API level 30 (Android 11, the app's minSdk). For the app also a JDK 17 or newer
and the SDK's `platforms;android-36` and `build-tools;36.0.0` (`ANDROID_HOME`;
`/opt/android-sdk` is tried when it is unset). Gradle (8.14.3, wrapper with a pinned
checksum) and the Android Gradle plugin (8.13.2) are downloaded on the first run, from
`services.gradle.org`, Google's Maven repository and Maven Central (as
`repo1.maven.org`); the app itself has no dependencies.

What `build-android-native.sh` does, per ABI:

1. FFmpeg: `scripts/build-ffmpeg.sh --android <abi>` (the same components as the
   desktop builds; NDK clang, `--target-os=android`) into
   `third_party/ffmpeg-android-<arch>/`, rebuilt when `build-ffmpeg.sh` or
   `android-ndk.sh` changes. x86_64 keeps its assembly.
2. libusb 1.0.30 from its release tarball (sha256 pinned in the script), static,
   without udev, into `third_party/libusb-android-<arch>/`.
3. The core with the NDK's CMake toolchain file and `-DPIN_BUILD_TESTS=OFF` (the
   ctest suite cannot run on the build machine) into `build/android-<abi>/core`.
4. The app's JNI layer (`gui/android/app/src/main/cpp`, its own small CMake project:
   the `pin_api.h` wrappers, the OpenGL ES preview, the AAudio monitor) with the same
   toolchain, linked against that core, into `build/android-<abi>/jni`.
5. `build/android-<abi>/dist`, stripped:

```
libmarvin-core.so      the core with FFmpeg and libusb linked in; exports pin_api.h only
libmarvin-jni.so       the app's JNI layer (needs libmarvin-core.so)
MarvinCaptureCLI       the command line, for Termux (RUNPATH $ORIGIN: the .so next to it)
firmware/              FPGA bitstreams
```

It then checks that the two libraries need no shared libusb or FFmpeg (only `libc`,
`libm`, `libdl`, and the NDK's `libandroid` and `libmediandk` for FFmpeg's MediaCodec
encoders, plus `liblog`, `libEGL`, `libGLESv3` and `libaaudio` for the JNI layer, all
present on every device), have no text relocations, and that their LOAD segments are
aligned to 16 KB (`-Wl,-z,max-page-size=16384`, which devices with 16 KB pages and
Google Play require). `<arch>` is `arm64` or `x86_64` (the `third_party/` naming),
`<abi>` Android's `arm64-v8a` or `x86_64` (x86_64 is for the emulator).

`build-android.sh` then runs `gui/android/gradlew assembleRelease` (or
`assembleDebug`) with `-PmarvinAbis=<the ABIs>`: Gradle only packages the prebuilt
`.so` files (no CMake or NDK inside Gradle), the FPGA bitstreams as assets (the app
copies them to its files directory on first start and calls `pin_set_firmware_dir()`)
and the Java code (R8 and resource shrinking for release). The APK goes to
`build/android/MarvinCapture-<version>-android.apk` (`-debug.apk` for `--debug`); the
script verifies its signature and the 16 KB alignment and prints its size per part. A
release APK is signed from `MARVIN_ANDROID_KEYSTORE`, `MARVIN_ANDROID_KEYSTORE_PASSWORD`,
`MARVIN_ANDROID_KEY_ALIAS` and `MARVIN_ANDROID_KEY_PASSWORD`, else with the debug key
(with a warning). The device comes from `UsbManager` as a file descriptor
(`pin_open("fd:N")`), and the app passes its own directories with `pin_set_app_dirs()`.
For Termux, see [cli.md](cli.md#android-termux).

For CI: the script is non-interactive and needs no `local.properties`. The GitHub
Actions workflow builds the arm64-v8a release APK on `ubuntu-24.04`, which has the JDK,
SDK and NDK preinstalled, and caches `third_party/ffmpeg-android-*`,
`third_party/libusb-android-*` and Gradle's downloads; see [ci.md](ci.md) (also for the
release signing key).

## Installers

Each builds the platform first (pass `--skip-build` / `-SkipBuild` to reuse
`dist/`) and writes to `build/<os>-<arch>/installer/`. The version comes from the
`"version"` field of the top-level `VERSION` file. All are unsigned for now (see the README's Installation
section for what users see).

`VERSION` is JSON: `"version"` (dotted number, compiled into the core as
`pin_app_version()`), `"release_notes"` (plain text) and `"download_url"` (the
download page). The CLI (`--help`, before the device list) and both GUIs (a
banner at startup) fetch the copy on the `main` branch from
raw.githubusercontent.com at most once a week (the time of the last successful check is the
setting `update.last_check`, the answer is kept too) and announce a newer `"version"` until
the app is updated; offline or on any error they show nothing and try again at the next start. So to release: bump `VERSION`, build and package, publish
the installers, then push `VERSION` to `main`. `MARVIN_UPDATE_URL` replaces the
address (an http(s) URL or a local file, for testing; empty turns the check off).
On macOS and Linux the core loads libcurl at run time for the check (the `.deb`
only recommends it); Windows uses WinHTTP.
On Android the app downloads the file itself and hands it to the core (`pin_update_check_json`),
and the result is shown in the options panel until the app is updated.

| Script | Output | Notes |
|---|---|---|
| `scripts/package-macos.sh` | `MarvinCapture-<version>-macos-<arch>.pkg` | `pkgbuild` + `productbuild`, choices "app" (`/Applications`) and "cli" (`/usr/local/lib/marvincapture`, link in `/usr/local/bin`); resources and `uninstall.sh` in `packaging/macos/` |
| `scripts\package-windows.ps1` | `MarvinCapture-<version>-windows-<arch>-setup.exe` | `-Arch x64\|arm64`; NSIS (`makensis` from NSIS or `mingw-w64-ucrt-x86_64-nsis`); builds libwdi's `wdi-simple.exe` once with Visual Studio (x64, with the x64 and ARM64 helpers, used by both installers; the arm64 setup needs Windows 11; downloads a pinned libwdi and the WDK 8.0 co-installer redistributable). See [../packaging/windows/README.md](../packaging/windows/README.md) |
| `scripts/package-linux.sh` | `marvincapture_<version>_<debarch>.deb` | Linux only, needs `dpkg-deb`; udev rules and maintainer scripts in `packaging/linux/` |
| `scripts/package-linux-docker.sh` | same `.deb` | runs `package-linux.sh` in `ubuntu:22.04` (`--arch amd64\|arm64\|all`), so the package's `libc6` dependency is 22.04's glibc; works from macOS with Docker/OrbStack |

`scripts/publish-release.sh [--draft] [--title T] [--notes N] <dir>` creates the GitHub
release `v<version>` from the installers in `<dir>` (with the `gh` CLI). The GitHub Actions
workflow `.github/workflows/build.yml` runs these scripts for all six desktop platforms
(and `build-android.sh` for the APK) and can publish the result as a (draft) release; see
[ci.md](ci.md).

## Tests

`tests/` has three kinds:

- `tests/engine/`, `tests/test_lock.c`, `tests/sinks/test_analog_sinks.c`: plain
  unit tests.
- `tests/engine_replay/` and the rest of `tests/sinks/`: drive the whole session
  engine through `pin_api.h` against the **replay device** (`PIN_REPLAY=<file>`,
  a virtual device that plays a recording back), covering DV/HDV formats, scene
  splitting, multi-pass, preview frames and output checks.
- `tests/replay_reassembler.c` (`reassembler_ep88_*`): the DV reassembler must turn a
  raw EP 0x88 dump back into the DV it carries, byte for byte.

The recordings these tests run on are not committed: ctest makes them at test time
(about a second, into `build/<os>-<arch>/core/tests/data/`) with the ffmpeg
command-line tool and `tests/make_ep88.c`, which wraps DV into the device's EP 0x88
framing. Configure finds ffmpeg on PATH and warns when there is none; then the tests
that need the recordings skip themselves, except when the environment variable `CI` is
set: then they fail. See [../tests/data/README.md](../tests/data/README.md), also for
the one optional camera recording (12-bit DV audio) that ffmpeg cannot make.

## Repository layout

```
src/core/      hardware layer: USB device, 1394 link, streams, DV/HDV reassembly,
               analog, config channel, device lock, AVI writer
src/engine/    session engine: state machine, deck, preview, scene split, settings
src/sinks/     file writers (raw, AVI, MOV/MKV rewrap, FFV1)
src/api/       pin_api.h / pin_api.c, the flat C API the GUI links
src/cli/       MarvinCaptureCLI
gui/windows/   WinUI 3 app
gui/macos/     SwiftUI/AppKit app (SwiftPM)
tests/         unit and replay tests (their recordings are made at test time, see tests/data/)
firmware/      FPGA bitstreams (see firmware/README.md)
scripts/       build.ps1 (Windows), build.sh (macOS/Linux), build-ffmpeg.sh,
               package-*.sh/.ps1 (installers), extract-bitstreams.py,
               driver-status.ps1
packaging/     installer resources (macos/, windows/, linux/)
third_party/   vendored FFmpeg, built from source
docs/          documentation
build/         all build output, one directory per platform (git-ignored)
```

The source is portable C11. Windows is the platform that is built and tested
regularly, with the GUI. On macOS (Apple Silicon) `scripts/build.sh` builds the
core, MarvinCaptureCLI and the GUI, and the whole ctest suite passes (not yet
tried against the device there). On Linux the
same CMake build passes ctest and the CLI has been run against the device. There
is no Linux GUI yet.
