# Android GUI: feasibility and plan

Written 2026-11-02 from the code at `1ce9d79` as a plan; updated 2026-11-03: the core
changes and the app (phase 1 and most of phase 2) are built, tested on the emulator with
the replay device only, **not yet on a phone with the box**. See [Status](#status).
Facts about Android, FFmpeg, libusb and Google Play were checked against their
documentation on 2026-11-02 (sources at the end); anything not confirmed is marked
"unverified".

## Status

Built (2026-11-03):

- **Core**: open by file descriptor (`pin_open("fd:N")`), `pin_usb_model()`,
  `pin_device_fd_problem()`, `pin_set_app_dirs()`, the HDV remux temp file in the temp
  dir, `--device fd:N` in the CLI, Android texts, the H.264 / HEVC sink with
  `pin_encoders_probe()` / `pin_format_options()`; HDV MOV / MKV hidden on Android; the
  replay device reports capture time and bytes like the hardware loops.
- **Native build**: `scripts/build-android-native.sh` (FFmpeg, libusb 1.0.30, core, CLI,
  and the app's JNI layer), arm64-v8a and x86_64.
- **App** (`gui/android`, [README](../../gui/android/README.md)): Java, framework widgets,
  no dependencies, minSdk 30, target 36; `scripts/build-android.sh` makes the APK
  (2.9 MB for arm64-v8a). Everything under [Architecture](#architecture) and
  [Phone UI](#phone-ui): capture service (foreground `connectedDevice`, wake lock,
  notification with Stop, result notification / dialog), USB discovery and permission
  (vendor 0x2304 only, the model table decides), GLES 3 preview with AChoreographer
  pacing, AAudio monitor (muted by default), the options panel with the per-format
  settings and warnings, the analog controls, the performance options (keep-busy thread,
  low-latency audio keep-alive), the replay device for demos (`--es replay <file>`).
- **Tested** on the x86_64 API 34 emulator (software rendering, no KVM) with the replay
  device: start-up, no-device screen, preview, portrait / landscape, options, DV captures
  into `Movies/MarvinCapture` (numbering, end of tape, user stop, finalising, the
  notifications, the abnormal-end dialog), error state and recovery.

Not done or not tested:

- **Nothing has run against the real box on a phone**: USB permission / attach intent,
  `fd:` open and firmware upload from the app, DV / HDV / analog captures, unplugging
  during a capture, the analog controls, thermal behaviour. Phase 0 (measuring analog on
  phones, with and without the keep-busy thread and the audio keep-alive) is still open.
- H.264 from DV on the emulator's software encoder stopped after 18 s ("the encoder took
  no picture for 10 s"); probably the emulator's speed, to be tried on a phone.
- Not in the app yet: SD card location, SAF, `progress_percent` during finalising
  (indeterminate bar), thermal / battery warnings, `Notification.ProgressStyle`, a
  monochrome (themed) launcher icon, a CI job (the script is ready for one, see
  [building.md](../building.md#android)).
- Only API 34 ran; Android 11-13 and 15-16 and 16 KB page devices are untested.

## Verdict

Feasible, and a good fit for the core's design. Android is Linux underneath (usbfs,
pthreads, POSIX files), the core is plain C11 with a polled, callback-free API
(`src/api/pin_api.h`), and every status text, enable rule, file name and setting
already comes from the core. A phone front end is the same kind of thin shell as
the macOS app (~6,800 lines of Swift), plus a small JNI layer.

What does not carry over as is:

| Area | Problem on Android | Fix |
|---|---|---|
| USB access | Apps cannot enumerate `/dev/bus/usb`; `libusb_get_device_list()` finds nothing without root | The app gets an fd from `UsbManager`; the core wraps it with `libusb_wrap_sys_device()` (core change, small) |
| Directories | Settings (`$XDG_CONFIG_HOME`/`$HOME`), lock (`$XDG_RUNTIME_DIR`/`$TMPDIR`), firmware next to the `.so` (inside the APK) | The app passes its own dirs; firmware copied from APK assets |
| Output files | Scoped storage | Direct paths into `Movies/MarvinCapture/` (API 30+), see [Storage](#storage) |
| CPU idle latency | `/dev/cpu_dma_latency` is not writable by apps; the fallback busy thread costs battery and heat | Measure first (phase 0); see [Risks](#risks) |
| Background | A long capture has to survive the screen turning off and app switching | Foreground service + wake lock, notification with Stop |
| Hotplug | `pin_devices_wait()` relies on libusb hotplug, unavailable | `USB_DEVICE_ATTACHED` / `DETACHED` broadcasts in the app |
| Update check | `pin_update_check()` dlopens libcurl, absent on Android (silently off) | Kotlin fetch for the GitHub build; none in a Play build |

## What the Termux report tells us

The CLI working in Termux shows that the engine, sinks and FFmpeg build run on an
Android kernel and toolchain. Unrooted apps cannot open `/dev/bus/usb/*` (SELinux gives
them read/write/ioctl on an fd but no `open`), so upstream libusb lists nothing there.
Termux gets around it: `termux-usb` has the Termux:API app open the device and passes the
fd on (as `argv[1]`, or `TERMUX_USB_FD` with `-E`), and Termux's libusb package carries a
patch that makes device listing return just that fd. So the test most likely ran unrooted
through `termux-usb -E`. If so, the core's bring-up, bulk and async transfers already work on
an fd that another app opened, which is exactly the app's situation, and the biggest unknown
is gone. Worth asking the tester: root or `termux-usb`, which phone, and whether analog was
tried (the timing-sensitive input).

Two kernel facts that matter: usbfs caps in-flight transfer buffers at 16 MB per process
(the DV queue is 256 x 16 KiB and the Linux analog queue 1024 x 4 KiB, 4 MiB each, so both
fit), and a device whose usbfs file is open never autosuspends.

## Architecture

```
MainActivity (UI, preview SurfaceView)  ──bind──▶  CaptureService (foreground, owns the session)
        │                                                   │
        └──────────── JNI: libmarvin-jni.so ────────────────┘
                          │  pin_api.h, preview renderer (EGL/GLES 3), audio monitor (AAudio)
                          ▼
                    libmarvin-core.so  (unchanged engine + sinks + static FFmpeg)
                          │
                    libusb (wrapped fd from UsbManager.openDevice())
```

- **CaptureService**: holds the `pin_session_t`, a `PARTIAL_WAKE_LOCK` while capturing or
  finalising (a foreground service alone does not keep the CPU awake with the screen off), and
  the ongoing notification (elapsed, size, errors, a Stop action; "Finalizing…" with progress
  after Stop). Type `connectedDevice`: the system's check is that the app holds USB permission
  for an attached device, and the type has no time limit (unlike `dataSync` and
  `mediaProcessing`, 6 h per day). Started from the record button (the app is in the foreground
  then, so the Android 12+ background-start limits don't apply). The finalise step after Stop
  (HDV remux, FFV1 flush) runs in the same service, which is what makes "remux in a background
  task with a notification" work without a separate job system.
- **MainActivity**: binds to the service; when it is not visible the preview is turned off
  (`pin_preview_enable(0)`), and with no capture running the service is not in the foreground,
  so Android may end the process; the next start reopens the device (warm start 0.3-1.7 s).
- **JNI layer** (C, est. 600-900 lines): thin wrappers over `pin_api.h`; the status snapshot goes
  to Kotlin as a handful of ints plus the ready-made strings (`pin_format_status_short`,
  `pin_format_signal`, `pin_format_frames`, `pin_format_sizes`, `pin_format_storage_free`, ...).
  The preview renderer and the audio monitor live here too, so no frame or sample crosses JNI.
- **Preview**: `SurfaceView` → `ANativeWindow` → EGL + GLES 3.0, three R8 textures (Y, Cb, Cr),
  the matrix from `pin_yuv_to_rgb_matrix()`, letterboxing from `pin_fit_rect()`. Paced with
  `AChoreographer` (API 29 has the 64-bit callback): its frame time is `CLOCK_MONOTONIC`, the
  same clock as `pin_clock_now()` on Linux, so `pin_preview_lock_due()` works as on macOS.
- **Audio monitor**: AAudio output stream (NDK, API 26+), data callback pulls
  `pin_monitor_read()`. Off by default on a phone (speaker next to the source's microphone).
- **USB permission**: `res/xml/device_filter.xml` (VID 0x2304 = 8964, the four supported PIDs)
  on the activity's `USB_DEVICE_ATTACHED` intent filter, so plugging the box in offers to open
  the app ("always" remembers it); `UsbManager.requestPermission()` otherwise. One device only:
  the first supported one `UsbManager.getDeviceList()` returns.

## Core changes (core-first rule)

All of these keep the desktop GUIs and the CLI working as before.

1. **Open by file descriptor.** `pin_open("fd:<n>", ...)` alongside the existing id / serial /
   path forms. `pinnacle_open_by_id()` gets a branch: `libusb_init_context()` with
   `LIBUSB_OPTION_NO_DEVICE_DISCOVERY` (libusb 1.0.27+; 1.0.30 is current and fixes an
   Android fd issue), `libusb_wrap_sys_device()`, PID from the descriptor, then the same
   set-configuration / claim as today. libusb does not close the fd; the app keeps its
   `UsbDeviceConnection` open until `pin_close()` returns. Port numbers are not available on a
   wrapped device, so hub depth is -1 (no hub warning). The cross-process lock keyed by "fd:N"
   is meaningless but harmless: usbfs itself refuses a second claim (BUSY), which is already
   reported as "in use". The MovieBox Deluxe re-enumerates after its FX2 download and the
   reopen path enumerates devices, so it stays unsupported on Android (untested anyway).
2. **Model lookup without enumeration**: `pin_usb_model(vid, pid, pin_device_info_t *out)`
   (name, supported, tested) so the app names the device from the same table. The
   `device_filter.xml` PID list is generated from, or tested against, `pinnacle_model.c`.
3. **CLI `--device fd:N`**: the same id through the existing `-d`, so the CLI built against
   upstream libusb can take `termux-usb`'s fd (`argv[1]` / `TERMUX_USB_FD`). That tests the
   app's exact open path in Termux before any app exists.
4. **Directories**: `pin_set_app_dirs(config, runtime, temp)` (or the app sets `XDG_CONFIG_HOME`
   and `TMPDIR` before the first core call, which works today with no change). Firmware: the APK
   carries `firmware/` as assets, copied to `filesDir` on first start or version change, then
   `pin_set_firmware_dir()`.
5. **HDV remux temp file**: `<name>.rawts.tmp` is written next to the output today. Android's
   media provider refuses to create it in `Movies/` (EPERM: `.tmp` is not a video type). Move it
   to the temp dir (configurable, default unchanged on desktop), or name it `....rawts.tmp.ts`.
6. **File numbering on shared storage**: in `Movies/` a directory listing shows only the app's
   own files, and after a reinstall the app no longer owns its old ones; creating a name the
   media database already has fails with EEXIST. `pin_next_file_number()` can then pick a
   number that is taken, so opening the output has to treat EEXIST as "taken, try the next
   number" instead of failing the capture.
7. **Finalize progress**: `progress_percent` during `PIN_STATE_STOPPING` (the HDV remux knows
   its read position in the temp file). Feeds the notification's progress bar, and the desktop
   GUIs' "Finalizing…" could show it too.
8. **Platform perf hints**: under `__ANDROID__`, `pin_perf_check()` must not report
   `PIN_PERF_CPU_LATENCY` (the user cannot fix it; apps cannot open `/dev/cpu_dma_latency`);
   whether the keep-busy thread stays on is decided by phase 0's measurements.
9. **Hardware encoder sink** (phase 3), see [H.264 / HEVC](#h264--hevc-hardware-encoding).

Build: `scripts/build-ffmpeg.sh` gains an Android mode (NDK clang, `--target-os=android`,
`--arch=aarch64`, cross compile); libusb is built from source with the NDK (it is not on the
system); the top-level `CMakeLists.txt` gets an option to skip the CLI/tests for the app build.
The NDK links with 16 KB page alignment by default from r28 (AGP 8.5.1+), which Play requires.
A `scripts/build-android.sh` + an Ubuntu CI job, following the "scripts do the work, YAML picks
runners" rule of [ci.md](../ci.md).

## Phone UI

One screen, built for one-handed use during a live recording:

```
Portrait                                 Landscape
┌──────────────────────────┐            ┌──────────────────────────────┬─────┐
│ ● REC 00:12:34   DV · PAL│            │                              │  ⚙  │
│                          │            │                              │     │
│        preview           │            │           preview            │ (⬇) │
│                          │            │                              │     │
├──────────────────────────┤            │ ● REC 00:12:34      DV · PAL │ ▭ ▭ │
│ tape-0007.dv             │            └──────────────────────────────┴─────┘
│ 1.2 GB · 18.3 GB free ·  │
│ 1 h 25 min left          │
│ Frames 18,512 · 0 err    │
│ L ▮▮▮▮▮▮▮▯▯  R ▮▮▮▮▮▮▯▯▯ │
│                          │
│  [input]   (⬇)    [⚙]    │
└──────────────────────────┘
```

- **Record button**: a large round button with a "save to device" glyph (arrow into a tray);
  while recording, a stop square. It is the manual capture (`PIN_CAPTURE_START_MANUAL`,
  enabled by `pin_manual_capture_allowed()`, i.e. only with a signal; the disabled reason
  from `pin_manual_capture_block_text()` shows under it).
- **Status** comes straight from the core's ready-made texts; the frame line opens
  `pin_format_frames_detail()`, the storage line `pin_format_storage_detail()`.
- **Options panel** (the gear; a slide-up sheet or a second screen, locked while capturing per
  `pin_state_is_capturing()`): input (DV/HDV, S-Video, Composite), TV standard, output format per
  kind, aspect, file name (also the title), scene split, stop after no signal, stop after a total
  time, output location, analog picture and audio-gain controls, audio monitor, keep screen on,
  dim the preview while recording, about / licences.
- **Dropped on the phone**: device picker, deck transport, automatic rewind & capture,
  multi-pass, "stop tape", command-line scripts.
- **Other states**: no device (an Android version of `pin_no_devices_hint()`: "Connect the box
  with a USB-C OTG adapter"), permission refused, preparing (core `detail` + progress), error
  with retry. An abnormal capture end (`pin_stop_reason_abnormal()`) is a dialog in the
  foreground and a high-priority notification in the background.

## Storage

The core is path based: `fopen` and FFmpeg's `file` protocol, `statvfs` for free space and
FAT32 detection, `readdir` for `pin_next_file_number()`, segment files for big AVIs. Keeping
plain paths keeps all of that.

| Location | How | Permission | Visible / survives uninstall |
|---|---|---|---|
| `Movies/MarvinCapture/` on internal shared storage (default) | direct path, API 30+ | none (API 30+); API 29: `requestLegacyExternalStorage` + `WRITE_EXTERNAL_STORAGE` | yes / yes |
| SD card, app-specific dir (`getExternalFilesDirs()`) | direct path | none | file managers: yes; deleted on uninstall |
| Any folder the user picks (SAF) | `content://` URIs only | per-folder grant | yes / yes |

Recommendation: the first two in v1. SAF needs an "open output file" hook in the core (fd in,
no directory listing, no `statvfs`), which is a larger change; later if asked for.

What the media provider allows in `Movies/` without permission: files whose extension maps to
a `video/*` type. `.dv`, `.ts`, `.m2t`, `.mkv`, `.avi`, `.mov`, `.mp4` all do; `.tmp` and
`.nut` don't (see core changes 5 and 6). `statvfs` passes through to the real file system, so
free space and FAT32 detection keep working. Sequential writes through the FUSE layer are
documented as comparable to MediaStore; FUSE passthrough (Android 12+, newer kernels only)
removes most of the overhead. Whether ~21 MB/s (uncompressed analog) is sustained on an
Android 11 phone is for phase 0 to measure.

## Formats on the phone

| Input | Offered | Notes |
|---|---|---|
| DV | `.dv` (default), DV AVI, DV MOV | All written live; closing writes only an index. DV is ~13 GB/h, CPU light. The best phone format. |
| HDV | `.ts`/`.m2t` (default) | Written live. HDV MOV / MKV remux the whole file at Stop (minutes, twice the space): hide them on Android, or keep them with the finalize notification (decision below). |
| Analog | FFV1/MKV, H.264/HEVC (phase 3) | FFV1 is ~30 GB/h and on the Mac ~60-75 % of a core at SD: measure on phones. Uncompressed AVI is ~75 GB/h: probably hidden on Android. |

## H.264 / HEVC hardware encoding

FFmpeg has wrappers for each platform's encoder, so one new sink serves all GUIs:

| Platform | FFmpeg encoder | Comes with the OS | Build change |
|---|---|---|---|
| Android | `h264_mediacodec`, `hevc_mediacodec` (FFmpeg 6.0+; NDK `AMediaCodec`, `libmediandk.so` loaded at run time) | yes | `--target-os=android --enable-jni --enable-mediacodec` + the encoders (configure insists on JNI; inside an app process that is fine) |
| macOS | `h264_videotoolbox`, `hevc_videotoolbox` | yes (VideoToolbox) | `--enable-videotoolbox` (the build uses `--disable-autodetect`) + encoders; LGPL-clean |
| Windows | `h264_mf`, `hevc_mf` (Media Foundation; the GPU vendor's MFT underneath) | yes | `--enable-mediafoundation` + encoders; hardware needs `hw_encoding=1` |
| Windows, alternatives | `h264_nvenc` / `h264_amf` (MIT headers, driver loaded at run time), `h264_qsv` (libvpl) | driver | headers / one library; not needed for a first version |
| Linux | VAAPI needs libva; no LGPL software H.264 encoder (x264 is GPL) | no | probably not offered |

On Android, H.264 encoding is mandatory on every phone (CDD), and there is a software
fallback in the platform (`c2.android.avc.encoder`, up to 12 Mbit/s, enough for SD). HEVC
encoding is optional and the platform's software HEVC encoder stops at 512 x 512, so HEVC is
offered only where a hardware encoder answers the probe. Encoders must accept YUV420Flexible
but only one of planar / semi-planar is guaranteed: the probe picks `yuv420p` or `nv12`.

The sink (`sink_hwenc.c`) would follow `sink_ffv1.c`: encoder thread behind a bounded queue,
audio on its own thread, the muxer behind a lock.

- **Input**: analog YUY2 → NV12 with field-aware 4:2:2→4:2:0 chroma (each field on its own, so
  colour doesn't bleed between fields). DV: decode with `dvvideo` (already built in for the
  preview) → NV12 (PAL 4:2:0 as is, NTSC 4:1:1 upsampled); audio as `sink_rewrap.c` extracts it.
  HDV: would need an MPEG-2 HD software decode first; not planned (the `.ts` is already compact).
- **Interlacing**: none of the three wrappers encodes interlaced (MediaCodec has no field
  mode at all, `h264_mf` forces progressive). Either encode the interlaced frame as is and flag
  the field order in the container (nothing is lost; players that honour the flag
  deinterlace, phone gallery players usually don't and show combing), or deinterlace to
  50p / 59.94p first with FFmpeg's `bwdif` or `yadif` (both LGPL; brings in a small part of
  libavfilter). Decision below.
- **Container**: MKV survives a crash or a dead battery mid-recording; MediaMuxer's MP4 has no
  fragmented mode and writes its index at stop. With FFmpeg's muxer: MKV, or fragmented MP4
  (`frag_keyframe`) for gallery compatibility. MediaCodec gives no DTS (FFmpeg's `pts_as_dts`
  covers it, no B-frames). **Audio**: AAC with FFmpeg's native `aac` encoder (LGPL; there is no
  MediaCodec AAC encoder in FFmpeg) for MP4; FLAC or PCM in MKV.
- **Availability**: the core opens the encoder once to probe it and `pin_formats()` lists the
  format only where it works, so no GUI needs platform logic.
- Labelled as lossy ("small files"), since the rest of the project is about archival quality.

Status: built (core and FFmpeg; the GUIs' settings come next). What was decided is under
[Decisions](#decisions), item 7; how it works, the quality mapping per encoder and the
probe in [usage.md](../usage.md#h264--hevc-the-systems-video-encoder). FFmpeg 8.1's MediaCodec
wrapper links `libmediandk.so` and `libandroid.so` (both public NDK libraries) and dlopens
only its optional newer calls.

## Android versions and ABIs

Recommendation: **minSdk 29 (Android 10)**, target the latest API. Nothing newer is required:

| API | What it brings | Needed? |
|---|---|---|
| 26 (8.0) | AAudio, notification channels | yes (floor for AAudio) |
| 29 (10) | `AChoreographer` 64-bit callback, thermal status listener, FGS types | nice |
| 30 (11) | Direct file paths into shared storage without permission, thermal headroom | nice; API 29 takes the legacy-storage path instead |
| 31 (12) | Material You system colours | cosmetic |
| 33 (13) | Notification permission (must ask) | handled |
| 34 (14) | FGS type + `FOREGROUND_SERVICE_CONNECTED_DEVICE` mandatory | handled |
| 35 (15) | `mediaProcessing` FGS type | not needed (`connectedDevice` covers finalize) |
| 36 / 36.1 (16) | `Notification.ProgressStyle` (36), Live Updates in the status bar (36.1, `POST_PROMOTED_NOTIFICATIONS`) | optional polish |

The target API is not a choice for a Play build: new apps and updates must target API 36
since 2026-08-31.

Going down to API 26 costs little code (API 26-29 share the legacy storage path) but more
testing on old phones' USB host stacks. ABIs: **arm64-v8a only** (32-bit phones would struggle
with analog anyway); x86_64 optional for the emulator (useful only with the replay device).

## Size

| Part | Size (est.) |
|---|---|
| `libmarvin-core.so` with the static FFmpeg slice (arm64) | ~3.5 MB (the Windows DLL's size) |
| libusb | ~0.15 MB |
| JNI layer | ~0.1 MB |
| firmware | 0.16 MB |
| app code, framework Views (Kotlin, R8) | ~0.1-0.3 MB (a framework-only hello world is under 0.1 MB) |
| app code, Jetpack Compose + Material 3 instead | ~1.5-3 MB for a minimal app (anecdotal); Google's own Views vs Compose comparison of one app: 2.25 vs 2.97 MB |
| hardware encoders + AAC (+ bwdif) | +0.3-0.6 MB (est.) |

So ~4-5 MB installed with framework Views, ~6-8 MB with Compose. Native libraries are stored
uncompressed in the APK (page-aligned loading); Play's download is compressed.

## Risks

1. **Analog drops from CPU idle latency.** On Linux the cause of repeated analog frames was
   wake-up latency from deep idle states ([analog.md](../analog.md#power-saving-and-capture-reliability-linux)),
   fixed by `/dev/cpu_dma_latency`, which an app cannot open. Phones idle aggressively and run
   the xHCI in a different (DWC3) controller. The keep-busy fallback probably works but costs
   battery and heat on a long recording. **Measure in phase 0 before building the app.** DV is
   far less sensitive (3.6 MB/s vs 20.7 MB/s), so DV-only on Android is a fallback position.
2. **Power and charging.** The box draws 300 mA bus power, which phones supply. Charging during
   a long recording needs a USB-C hub with power pass-through, i.e. a hub, which the README warns
   against; on Android the core cannot even see the hub (`hub_depth` -1). Test DV and analog
   through one with nothing else attached; the phone README likely needs a "recommended adapter"
   note.
3. **Thermals.** Hours of screen + preview decode + FFV1 on a phone will throttle. Mitigations:
   dim or stop the preview while recording, thermal status warning (API 29 listener, API 30
   headroom), H.264 for analog. `Window.setSustainedPerformanceMode` only applies while the
   window is visible.
4. **Storage speed and size.** FUSE overhead on shared storage, FAT32 SD cards (the core
   already warns about 4 GiB limits), 13 GB/h for DV.
5. **USB host quirks per phone** (OTG off by default on some, power limits, re-plug resets the
   permission prompt unless "always open" is ticked). The FPGA occasionally needs a power cycle,
   which on a phone means unplugging the adapter.

## Google Play

Possible, with one real blocker (the firmware) and some friction (review without the box,
the testing period, wake-lock vitals). Checked 2026-11-02.

| Topic | What applies | Risk |
|---|---|---|
| Firmware blobs | Play's IP policy and the Developer Distribution Agreement (§11.2: "You represent and warrant that You have the right to distribute the third-party material") rule out bundling Pinnacle's bitstreams. Downloading them from the project at first run is the same distribution. | **high** |
| Review without the hardware | No Play guidance for hardware companion apps. Reviewers get "App access" instructions (text, links). "Broken functionality" / "Limited functionality" are the rejection reasons to avoid. Other USB capture apps (UVC camera, EasyCap recorders) are on Play. | medium |
| USB access | No Play policy or declaration specific to USB host. `uses-feature android.hardware.usb.host` (required) hides the app from phones without host mode. USB permission lasts until unplug; the `USB_DEVICE_ATTACHED` launch grants it. The CAMERA-permission rule for USB video-class devices does not apply: the box is vendor class 0xFF ([hardware.md](../hardware.md)). | low |
| Foreground service | `connectedDevice` type, no time limit; Play Console declaration per type with a description and a **video showing the in-app setup and the device interaction** (so: filmed with the real box). | low (needs the video) |
| Wake locks | Android vitals counts a session as excessive at 2+ h of non-exempt partial wake locks in 24 h, FGS time included; above 5 % of sessions over 28 days the app may lose discovery and get a battery warning on its listing (since 2026-03-01). Long screen-off captures will hit it. Keeping the screen on (black, on OLED nearly free) instead of a wake lock avoids the metric. | medium |
| Account | US$25, ID check, verified Android device. New personal accounts: a closed test with **12 testers opted in for 14 days** before production. | medium (finding 12 testers) |
| Technical | Target API 36 for new apps and updates (since 2026-08-31); AAB + Play App Signing; 64-bit; 16 KB pages (NDK r28+); privacy policy (in the listing and the app) and Data safety form even with no data collected. | low |
| Licences | AGPL apps are on Play (Signal). Statically linked LGPL FFmpeg is fine when the whole app's source is published (relinking possible). | low |

How to make it work:

- **Firmware not in the APK.** An in-app importer: the user picks their own copy of the vendor
  driver (`MarvinAVS64.sys`, or the `.msi`/`.cab` it comes in) and the core extracts the
  bitstreams by offset with MD5 checks, as `scripts/extract-bitstreams.py` does today (a port to
  C in the core, so every GUI could use it). Gadgetbridge (AGPL) handles watch firmware this
  way. The GitHub APK can keep bundling them like the desktop installers, so there would be two
  flavours (`github` with firmware, `play` without), or one APK that always asks. Note that the
  firmware README already names this as the fallback if the rights holder objects.
- **Review.** A demo mode on the replay device (`pin_set_replay_file()` with a short bundled DV
  clip), a clear "no device" screen, and an unlisted video of the app with the box in the App
  access instructions; the same video for the FGS declaration.
- **No self-update notices** in the Play flavour.

Outside Play: F-Droid would reject bundled blobs; with user-supplied firmware it would probably
take the app with the NonFreeDep anti-feature. IzzyOnDroid builds from GitHub releases; whether it
accepts the bundled blobs is unknown. And independent of Play: Android **developer verification**
(enforced since 2026-09-30 in a few countries, globally planned for 2027) will require the
GitHub APK's package name and signing key to be registered too (Play Console account, or the
$25 Android Developer Console). Choose one signing key early and keep it safe; a lost key
cannot be recovered.

## Phases

0. **De-risk on a phone, no app.** The CLI already runs in Termux, so start there: DV, HDV,
   analog (QR disc, 5-minute runs as in [linux-testing.md](linux-testing.md)) on 2-3 phones,
   with and without the keep-busy thread, directly and through a PD hub, writing to `Movies/`;
   measure FFV1 CPU and temperature. Then the core `fd:` open, CLI `--device fd:N` and an NDK
   build of FFmpeg + upstream libusb + core, run the same way. Outcome: which inputs and
   formats are viable on a phone, before any app code exists.
1. **App MVP.** Service, activity, preview, record button, status, notification, `Movies/`
   output, the existing formats. APK on GitHub releases (CI job).
2. **Polish.** Options panel complete, analog controls, audio monitor, finalize progress,
   thermal / battery warnings, SD card location.
3. **Hardware encoder sink.** Android first, then macOS / Windows (same sink, configure flags).
4. **Distribution.** Play Store if the [Google Play](#google-play) issues are acceptable.

## Decisions

Decided by the owner (2026-11-03): 1 framework Views + `Theme.DeviceDefault`; 2 minimum
Android 11 (API 30); 3 GitHub APK with the bitstreams bundled, no Play build for now;
4 HDV MOV / MKV hidden; 5 uncompressed analog AVI kept, with the core's warning; 6 partial
wake lock (screen off allowed; "keep the screen on during a capture" is an option);
7 as below; 8 `Movies/MarvinCapture` only for now; 9 Java. The original options and
recommendations follow.

1. **UI toolkit**: framework Views + `Theme.DeviceDefault` (smallest, uses only what the OS
   ships, Material You colours on 12+, but no bottom sheet or Material 3 controls: those are
   hand-made) / Jetpack Compose + Material 3 (+1.5-3 MB, modern controls, quicker to build).
2. **Minimum Android version**: 10 (API 29) / 8.0 (API 26, same storage code, more old-phone
   USB testing) / 11 (API 30, no legacy storage path at all).
3. **Distribution and firmware**: GitHub APK with bundled firmware first, Play later with a
   firmware importer / Play from the start / importer everywhere (one APK, nothing
   proprietary shipped).
4. **HDV MOV / MKV on Android** (full remux at Stop, minutes, twice the space): hide in v1 / keep,
   with the finalize notification.
5. **Uncompressed analog AVI on Android** (~75 GB/h): hide / keep.
6. **Screen-off recording**: partial wake lock (works with the screen off; counts against Play's
   excessive-wake-lock vitals beyond 2 h) / keep the screen on, black (no vitals issue, needs
   the screen on).
7. **Hardware encoder scope**: decided and in the core (`src/sinks/sink_hwenc.c`, see
   [usage.md](../usage.md#h264--hevc-the-systems-video-encoder)): analog and DV at once;
   both interlace modes (keep the fields as one progressive picture, with a quality warning,
   or bwdif to 50p / 59.94p, the default); MKV with FLAC; H.264 and HEVC where the encoder
   answers; the same sink on Android, macOS and Windows (none on Linux). A quality setting on
   the CRF scale and FFV1 compression levels came with it (`pin_format_options()`).
8. **Storage locations in v1**: `Movies/MarvinCapture` + SD card app folder / also SAF (any
   folder, more core work).
9. **Language**: Kotlin (the R8-shrunk stdlib is ~0.1-0.2 MB) / Java (zero runtime overhead,
   more verbose).

Questions for the Termux tester: root or `termux-usb`, which phone, which inputs (analog?).

## Sources

Checked 2026-11-02.

- libusb Android flow and `NO_DEVICE_DISCOVERY`: <https://github.com/libusb/libusb/blob/master/android/README>,
  ChangeLog <https://github.com/libusb/libusb/blob/master/ChangeLog> (1.0.30, 2026-05-17)
- Termux USB: <https://github.com/termux/termux-api-package> (`termux-usb`),
  <https://github.com/termux/termux-packages/tree/master/packages/libusb> (`termux-usb-support.patch`)
- USB host on Android: <https://developer.android.com/develop/connectivity/usb/host>,
  <https://developer.android.com/reference/android/hardware/usb/UsbManager>
- Kernel: usbfs memory limit (`usbfs_memory_mb`, `drivers/usb/core/devio.c`), autosuspend
  <https://docs.kernel.org/driver-api/usb/power-management.html>
- FFmpeg encoders: `libavcodec/mediacodecenc.c`, `doc/encoders.texi`, `configure` (release/8.1);
  licences <https://ffmpeg.org/legal.html>; `libavfilter/vf_bwdif.c`, `vf_yadif.c` (LGPL-2.1+)
- MediaCodec requirements: Android 16 CDD 5.1 <https://source.android.com/docs/compatibility/16/android-16-cdd>;
  AOSP `media_codecs_sw.xml`; MediaMuxer <https://developer.android.com/reference/android/media/MediaMuxer>
- Storage: <https://developer.android.com/training/data-storage/shared/media#direct-file-paths>,
  <https://developer.android.com/training/data-storage/use-cases>, FUSE passthrough
  <https://source.android.com/docs/core/storage/fuse-passthrough>; MediaProvider
  `insertFileIfNecessaryForFuse` (MIME vs directory check)
- Foreground services: types <https://developer.android.com/develop/background-work/services/fgs/service-types>,
  timeouts <https://developer.android.com/develop/background-work/services/fgs/timeout>,
  staying awake <https://developer.android.com/develop/background-work/background-tasks/awake>,
  Live Updates <https://developer.android.com/develop/ui/views/notifications/live-update>
- UI size: <https://developer.android.com/develop/ui/compose/migrate/compare-metrics>,
  <https://dmitrybrant.com/?p=2087>
- Play: App access <https://support.google.com/googleplay/android-developer/answer/9859455>;
  functionality policies <https://support.google.com/googleplay/android-developer/answer/9898783>;
  FGS declaration <https://support.google.com/googleplay/android-developer/answer/13392821>;
  excessive wake locks <https://developer.android.com/topic/performance/vitals/excessive-wakelock>,
  <https://android-developers.googleblog.com/2025/11/raising-bar-on-battery-performance.html>;
  target API <https://support.google.com/googleplay/android-developer/answer/11926878>;
  testing requirement <https://support.google.com/googleplay/android-developer/answer/14151465>;
  IP policy <https://support.google.com/googleplay/android-developer/answer/9888072>;
  DDA <https://play.google/developer-distribution-agreement.html>;
  16 KB pages <https://developer.android.com/guide/practices/page-sizes>;
  privacy / Data safety <https://support.google.com/googleplay/android-developer/answer/10144311>,
  <https://support.google.com/googleplay/android-developer/answer/10787469>;
  developer verification <https://developer.android.com/developer-verification/guides/faq>,
  <https://android-developers.googleblog.com/2026/06/android-developer-verification.html>
- Distribution elsewhere: <https://f-droid.org/docs/Inclusion_Policy/>,
  <https://f-droid.org/docs/Anti-Features/>, <https://izzyondroid.org/docs/general/AppInclusionPolicy/>;
  firmware import precedent <https://codeberg.org/Freeyourgadget/Gadgetbridge/wiki/Firmware-Update>
- Not confirmed: sustained FUSE write speed on Android 11; OTG bulk throughput / DWC3 behaviour
  on phones; whether any phone encoder does MBAFF/PAFF; a firmware-specific Play takedown
  precedent.
