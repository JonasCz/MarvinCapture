# Android GUI: feasibility and plan

Status: plan only, nothing implemented. Written 2026-11-02 from the code at `1ce9d79`.
Facts about Android, FFmpeg, libusb and Google Play were checked against their
documentation on that date (sources at the end); anything not confirmed is marked
"unverified".

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
Android kernel and toolchain. It does not show the USB access path an app needs:
stock libusb cannot list devices on an unrooted phone, so that test most likely ran
as root or with an fd handed over by `termux-usb` (see [sources](#sources) for what
Termux's libusb does). Worth asking the tester which one it was, which phone, and
whether analog capture was tried (it is the timing-sensitive input).

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
  finalising, and the ongoing notification (elapsed, size, errors, a Stop action; "Finalizing…"
  with progress after Stop). Type `connectedDevice` (allowed because the app holds USB permission
  for the device). Started from the record button (the app is in the foreground then, so the
  Android 12+ background-start limits don't apply). The finalise step after Stop (HDV remux,
  FFV1 flush) runs in the same service, which is what makes "remux in a background task with a
  notification" work without a separate job system.
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
   path forms. `pinnacle_open_by_id()` gets a branch: a context with
   `LIBUSB_OPTION_NO_DEVICE_DISCOVERY`, `libusb_wrap_sys_device()`, PID from the descriptor,
   then the same set-configuration / claim as today. Hub depth is unknown there (-1, no hub
   warning). The cross-process lock keyed by "fd:N" is meaningless but harmless: usbfs itself
   refuses a second claim (BUSY), which is already reported as "in use".
   The MovieBox Deluxe re-enumerates after its FX2 download and the reopen path enumerates
   devices, so it stays unsupported on Android (it is untested anyway).
2. **Model lookup without enumeration**: `pin_usb_model(vid, pid, pin_device_info_t *out)`
   (name, supported, tested) so the app names the device from the same table. The
   `device_filter.xml` PID list is generated from, or tested against, `pinnacle_model.c`.
3. **CLI `--device fd:N`**: the same id through the existing `-d`, so `termux-usb` can hand
   the CLI an fd on an unrooted phone. This is phase 0's test vehicle and a small feature in its
   own right.
4. **Directories**: `pin_set_app_dirs(config, runtime, temp)` (or the app sets `XDG_CONFIG_HOME`
   and `TMPDIR` before the first core call, which works today with no change). Firmware: the APK
   carries `firmware/` as assets, copied to `filesDir` on first start or version change, then
   `pin_set_firmware_dir()`.
5. **HDV remux temp file**: `<name>.rawts.tmp` is written next to the output today. In
   `Movies/` that name is not a video type; move it to the temp dir (configurable, default
   unchanged on desktop).
6. **Finalize progress**: `progress_percent` during `PIN_STATE_STOPPING` (the HDV remux knows
   its read position in the temp file). Feeds the notification's progress bar, and the desktop
   GUIs' "Finalizing…" could show it too.
7. **Platform perf hints**: under `__ANDROID__`, `pin_perf_check()` must not report
   `PIN_PERF_CPU_LATENCY` (the user cannot fix it); whether the keep-busy thread stays on is
   decided by phase 0's measurements.
8. **Hardware encoder sink** (phase 3), see [H.264 / HEVC](#h264--hevc-hardware-encoding).

Build: `scripts/build-ffmpeg.sh` gains an Android mode (NDK clang, `--target-os=android`,
`--arch=aarch64`, cross compile); libusb is built from source with the NDK (it is not on the
system); the top-level `CMakeLists.txt` gets an option to skip the CLI/tests for the app build.
The NDK links with 16 KB page alignment by default from r28, which Play requires. A
`scripts/build-android.sh` + an Ubuntu CI job, following the "scripts do the work, YAML picks
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
| Android | `h264_mediacodec`, `hevc_mediacodec` (NDK `AMediaCodec`) | yes | `--enable-mediacodec` + encoders (JNI needs, see sources) |
| macOS | `h264_videotoolbox`, `hevc_videotoolbox` | yes (VideoToolbox) | `--enable-videotoolbox` + encoders |
| Windows | `h264_mf`, `hevc_mf` (Media Foundation; the GPU vendor's MFT underneath) | yes | `--enable-mediafoundation` + encoders |
| Linux | VAAPI needs libva; no LGPL software H.264 encoder (x264 is GPL) | no | probably not offered |

The sink (`sink_hwenc.c`) would follow `sink_ffv1.c`: encoder thread behind a bounded queue,
audio on its own thread, the muxer behind a lock.

- **Input**: analog YUY2 → NV12 with field-aware 4:2:2→4:2:0 chroma (each field on its own, so
  colour doesn't bleed between fields). DV: decode with `dvvideo` (already built in for the
  preview) → NV12 (PAL 4:2:0 as is, NTSC 4:1:1 upsampled); audio as `sink_rewrap.c` extracts it.
  HDV: would need an MPEG-2 HD software decode first; not planned (the `.ts` is already compact).
- **Interlacing**: phone encoders encode progressive frames (see sources). Either encode the
  interlaced frame as is and flag the field order in the container (nothing is lost; players that
  honour the flag deinterlace, phone gallery players usually don't and show combing), or
  deinterlace to 50p / 59.94p first with FFmpeg's `bwdif` or `yadif` (both LGPL; brings in a
  small part of libavfilter). Decision below.
- **Container**: MKV survives a crash or a dead battery mid-recording; a plain MP4 written by
  MediaMuxer does not (no `moov` until stop). With FFmpeg's muxer: MKV, or fragmented MP4
  (`frag_keyframe`) for gallery compatibility. **Audio**: AAC (FFmpeg's native `aac` encoder,
  LGPL) for MP4; FLAC or PCM in MKV.
- **Availability**: the core opens the encoder once to probe it and `pin_formats()` lists the
  format only where it works, so no GUI needs platform logic.
- Labelled as lossy ("small files"), since the rest of the project is about archival quality.

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
| 36 (16) | Progress-centric notifications / Live Updates | optional polish |

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
| app code, framework Views | ~0.2-0.4 MB |
| app code, Jetpack Compose + Material 3 instead | +2-3 MB (see sources) |
| hardware encoders + AAC (+ bwdif) | +0.3-0.6 MB |

So ~4-5 MB installed with framework Views. Native libraries are stored uncompressed in the
APK (page-aligned loading), Play's download is compressed.

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
   dim or stop the preview while recording, thermal status warning, H.264 for analog.
4. **Storage speed and size.** FUSE overhead on shared storage, FAT32 SD cards (the core
   already warns about 4 GiB limits), 13 GB/h for DV.
5. **USB host quirks per phone** (OTG off by default on some, power limits, re-plug resets the
   permission prompt unless "always open" is ticked). The FPGA occasionally needs a power cycle,
   which on a phone means unplugging the adapter.

## Google Play

(filled in below)

## Phases

0. **De-risk on a phone, no app.** Core `fd:` open, CLI `--device fd:N`, Android NDK build of
   FFmpeg + libusb + core + CLI. Run it from Termux via `termux-usb` on 2-3 phones: DV, HDV,
   analog (QR disc, 5-minute runs as in [linux-testing.md](linux-testing.md)), with and without
   the keep-busy thread, through a PD hub, writing to `Movies/`. Measure FFV1 CPU and
   temperature. Outcome: which inputs and formats are viable on a phone.
1. **App MVP.** Service, activity, preview, record button, status, notification, `Movies/`
   output, the existing formats. APK on GitHub releases (CI job).
2. **Polish.** Options panel complete, analog controls, audio monitor, finalize progress,
   thermal / battery warnings, SD card location.
3. **Hardware encoder sink.** Android first, then macOS / Windows (same sink, configure flags).
4. **Distribution.** Play Store if the [Google Play](#google-play) issues are acceptable.

## Decisions

See the list in the conversation that produced this plan; recorded here once made.

## Sources

(filled in below)
