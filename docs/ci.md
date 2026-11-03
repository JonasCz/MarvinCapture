# CI builds (GitHub Actions)

`.github/workflows/build.yml` builds the installers. It only runs when started by
hand: **Actions > Build > Run workflow** (on `main` or any branch).

The workflow runs the same scripts as a local build, so a local run does the same
thing; the YAML only picks the runners, restores and saves the caches, and passes the
installers on:

| Step | macOS / Linux | Windows | Android |
|---|---|---|---|
| Dependencies | `scripts/install-deps.sh` | `scripts\install-deps.ps1 -Arch <x64\|arm64> -Msys2 <root> -Refresh` (after `setup-msys2` installed a bare MSYS2, and `setup-dotnet`) | none (preinstalled on the runner) |
| Build + installer | `scripts/package-macos.sh` / `scripts/package-linux.sh` `[--skip-tests]` | `scripts\package-windows.ps1 -Arch <x64\|arm64> -Msys2 <root> [-SkipTests]` | `scripts/build-android.sh --release --abi arm64-v8a` |
| Release | `scripts/publish-release.sh [--draft] [--title T] [--notes N] <dir>` (and `--check` before building) | | |

## Options

| Input | Default | |
|---|---|---|
| Windows x86_64 / arm64, macOS arm64 / x86_64, Linux x86_64 / arm64, Android arm64 | all on | one checkbox per platform; unticked ones don't start a runner |
| Run the ctest suite | off | `--skip-tests` / `-SkipTests` otherwise (not for Android: the core's tests cannot run on an Android target there) |
| What to do with the installers | artifacts only | `artifacts only`, `draft release` or `release` |
| Release title | empty | `MarvinCapture <version>` when empty |
| Release notes | empty | `"release_notes"` from `VERSION` when empty |

**Artifacts only**: each platform's installer is an artifact of the run (run page,
bottom; kept 30 days), one zip per platform.

