# MarvinCapture (Android)

Phone front-end for the Pinnacle Studio 500-USB open driver, the Android
counterpart of the [Windows](../windows/README.md) and [macOS](../macos/README.md)
apps. Like them it is a thin shell over the `marvin-core` C library: every status
text, enable rule, format list and option, file name and setting comes from the core
through `src/api/pin_api.h`. The app holds the widgets, the preview renderer, the
audio monitor, the USB plumbing, the capture service and its notification.

- Android 11 (API 30) or later, target and compile SDK 36; arm64-v8a (phones), x86_64
  (emulator, replay device only)
- Java, framework widgets only (`android.widget`, `Theme.DeviceDefault`): no AndroidX,
  no Material Components, no Kotlin, no dependencies at all
- Native: the core (`libmarvin-core.so`, FFmpeg and libusb linked in) and a JNI layer
  (`libmarvin-jni.so`: the `pin_api.h` wrappers, the OpenGL ES 3 preview, the AAudio
  monitor), both prebuilt by `scripts/build-android-native.sh`; Gradle has no CMake or
  NDK step
- Distributed as an APK on GitHub releases, FPGA bitstreams bundled (no Google Play
  build; see [the plan](../../docs/handoff/android-plan.md#google-play) for why)

## Build

```sh
scripts/build-android.sh                          # release APK, arm64-v8a
scripts/build-android.sh --debug --abi x86_64     # debug APK for the emulator
scripts/build-android.sh --abi arm64-v8a --abi x86_64 --skip-native   # both ABIs, natives as built
```

The script runs `scripts/build-android-native.sh` for the ABIs (FFmpeg and libusb the
first time, then the core and the JNI layer into `build/android-<abi>/dist`), then
`gui/android/gradlew assembleRelease|assembleDebug -PmarvinAbis=...`, and copies the
result to

```
build/android/MarvinCapture-<version>-android.apk          (--release)
build/android/MarvinCapture-<version>-android-debug.apk    (--debug)
```

checks the signature (`apksigner`) and the 16 KB alignment of the native libraries
(`zipalign -P 16`), and prints the size per part. `<version>` is `"version"` from the
top-level `VERSION` file; the APK's `versionName` is the same and `versionCode` is
derived from it (1.2.3 → 1020300).

Requirements: everything `build-android-native.sh` needs ([docs/building.md](../../docs/building.md#android):
NDK 29.0.14206865, cmake, ninja, ...), a JDK 17 or newer, and the Android SDK with
`platforms;android-36` and `build-tools;36.0.0` in `ANDROID_HOME` (the script tries
`/opt/android-sdk` when it is unset). Gradle 8.14.3 comes from the wrapper
(`gradlew`, checksum pinned), the Android Gradle plugin 8.13.2 from Google's Maven
repository; Maven Central is addressed as `repo1.maven.org`, because some proxies
rate-limit its usual name. `local.properties` (the SDK path for Gradle) is not
committed; `ANDROID_HOME` does the same.

### Signing

A release APK is signed with the key named by these environment variables; without
them it gets the debug key and both Gradle and the script warn:

| Variable | |
|---|---|
| `MARVIN_ANDROID_KEYSTORE` | path of the keystore (`.jks` or `.p12`) |
| `MARVIN_ANDROID_KEYSTORE_PASSWORD` | its password |
| `MARVIN_ANDROID_KEY_ALIAS` | the key's alias |
| `MARVIN_ANDROID_KEY_PASSWORD` | the key's password (default: the keystore password) |

Keep one key for good: Android installs an update only over an APK with the same
signature, and developer verification will tie the package name
(`jonascz.marvincapture`) to it.

### Gradle only

With the native libraries already in `build/android-<abi>/dist`:

```sh
cd gui/android
./gradlew assembleDebug -PmarvinAbis=x86_64     # app/build/outputs/apk/debug/app-debug.apk
./gradlew lintDebug
```

Without `-PmarvinAbis` every ABI that has both libraries in `build/` is packaged. The
`stageNativeLibs` and `stageAssets` tasks copy the two `.so` files and the FPGA
bitstreams (`firmware/fpga-*.bin` and its README; not `fx2-marvin.bin`, which only the
MovieBox Deluxe needs) into `app/build/marvin/`.

## Size

The release APK for arm64-v8a is about 2.9 MB (5.9 MB with x86_64 as well):

| Part | In the APK |
|---|---|
| `libmarvin-core.so` (arm64) | 2.66 MB (stored uncompressed and page aligned, so it loads in place) |
| `libmarvin-jni.so` (arm64) | 0.04 MB |
| `classes.dex` (R8) | 0.08 MB |
| firmware assets | 0.07 MB (0.16 MB unpacked) |
| resources, manifest, signature | 0.01 MB |

## Run

Plugging the box in (USB-C OTG adapter) offers to open the app; ticking "always" makes
that the default and grants the USB permission. Started from the launcher, the app
asks for the permission itself. Captures go to `Movies/MarvinCapture/` on the chosen storage (options: Save to) as `<name>-0001.<ext>`, `-0002`, ... (`pin_next_file_number`); the name is set
in the options and is also written into each file as its title where the file's format
has one (the core's help under the name says which of the selected formats do: not raw
`.dv` or `.m2t`).

### Replay device (demo, emulator)

The emulator has no USB, and the app can be shown without the hardware: the `replay`
extra opens the core's replay device (`pin_set_replay_file`, a DV or HDV recording
played back in real time, looping while no capture runs; a capture ends at the end of
the file) instead of USB:

```sh
adb push tape.dv /sdcard/Android/data/jonascz.marvincapture/files/
adb shell am start -n jonascz.marvincapture/.MainActivity --es replay tape.dv
```

A relative path is taken from the app's external files directory (readable without a
permission); an absolute path works if the app may read it. `tests/data/dv-pal.dv`
(2 s, see [tests/data/README.md](../../tests/data/README.md)) concatenated a few dozen
times makes a usable demo tape. The replay device only has the DV / HDV input: an
analog input gives the core's error. Logs: `adb logcat -s MarvinCapture marvin-core`
(the core's log lines and events go to the `marvin-core` tag).

## The screen

One screen, camera-app style (`MainLayout`), adapting to the window's size and shape. The picture
is always left aligned (landscape) or top aligned (portrait) at its own aspect, and letterboxes
inside its box (`pin_fit_rect`). Rotation, folding and window resizes are handled in place
(`configChanges`), so nothing is reopened. The window is edge to edge (enforced from API 35); the
system bar and cutout insets are padding.

| Window | Layout |
| --- | --- |
| Portrait | preview on top, as tall as the aspect allows at full width; the controls below |
| Landscape, free space right of the picture >= 300 dp (a 21:9 phone) | controls docked in the free space |
| Landscape, less than that (a 16:9 / 16:10 phone) | the controls keep 300 dp and **overlay** the right end of the picture ("overlay" mode): translucent over a live picture; once a capture runs they fade out completely (320 ms) except the audio / library / record-stop / options buttons, which sit on round backdrops; a tap on the picture shows the panel for 5 s (also shown at once for an error). The picture does not move. |
| Tablet / unfolded foldable: landscape >= 840 x 600 dp, portrait >= 600 x 900 dp | "dual" mode: the controls **and** the options are on screen all the time (landscape: one 380 dp column right of the picture, portrait: two columns under it); no gear |

The no-video card (the animated Marvin) is centred in the space the panel leaves free, in every mode.

On phones (smallest width < 600 dp) the picture goes **behind the status and navigation bars**, as the panel
already does; a fade in the theme's panel colour at its top and bottom keeps the bar icons legible over any
picture. On a tablet (a persistent taskbar) it stays between the bars.

- **Preview**: the picture, or the card with the animated Marvin (a Canvas port of
  `docs/logo.html`; still when animations are off: "Remove animations" or an animator
  duration scale of 0) and the core's sentence why there is no picture (`detail`,
  `error_text`, `pin_no_devices_hint`), the preparing progress and the no-signal
  countdown. A tap on the card retries a session in the error state. A "REC" badge
  with the capture time while capturing.
- **Status**: `pin_format_status_short` (file name while capturing, the stop sentence
  afterwards), `pin_format_signal` with the timecode (DV / HDV) or capture time
  (analog), `pin_format_frames` (tap: `pin_format_frames_detail`), `pin_format_sizes`
  and `pin_format_storage_free` (tap: `pin_format_storage_detail`; amber when
  `disk_low`), the L / R level meters (peak with decay, 10 s hold).
- **Record button**: the manual capture, enabled by `pin_manual_capture_allowed`
  (`pin_manual_capture_block_text` under it when not); while capturing the stop button
  (`pin_capture_action_allowed(STOP)`), grey while the file is finalised. Before a
  capture the core checks the output (`pin_check_output`) and the app asks what it
  reports (destination problem, low space, overwrite).
- **Audio card**: the L / R level meters (the theme's blue, yellow and red only when hot) with the speaker at
  their right: the audio monitor, muted by default (`gui.muted`).
- **Button rows**: the deck row (when shown) and the button row below it share four equal columns, so the small
  buttons (46 dp) line up: settings under the first, capture (the big one) between the middle two, library under
  the last. The library button (a stack of two videos) has a count badge (the captures in the chosen place); it is disabled while capturing.
- **Deck controls** (option "Use deck controls", `gui.deck_controls`, off by default; DV input only): a row
  above the buttons with rewind, play, stop and fast forward (`pin_deck`, enabled by `pin_deck_cmd_allowed`,
  lit with the deck's state), and the capture button becomes "rewind and capture"
  (`start_deck` + `rewind_first`, enabled by `pin_capture_action_allowed(START_AUTO)`); stopping it stops the
  tape too. Holding the capture button records what arrives without rewinding (a manual capture; it needs a
  signal, `pin_manual_capture_allowed`). Without the option the button is always a manual capture.
- **Gear**: the options, in place of the panel (the preview stays visible for the
  picture controls); Back closes them. On a tablet the options are always shown.

## Storage and library

**Save to** (first section of the options, a drop-down when there is more than one place):
`Storage` lists the mounted volumes of `StorageManager` (the phone, an SD card, a USB drive).
Captures go to `Movies/MarvinCapture` on the chosen volume, if the app may create files there
(`Storage` tries to create one; it needs no permission on the phone's own storage, and on removable
media it depends on the Android version and the card); if it may not, to the app's own folder on that
volume (`Android/data/jonascz.marvincapture/files/Movies`, always writable, deleted with the app) and the
options say so. A volume that is gone falls back to the phone's storage. Setting: `gui.storage` (the
volume's id).

**Library** (button next to the record button; slides up like the options): the files in the chosen place, newest first, as a grid
of thumbnails (`MediaMetadataRetriever`, made on two worker threads and cached) with size, length and
date; a tap plays (`VideoView` + `MediaController`), a long press offers to delete. Only the app's own
folder content is listed, because that is all it can see. Playback and thumbnails use **Android's**
media stack, so they work for what the phone decodes: H.264 / HEVC in Matroska (the phone encoder
formats). DV, HDV, FFV1 and uncompressed AVI files get a placeholder with their extension and the
player reports that the phone cannot play them. The core's FFmpeg build does contain decoders for DV,
MPEG-2, FFV1, PCM and MP2 (for the preview), but no H.264 / HEVC decoder and no libswscale, so a player
of its own would need a new native decode/convert/audio path; not done.

## Options

`OptionsPanel`, built on first use. First entry: the input (a segmented control, locked while capturing); a
launcher shortcut can set it too. All segmented controls and cards share one corner radius (10 dp). Analog: TV standard, file name, format,
aspect, stop after no signal / total time, picture and audio-gain sliders
(`pin_get_control` / `pin_set_control`, live during a capture, hue enabled by the
core); DV and HDV (shown for the DV input, which detects which one arrives): the "Show settings for"
DV / HDV tab (like the desktop apps) at the top of the section, then what both share (the deck controls
option, the file name), then the selected kind's format, aspect, scene split (where the format supports it), the two
stops. The aspect setting is disabled while its format stores no aspect (`Core.FMT_ASPECT`
not set: raw `.dv`, HDV `.m2t`), with the core's reason under it (`pin_format_aspect_help`);
the choice is kept, but Auto goes to the capture and the preview (`Settings.effectiveAspect`).
Scene split is on and disabled for a format that always splits (`Core.FMT_SPLIT_FORCED`: HDV
MOV / MKV, which the core does not offer on Android, as they decode every GOP during the
capture), with the core's text under it (`pin_format_scene_split_help`); the saved choice is
kept for the other formats.
Each file of a capture gets its own kind's aspect: `OPT_ASPECT` from the analog or DV
setting, `OPT_ASPECT_HDV` (`aspect_hdv`) from the HDV one. The help under the file name
comes from the core too (`pin_format_title_help`) and follows the format choices. Each format shows the core's warning (`pin_format_warning`) and its own settings
from `pin_format_options` (H.264 / HEVC: interlaced video as "Deinterlace" or "Encode as is" (the core's
keep-fields mode, with its help in amber and "OK to use if your camera is set to progressive");
quality as a bare 0 to 35 slider (the core accepts 0 to 51; no estimate or scale text, CRF sizes depend on the content);
FFV1: compression). H.264 / HEVC are listed only where `pin_encoders_probe` (run once
at start-up on a worker thread) found the encoder. Then "Screen" (keep the screen on during a
capture, all inputs), "Performance (analog)" (analog input only, both off by default: keep a CPU core busy
(`pin_set_cpu_keep_busy`: on = AUTO, which on Android means always; off = OFF); silent low-latency audio, below),
about and licences. The options slide up over the controls (a quarter of a second, scaled by the system's
animation setting) and stay open when a setting changes the picture's shape (the layout may then switch between docked and overlay). Everything but the picture controls is locked while
`pin_state_is_capturing`.

### Update check

The one thing the app uses the network for (the `INTERNET` permission). The core cannot download
the published `VERSION` file on Android (it loads libcurl, which Android lacks), so
`UpdateCheck.java` does only the HTTP request and the core decides everything else, the same code
the desktop apps use: `pin_update_url()` (the address; `MARVIN_UPDATE_URL` has no effect here),
`pin_update_due(now)` (at most one check a week), `pin_update_check_json()` (parse, compare with
`pin_app_version()`, remember). It starts from `MainActivity.onCreate`, once per process, the
download runs on a worker thread and the result goes back to the main thread. A failed download
shows nothing and is tried again at the next start (only a successful check counts as "checked").

The options panel shows "Update available" with the core's notice (`pin_update_known()`, the same
text as the desktop banner), the release notes and a Download button (opens the release page) at the top for as long as
the remembered version is newer than the running one. It is remembered in the core's settings:
`update.last_check` (unix seconds), `update.latest`, `update.download_url`, `update.release_notes` (newlines
stored as `\n`; the core encodes and decodes them). These belong to the core, not to `Settings.java`. The
panel shows the release notes ("What's new") under the notice, also when no check ran in this start.

### Performance options

Android apps cannot open `/dev/cpu_dma_latency`, the remedy for analog frame drops on
Linux. Two substitutes, both off by default (shown for the analog input only; try them if analog frames drop):

- the core's keep-busy thread (one core busy at idle priority while the analog read
  loop runs);
- the audio monitor's own AAudio output stream, opened in `AAUDIO_PERFORMANCE_MODE_LOW_LATENCY`
  while an analog capture runs, playing silence while muted (`audioSetKeepAlive` in
  `audio.c`: one stream, reopened when the mode changes; DV and HDV never ask for it, their
  unmuted stream is the normal-latency one). The mechanism is AOSP's, not Pixel's: the audio
  HAL tells the power HAL that a low-latency stream is active (`Mode::AUDIO_STREAMING_LOW_LATENCY`;
  Qualcomm's audio HAL does it for every low-latency stream) and what the power HAL does with
  it is the vendor's: on Pixel it sets a PM QoS CPU-latency request (`/dev/cpu_dma_latency`).
  Other makers' behaviour is undocumented and not measured. Device dependent.

## Settings

Stored by the core (`pin_settings_get/set`, `settings.ini` in the app's files
directory), global (one device), cached in `Settings.java`: `gui.last_input`,
`gui.std`, `gui.control_<n>`, `gui.format_<kind>`, `gui.aspect_<kind>`,
`gui.split_dv|hdv`, `gui.idle_<kind>`, `gui.duration_<kind>` (kind = `analog`, `dv`,
`hdv`), `gui.name_analog`, `gui.name_dv` (DV and HDV share it),
`gui.deinterlace_analog|dv`, `gui.quality_analog|dv`, `gui.ffv1_compression`,
`gui.muted`, `gui.storage`, `gui.kind_tab`, `gui.keep_screen_on`, `gui.cpu_keep_busy`, `gui.audio_keepalive`. A format
that is no longer offered (no encoder) falls back to the kind's default.

## What lives where

```
app/build.gradle               versions, ABIs (-PmarvinAbis), staging of .so + firmware, signing
app/src/main/AndroidManifest.xml
app/src/main/cpp/              the JNI layer (built by scripts/build-android-native.sh)
    marvin_jni.c               pin_api.h wrappers; status as arrays + ready-made texts
    preview.c                  EGL + GLES 3 renderer, AChoreographer pacing
    audio.c                    AAudio monitor + low-latency keep-alive
app/src/main/java/jonascz/marvincapture/
    MarvinApp.java             start-up: libraries, pin_set_app_dirs, firmware copy, probe
    Core.java                  the natives and the mirrored enum values / array indices
    CaptureService.java        the session, USB connection, capture, notification, wake lock
    MainActivity.java          the screen, USB discovery and permission, capture flow
    OptionsPanel.java          the options
    MainLayout.java            preview, controls, options: portrait / landscape / overlay / tablet
    Storage.java               storage volumes, the output folder
    LibraryView.java           thumbnails + basic player
    MarvinIdleView.java        the no-video animation
    LevelMeterView.java, Glyphs.java (icons, record button), Ui.java (view helpers)
    Settings.java, Formats.java (format lists after the probe), Status.java, About.java
    UpdateCheck.java           the weekly update check: the download only, the core decides
app/src/main/res/              theme, launcher icon (the logo as a vector), notification
                               icon, device_filter.xml (vendor 0x2304 only)
```

### Startup

`MarvinApp.onCreate`: load `libmarvin-core.so` and `libmarvin-jni.so`, check
`pin_api_version()`, `pin_set_app_dirs(filesDir, cacheDir, cacheDir)` before any other
core call (no runtime dir: `pin_open` fails), copy `assets/firmware` to
`filesDir/firmware` once per installed APK, `pin_set_firmware_dir`, apply the keep-busy
setting, create the renderer and the audio monitor (process-wide native objects), start
the encoder probe thread.

### USB

`device_filter.xml` matches Pinnacle's vendor id (8964 = 0x2304) only, so the core's
model table stays the single source of truth: `pin_usb_model()` names the device and
`pin_device_fd_problem()` refuses the models that cannot be opened from a descriptor
yet (the MovieBox Deluxe; adding it later needs no app change). The activity takes the
first Pinnacle device `UsbManager` lists, asks for the permission when it was not
launched by the attach intent (`PendingIntent` with an explicit package, `FLAG_MUTABLE`)
and hands it to the service: `UsbManager.openDevice()` →
`pin_open("fd:" + connection.getFileDescriptor())`. The connection stays open until
`pin_close` returned. On `USB_DEVICE_DETACHED` the service waits until the core has ended
a running capture (it notices the loss itself and finalises, `PIN_STOP_DEVICE_LOST`),
then closes the session.

### Service and threads

- `CaptureService` owns the session. The activity binds to it from `onCreate` to
  `onDestroy`; while it is visible the service ticks at 5 Hz (`pin_get_status`, then
  `pin_poll_event` drained; both non-blocking), else at 1 Hz, and hands each snapshot to
  the activity. The service is the only reader of the status and the event queue (the
  audio peaks are "since the previous read").
- A capture: the activity checks and starts it (`pin_capture_start`), then
  `startForegroundService(ACTION_FOREGROUND)`; the service goes foreground (type
  `connectedDevice`; `mediaPlayback` for the replay device, which has no USB permission
  to show for the first) with the ongoing notification (state, file, sizes, frames, a
  chronometer, the countdown, Stop), takes a partial wake lock and, for analog, the
  keep-alive. All three stay while the file is finalised (`STOPPING`, or a
  `pin_close` that finalises) and end with it; then the service leaves the foreground
  and posts the result: a normal end as a notification, an abnormal one
  (`pin_stop_reason_abnormal`) as a dialog when the activity is visible, else as a
  high-priority notification. `POST_NOTIFICATIONS` is asked before the first capture.
- Close order (as on macOS): audio detach, preview detach (both synchronous: the
  real-time callback and the render thread are out of the core when they return),
  then `pin_close` on the "Core open/close" thread (it blocks until files are
  finalised), then the USB connection. Opens run on the same thread, after any close.
- Preview render thread (`preview.c`): blocks in `pin_preview_wait` while nothing
  arrives; with frames queued, AChoreographer frame callbacks pace
  `pin_preview_lock_due(frame time + one refresh)` (CLOCK_MONOTONIC = `pin_clock_now`).
  `previewSetSurface` (the SurfaceHolder callbacks) waits until the thread has dropped
  the old EGL surface. The activity turns the preview off while it is not visible
  (`pin_preview_enable(0)`); the audio monitor runs only while visible and unmuted.

## Tested, and what not

Tested on the x86_64 API 34 emulator (software rendering) with the replay device only:
start-up, the no-device screen, the replay preview (GLES renderer, aspect, meters,
timecode), portrait and landscape, the options (format lists after the probe, warnings,
H.264 settings, analog controls, locking), a DV capture to `Movies/MarvinCapture`
(numbering, sizes, end of tape, user stop, finalising), the foreground notification and
the result notification, the abnormal-end dialog, the audio monitor stream opening,
recovery from the error state, Back / relaunch. A DV to H.264 capture on the emulator's
software encoder stopped after 18 s with "the video encoder failed" (the core's 10 s
watchdog); probably the emulator's speed, to be checked on a phone.

**Not tested** (no hardware here): everything with the real box: the USB permission flow
and the attach intent, `fd:` opening, firmware upload, DV / HDV / analog capture from the
device, detach during a capture, the analog controls on a live input, the keep-busy
thread and the low-latency keep-alive on a phone, H.264 / HEVC with a hardware encoder,
the 16 KB page devices, Android 11-13 behaviour (only API 34 ran).

## Look

The system theme decides (`Theme.DeviceDefault.DayNight`; palette in `res/values/colors.xml`
and `values-night/`, read by `Ui.applyTheme`). The activity handles `uiMode` itself, so a theme
change rebuilds the views in place (the service, session and surface carry on). The accent
colours are set in the theme and the drop-down lists colour the chosen entry themselves,
because some devices' own dark accent is too dark to see.

Edge to edge: the preview frame reaches behind the status bar and the display cutout (the
picture and the REC badge stay inside the insets); the panel takes the insets of the sides it
touches as padding. Without a picture the card has the shape of the chosen aspect (16:9 is wider).

## Launcher shortcuts

Long-pressing the app icon offers DV / HDV, S-Video, Composite and Library (static shortcuts,
`res/xml/shortcuts.xml`, each with its own icon in `res/drawable/ic_shortcut_*.xml`: a FireWire socket, an
S-Video mini-DIN, a yellow RCA jack and a stack of videos). The inputs start `MainActivity` with an `input`
extra (0, 1, 2), which `applyInputExtra` applies like a tap on the selector (ignored while capturing); Library
starts it with a `library` extra, which opens the library.
