# CI builds (GitHub Actions)

`.github/workflows/build.yml` builds the installers. It only runs when started by
hand: **Actions > Build > Run workflow** (on `main` or any branch).

The workflow runs the same scripts as a local build, so a local run does the same
thing; the YAML only picks the runners, restores and saves the caches, and passes the
installers on:

| Step | macOS / Linux | Windows |
|---|---|---|
| Dependencies | `scripts/install-deps.sh` | `scripts\install-deps.ps1 -Arch <x64\|arm64> -Msys2 <root> -Refresh` (after `setup-msys2` installed a bare MSYS2, and `setup-dotnet`) |
| Build + installer | `scripts/package-macos.sh` / `scripts/package-linux.sh` `[--skip-tests]` | `scripts\package-windows.ps1 -Arch <x64\|arm64> -Msys2 <root> [-SkipTests]` |
| Release | `scripts/publish-release.sh [--draft] [--title T] [--notes N] <dir>` (and `--check` before building) | |

## Options

| Input | Default | |
|---|---|---|
| Windows x86_64 / arm64, macOS arm64 / x86_64, Linux x86_64 / arm64 | all on | one checkbox per platform; unticked ones don't start a runner |
| Run the ctest suite | off | `--skip-tests` / `-SkipTests` otherwise |
| What to do with the installers | artifacts only | `artifacts only`, `draft release` or `release` |
| Release title | empty | `MarvinCapture <version>` when empty |
| Release notes | empty | `"release_notes"` from `VERSION` when empty |

**Artifacts only**: each platform's installer is an artifact of the run (run page,
bottom; kept 30 days), one zip per platform.

**Draft release** / **release**: after all selected platforms built,
`scripts/publish-release.sh` creates the release `v<version>` (`"version"` from
`VERSION`) on the run's commit with the installers and a `SHA256SUMS.txt`. A draft is only visible to maintainers until it
is published on the Releases page, which is also the place for longer notes than the
one-line input (the notes are Markdown). The run stops before building if a release
for that version already exists (drafts included) or the tag is on another commit:
bump `VERSION` first, as described in [building.md](building.md#installers). Publish
from a run with all six platforms; with fewer, the run warns.

## Runners

Every platform is built natively on its own GitHub-hosted runner, all in parallel:

| Platform | Runner | Output |
|---|---|---|
| windows-x86_64 | `windows-2025` | `MarvinCapture-<v>-windows-x86_64-setup.exe` |
| windows-arm64 | `windows-11-arm` | `MarvinCapture-<v>-windows-arm64-setup.exe` |
| macos-arm64 | `macos-15` | `MarvinCapture-<v>-macos-arm64.pkg` |
| macos-x86_64 | `macos-15-intel` | `MarvinCapture-<v>-macos-x86_64.pkg` |
| linux-x86_64 | `ubuntu-22.04` | `marvincapture_<v>_amd64.deb` |
| linux-arm64 | `ubuntu-22.04-arm` | `marvincapture_<v>_arm64.deb` |

Linux builds on 22.04 so the `.deb`'s glibc requirement stays at 22.04's (the same
reason `scripts/package-linux-docker.sh` uses `ubuntu:22.04`).

No cross-compiling: once FFmpeg is cached, a platform's build is a few minutes, mostly
setup (MSYS2, .NET, Homebrew) that a cross build would need as well, and the parallel
runners finish at about the same time as the slowest one. Cross-compiling would also
need a second libusb per platform (Homebrew's is the host's architecture only), FFmpeg
cross configure flags, and could not run the tests for the other architecture.

The arm64 and Intel-Mac builds are made by CI only and have not been tried against
the device. The Windows arm64 installer has **no driver section**: libwdi (which
installs the WinUSB driver on x64) only builds for ARM64 with its MSVC solution, not
with MSYS2. On Windows on Arm, install WinUSB for the device with Zadig
([windows-driver.md](windows-driver.md)).

Measured (October 2026), whole job with the caches filled: Linux 0.5 min, macOS arm64
2 min, macOS x86_64 4.5 min, Windows x86_64 4 min, Windows arm64 about 5 min. A cold
FFmpeg build adds about 1 min on Linux/macOS, 2.5 min on Windows x86_64 and 4 min on
Windows arm64; on Windows nearly all of that is FFmpeg's `configure` (thousands of small
compiler runs through the MSYS2 shell, whose bash and make run emulated on Arm), the
compile itself is under a minute.

## Caches

| Cache | Key | Saves |
|---|---|---|
| FFmpeg static build, `third_party/ffmpeg-<platform>` | platform + hash of `scripts/build-ffmpeg.sh` | the FFmpeg build (the slowest step; several minutes on Windows) |
| FFmpeg source tarball | FFmpeg version, shared by all platforms | the download, only needed when a platform's build cache misses |
| MSYS2 packages (pacman's download cache) | platform + hash of `scripts/install-deps.ps1` | the pacman downloads |
| NuGet packages | hash of the csproj/props/nuget.config + architecture; falls back to the other Windows architecture's | the package restore (the Windows App SDK packages hold both architectures) |
| libwdi `wdi-simple.exe` (Windows x64) | hash of `scripts/package-windows.ps1` | the libwdi autotools build and the WDK download |

The build scripts check the cached FFmpeg and libwdi themselves (their stamp files), so a
stale cache is rebuilt rather than used. Changing `scripts/build-ffmpeg.sh` rebuilds
FFmpeg on every platform once. Caches unused for 7 days are dropped by GitHub, so the
first run after a pause takes longer. The C core (about 30k lines) and the Swift GUI
are rebuilt every time: they take well under a minute or two, and a restored SwiftPM
build tree is older than the fresh checkout, so it would be rebuilt anyway.