**Draft release** / **release**: after all selected platforms built,
`scripts/publish-release.sh` creates the release `v<version>` (`"version"` from
`VERSION`) on the run's commit with the installers (`.exe`, `.pkg`, `.deb`, the
Android `.apk`) and a `SHA256SUMS.txt` over them. A draft is only visible to maintainers until it
is published on the Releases page, which is also the place for longer notes than the
one-line input (the notes are Markdown). The run stops before building if a release
for that version already exists (drafts included) or the tag is on another commit:
bump `VERSION` first, as described in [building.md](building.md#installers); and, with
Android ticked, if the [Android signing key](#android-signing-key) is not set up
(`publish-release.sh` itself also refuses a debug-signed APK). Publish from a run with
all seven platforms: the release only holds the installers of the platforms the run built.

## Runners

Every platform is built on its own GitHub-hosted runner, all in parallel; natively,
except Android (cross-built on Linux with the NDK):

| Platform | Runner | Output |
|---|---|---|
| windows-x86_64 | `windows-2025` | `MarvinCapture-<v>-windows-x86_64-setup.exe` |
| windows-arm64 | `windows-11-arm` | `MarvinCapture-<v>-windows-arm64-setup.exe` |
| macos-arm64 | `macos-15` | `MarvinCapture-<v>-macos-arm64.pkg` |
| macos-x86_64 | `macos-15-intel` | `MarvinCapture-<v>-macos-x86_64.pkg` |
| linux-x86_64 | `ubuntu-22.04` | `marvincapture_<v>_amd64.deb` |
| linux-arm64 | `ubuntu-22.04-arm` | `marvincapture_<v>_arm64.deb` |
| android-arm64 | `ubuntu-24.04` | `MarvinCapture-<v>-android.apk` (arm64-v8a) |

Linux builds on 22.04 so the `.deb`'s glibc requirement stays at 22.04's (the same
reason `scripts/package-linux-docker.sh` uses `ubuntu:22.04`).

Android is cross-built on `ubuntu-24.04` because that image already has everything the
build needs: JDK 17 (the default `java`), the SDK in `ANDROID_HOME` with
`platforms;android-36` and `build-tools;36.0.0`, the NDK version pinned in
`scripts/android-ndk.sh` (under `$ANDROID_HOME/ndk/`), cmake, ninja and pkg-config. So the
job installs nothing (no `sdkmanager`, no `setup-java`, no emulator) and only runs
`scripts/build-android.sh`. It clears the runner's `ANDROID_NDK_HOME`, which points at the
image's default NDK, so only the pinned NDK can be used (the build fails if the image
ever drops it: then update `PIN_NDK_VERSION` in `scripts/android-ndk.sh`). Only
`arm64-v8a`, the phones; Gradle and the Android Gradle plugin come from the wrapper and
Google's Maven repository (cached, below).

No cross-compiling for the desktop platforms: once FFmpeg is cached, a platform's build
is a few minutes, mostly setup (MSYS2, .NET, Homebrew) that a cross build would need as
well, and the parallel runners finish at about the same time as the slowest one. Cross-compiling would also
need a second libusb per platform (Homebrew's is the host's architecture only), FFmpeg
cross configure flags, and could not run the tests for the other architecture.

The arm64 and Intel-Mac builds are made by CI only and have not been tried against
the device. Both Windows installers carry the same `wdi-simple.exe` (libwdi, built with
Visual Studio, which the Windows runners have): it holds libwdi's x64 and ARM64
helpers, so the WinUSB driver install works on Windows on Arm too.

Measured (October 2026), whole job with the caches filled: Linux 0.5 min, macOS arm64
2 min, macOS x86_64 4.5 min, Windows x86_64 4 min, Windows arm64 about 5 min; Android
0.6 min (November 2026: about 25 s for the core, the JNI layer and Gradle; 2.3 min with
nothing cached, of which FFmpeg is under a minute and Gradle's first downloads about one). A cold
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
| libwdi `wdi-simple.exe` (both Windows jobs) | hash of `scripts/package-windows.ps1` | the libwdi MSBuild build and the WDK download |
| Android FFmpeg, `third_party/ffmpeg-android-arm64` | hash of `scripts/build-ffmpeg.sh` and `scripts/android-ndk.sh` (what its stamp covers) | the NDK build of FFmpeg |
| Android libusb, `third_party/libusb-android-arm64` | hash of `scripts/build-android-native.sh` and `scripts/android-ndk.sh`; falls back to an older entry | the libusb build |
| Gradle, `~/.gradle/wrapper` and `~/.gradle/caches` (not the wrapper's zip, not the build cache) | hash of `gui/android/gradle/wrapper/gradle-wrapper.properties`, `gui/android/**/*.gradle` and `gui/android/gradle.properties`; falls back to an older entry | the Gradle distribution and the Android Gradle plugin's downloads |

The build scripts check the cached FFmpeg, libusb and libwdi themselves (their stamp
files), so a stale cache is rebuilt rather than used (which is why the libusb cache may
fall back to an older entry). Changing `scripts/build-ffmpeg.sh` rebuilds
FFmpeg on every platform once. Caches unused for 7 days are dropped by GitHub, so the
first run after a pause takes longer. The C core (about 30k lines) and the Swift GUI
are rebuilt every time: they take well under a minute or two, and a restored SwiftPM
build tree is older than the fresh checkout, so it would be rebuilt anyway.

## Android signing key

Android installs an update only over an app signed with the same key, so the key the
first published APK is signed with is the app's key for good: a lost key means no
updates for anyone who installed it (they would have to uninstall first), and a leaked
one lets anyone publish updates. The workflow signs the APK with the key in four
repository secrets; without them it signs with a throw-away debug key, warns ("Debug-signed
APK" on the run page), and refuses to make a (draft) release with Android ticked.

Once, on your own machine (needs a JDK for `keytool`):

```sh
keytool -genkeypair -v -keystore marvincapture-release.jks -storetype PKCS12 \
    -alias marvincapture -keyalg RSA -keysize 4096 -validity 36500 \
    -dname "CN=MarvinCapture"
# asks for the keystore password (PKCS12: the key has the same password)
base64 < marvincapture-release.jks | tr -d '\n' > marvincapture-release.jks.b64
```

Then on GitHub, **Settings > Secrets and variables > Actions > New repository secret**
(or `gh secret set NAME`):

| Secret | Value |
|---|---|
| `MARVIN_ANDROID_KEYSTORE_BASE64` | the contents of `marvincapture-release.jks.b64` (`gh secret set MARVIN_ANDROID_KEYSTORE_BASE64 < marvincapture-release.jks.b64`) |
| `MARVIN_ANDROID_KEYSTORE_PASSWORD` | the keystore password |
| `MARVIN_ANDROID_KEY_ALIAS` | `marvincapture` |
| `MARVIN_ANDROID_KEY_PASSWORD` | optional: the key's password, when it differs from the keystore's (not with PKCS12) |

The job decodes the keystore into the runner's temporary folder and passes it to
`scripts/build-android.sh` as `MARVIN_ANDROID_KEYSTORE` (with the other three under their
own names; see [the Android README](../gui/android/README.md#signing)).

**Keep a backup** of `marvincapture-release.jks` and its password somewhere safe outside
GitHub (a password manager, an offline copy): secrets cannot be read back from GitHub.
Delete the `.b64` file. Local release builds use the same key through the same
environment variables.
