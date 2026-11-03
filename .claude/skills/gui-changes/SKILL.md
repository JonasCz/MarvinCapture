---
name: gui-changes
description: Make changes to the MarvinCapture GUIs (Windows: WinUI 3 / C#, gui/windows/MarvinCaptureGUI; macOS: SwiftUI + AppKit, gui/macos; Android: Java + framework widgets + a C JNI layer, gui/android): where things live, how settings and capture options flow, the core-first rule, threading contracts, build/lock gotchas. Use before editing GUI XAML, view models, Swift views/models, the Android app or the native interop.
---

# GUI changes

Three GUIs: `gui/windows/` (C#, WinUI 3; sections below up to "Taskbar / shell
interop"), `gui/macos/` (Swift; see "macOS GUI") and `gui/android/` (Java + JNI; see
"Android app" at the end). Docs: `gui/windows/README.md`, `gui/macos/README.md`,
`gui/android/README.md`. The macOS app is a port of the Windows one, the Android app a
phone-shaped port of the macOS one: when behaviour changes in one, check the others.

## Core-first rule

Each platform gets its own GUI codebase (`gui/windows/`, a C# WinUI 3 app;
`gui/macos/`, a Swift app; a Linux GUI would be separate code again). So **put
every piece of logic that isn't UI in the core** (`src/engine`, `src/api`) and keep
the per-platform code to widgets, dialogs, bindings and the native call layer.
Before writing C# or Swift logic, ask "would the other GUI need this too?". If yes,
it goes in the core and the GUI calls it through `pin_api.h`. Examples already done that way: output naming
and collision/free-space checks (`pin_check_output`), file name validation
(`pin_naming_validate`), settings, command-line parsing, scene splitting, the next
file number (`pin_next_file_number`), every status-bar / device-list text
(`pin_format_status_short`, `pin_format_signal`, `pin_format_frames`,
`pin_format_sizes`, `pin_format_storage_*`, `pin_format_bytes`,
`pin_device_status_text`, `pin_device_unavailable_reason`, `pin_no_devices_hint`,
... in `pin_api.h`, "ready-made texts") and the button enable rules
(`pin_deck_cmd_allowed`, `pin_capture_action_allowed`). The text helpers are in
`src/engine/pin_ui_text.c` (pure) and `src/api/pin_api.c` (snapshot / device based;
tested by `tests/engine/test_pin_ui_text.c` and `test_pin_ui_api.c`). A new status
text or enable rule goes there, not into C#.

Core change checklist: function in `src/engine/*.c` + declaration in its
header, expose through `src/api/pin_api.[ch]` if the GUI needs it, add a test in
`tests/engine/` (registered in `src/engine/CMakeLists.txt`), and mirror any new
API/struct/enum in `Interop/Native*.cs` (Windows; macOS imports the header, so
only a thin wrapper in `Core/Pin.swift`, if any). Prefer reusing an existing call (e.g.
`pin_check_output` already returns a message the GUI shows) over a new export.

## Where things are (gui/windows/MarvinCaptureGUI)

(Windows only; the macOS layout is in "macOS GUI" below.)

- `MainWindow.xaml` / `.xaml.cs`: the one window. Analog panel is `[0]`, DV/HDV
  panel is `[1]` inside `PanelSwitch`. Click handlers, dialogs, pickers.
  `StartCaptureAsync` is the capture entry: validates, calls `VM.CheckOutput`,
  shows dialogs, then `VM.StartCapture`.
- `ViewModels/MainViewModel.cs`: all state (CommunityToolkit.Mvvm
  `[ObservableProperty]`; `partial void OnXChanged` hooks). `BuildCaptureOpts`
  turns UI state into `PinCaptureOpts`; `LoadSettings` / `SaveSetting` persist
  (keys `gui.*`, stored via the core settings); `ApplyLaunch` applies command
  line presets.
- `ViewModels/KindSettingsViewModel.cs` + `Views/KindSettingsView.xaml`: the
  per-kind (DV, HDV) format / aspect / split / idle / passes options.
- Format settings (deinterlace, quality, FFV1 compression): `ViewModels/FormatOptionsViewModel.cs`
  holds `FormatChoices` (the stored values, -1 = the format's default; keys
  `gui.deinterlace_analog` / `_dv`, `gui.quality_analog` / `_dv`, `gui.ffv1_compression`, the
  Android app's; DV and HDV share the DV ones; saved in `MainViewModel.OnFormatChoiceChanged`)
  and `FormatOptionsViewModel` (one per format drop-down: `MainViewModel.AnalogFormatOptions`,
  `KindSettingsViewModel.FormatOptions`; everything from `pin_format_options()`,
  `pin_format_warning()`, `pin_format_quality_hint()`). `Views/FormatOptionsView.xaml` is the
  Flyout of the gear right of each format drop-down, set as `Flyout.Content` in code
  (constructor injection, like `KindSettingsView`). The warning is an InfoBar under the drop-down.
- Format lists: **empty until the encoder probe is done.** `MainViewModel.ProbeEncoders`
  (called first thing in the `MainWindow` constructor) runs `pin_encoders_probe()` on a worker
  thread, then `RefreshFormats` fills the analog, DV and HDV lists. Never call `Native.Formats`
  on the UI thread before that: listing the analog or DV formats runs the probe (50-300 ms per
  encoder). Saved / command-line formats go through `SelectAnalogFormat` /
  `KindSettingsViewModel.ApplyFormat`, which remember the wanted one for the refresh.
- `Interop/`: `Native.cs` (P/Invoke), `NativeStructs.cs`, `NativeEnums.cs`. Enum
  members are prefixed like `PinStatus.ErrArg` (not `Arg`).
- Output path = `OutputDir` + `Name` (analog: `AnalogOutputDir`/`AnalogName`,
  DV/HDV shared: `DvOutputDir`/`DvName`); `AnalogOutputPath` / `DvOutputPath` are
  computed. The name box is also the embedded title (no separate Title field).
  The directory button uses `FolderPicker`.

## Adding or changing a native call / option

- `Interop/Native.cs` uses source-generated `[LibraryImport]` (private) with a
  friendly public wrapper. Size-versioned structs the core fills are passed by
  `ref`, never `out` (`out` zero-inits and wipes `size`, giving `PIN_ERR_ABI`).
- Fixed-size embedded UTF-8 strings in structs (`Path`, `Title`, ...) go through
  `Utf8Fixed` property wrappers in `NativeStructs.cs`. A new field in
  `pin_capture_opts_t` must be added in the same order/size in C and C#. Structs whose size
  the core checks exactly (`pin_format_options_t`: `size != sizeof` is `PIN_ERR_ABI`) must match
  byte for byte; appended fields of `pin_capture_opts_t` / `pin_script_settings_t` are fine
  because the core also takes the older sizes.
- Settings: `Native.SettingsGet/Set` -> core `pin_settings_get/set`. Keys are
  `gui.*`, all best-effort (try/catch). Writes are suppressed while `_loading`.
  Add the save in the `OnXChanged` hook and the load in `LoadSettings`. Options
  are **per device**: `SaveSetting`/`LoadSetting` prefix the key with
  `dev_<GUID>.` (core `pin_device_settings_key`, scope `_settingsScope`), and
  `OnSelectedDeviceChanged` reloads them when another device is selected (no
  device = defaults, nothing saved). Only window geometry, `gui.last_device`
  and `gui.muted` are global: use `SaveGlobalSetting`/`LoadGlobalSetting`.
  Startup order matters: select the device (loads its settings), then
  `ApplyLaunch`, then open. No migration of old flat keys.
- Command line: the core parses it (`pin_script_parse`, language in
  `docs/cli.md`); the GUI applies the settings it has fields for
  (`pin_script_settings`) in `ApplyScriptSettings`, and runs the steps with
  `pin_script_run` once READY (`MainWindow.RunPendingScriptIfReady`; `PIN_EVT_STEP`
  / `PIN_EVT_DONE` in `HandleEvent`). Docs: `gui/windows/README.md`.
  `--debug` (the shared parser's `pin_script_debug`) sets `pin_set_log_level(0)`
  in `ParseCommandLine` so `ConsoleOutput` mirrors the core's debug log; there
  is no environment variable for it any more.
- Tab order / accessibility: give controls `AutomationProperties.Name` (and
  `HelpText` for the why); keep the XAML order = tab order.
- Errors to the user: `VM.ShowInfo(title, message, severity)` (InfoBar); modal
  questions: `ShowDialogAsync(title, text, primary, close)`.

## Status bar

One-line grid in `MainWindow.xaml` (bottom). Every text uses `StatusTextStyle`
(do not set FontFamily/FontWeight/FontSize). Each item sits in a "host" panel
that carries its 24 DIP left margin (a collapsed host leaves no gap).
`MainWindow.FitStatusBar` shows deck/storage by state and, if the items do not
fit next to the file text's 80 DIP, collapses free-space, frames, storage, time, deck
in that order (the file text is the first item, deck the second) (re-run on size and on the text properties changing).
Properties are set in `MainViewModel.ApplyStatus` straight from the core
snapshot (the strings come from the `pin_format_*` calls via `Native.Format*`); counters (`frames_error`, `clip_*`, `total_bytes_written`,
`est_seconds_left`) and the format label (`video_label`) are the core's, never
computed in C#. Low-disk uses two copies of the storage elements toggled by
`DiskLow`. Minimum window width is 1000 DIP.

## Debugging

- Core debug log: start `build\windows-x86_64\dist\MarvinCaptureGUI.exe --debug` from a console or
  with stdout redirected to a file (AV/C traffic, bring-up steps; `docs/cli.md`).
- Crashes / unhandled exceptions: `%LOCALAPPDATA%\PinnacleOSS\crash.log`
  (absent = clean run). If the core DLL fails to load or reports an API
  version other than 3, the app shows an error window instead of the UI.
- Quick GUI-only build: `dotnet build gui\windows\MarvinCaptureGUI -c Debug
  -p:Platform=x64`; the run target is
  `build\windows-x86_64\gui\bin\x64\Debug\net10.0-windows10.0.19041.0\win-x64\`
  and needs `marvin-core.dll` copied from `build\windows-x86_64\core` (done by the csproj).
- Hardware/replay testing of capture behaviour: see test-with-hardware-device.
- Packages: Windows App SDK 2.5.1 as components (Foundation, InteractiveExperiences,
  WinUI, DWrite; not the metapackage, which adds ~58 MB of AI/ML/Search/Widgets),
  CommunityToolkit.Mvvm 8.4.2, Vortice D3D11. App is unpackaged, self-contained, x64.
  Release is `dotnet publish`: trimmed (`TrimMode=partial`) + ReadyToRun, not NativeAOT
  (field-style `[ObservableProperty]` is fine; keep bindings `{x:Bind}`, avoid
  reflection; classic `[ComImport]` needs `BuiltInComInteropSupport=true`, which the
  csproj sets). See docs/windows-size.md.

## Gotchas

- `x:Bind` with `UpdateSourceTrigger=PropertyChanged` is needed for TextBoxes
  whose value is read at Start (otherwise the value commits on focus loss).
- Pickers need `WinRT.Interop.InitializeWithWindow.Initialize(picker, _hwnd)`.
  `FileSavePicker` creates an empty placeholder file; `FolderPicker` doesn't.
- Edits made with Python/sed: C source needs doubled backslashes; heredoc
  Python strings halve them. Check escapes (`"\\"`) in the result.
- Git reports CRLF to LF warnings on commit; harmless.
- XAML/C# files in the working tree may be CRLF; scripted edits must detect the
  line ending (assert the match count). Deck-button enablement lives in
  `MainViewModel` (`DeckRewEnabled` etc., driven by `DeckState`; the bodies only call
  the core rules `Native.DeckCmdAllowed` / `CaptureActionAllowed`); any new
  property they depend on must be added to the `[NotifyPropertyChangedFor]`
  lists of `_isCapturing` / `_sessionState` / `_deckState`.
- Window geometry uses `AppWindow` (no P/Invoke): `gui.window` + `gui.window_maximized`,
  saved in `Closing` and `Closed`; `DisplayAreaFallback.None` returns null for an
  off-screen rect, which is how the default fallback is detected.

## Taskbar / shell interop (Controls/TaskbarButton, TaskbarIcons, Interop/Win32.cs)

- **No WndProc in WinUI 3.** To see window messages, subclass the HWND from
  `WindowNative.GetWindowHandle(this)` with comctl32 `SetWindowSubclass`; keep the
  `SubclassProc` delegate in a field (the function pointer from
  `Marshal.GetFunctionPointerForDelegate` dangles if it is collected), never let an
  exception leave the proc, always end in `DefSubclassProc`, remove it in Dispose.
  The proc runs inside the message: hand real work to `DispatcherQueue.TryEnqueue`
  (a capture start can open a ContentDialog).
- **`TaskbarButtonCreated`** (`RegisterWindowMessage`) is sent when the shell makes the
  button, and again after an Explorer restart; everything set on the button before is
  lost. `ThumbBarAddButtons` fails until then and only works once per button, so the
  code retries on the status tick, and the message drops all cached state
  (progress, overlay, buttons, clip) so the next tick re-applies it. Call
  `ChangeWindowMessageFilterEx` for it, or an elevated app never gets it.
- **The project disables runtime marshalling** (`[LibraryImport]` everywhere): arrays/strings in
  signatures give SYSLIB1051; use `char*` / `nint` buffers. Same for the `[ComImport]`
  ITaskbarList3: it is declared with `nint` buffers, and THUMBBUTTON (552 bytes on x64,
  mask 0, id 4, icon 16, tip 24, flags 544) is written into unmanaged memory by hand.
  The COM vtable must declare every member up to the last one called, in order.
- **Icons** are made at run time: 32-bit top-down DIB section + a zeroed 1-bpp mask
  bitmap -> `CreateIconIndirect` (straight, not premultiplied, alpha). Glyphs: draw white
  text with `ANTIALIASED_QUALITY` on black in a DIB and use the coverage as alpha (GDI text
  has no alpha). GDI silently substitutes a missing font: check `GetTextFace` (Segoe
  Fluent Icons on Windows 11, else Segoe MDL2 Assets). Size = `GetSystemMetricsForDpi(SM_CXSMICON,
  GetDpiForWindow)`; colour follows `SystemUsesLightTheme` (the thumbnail bar is
  taskbar-themed); rebuild on `WM_DPICHANGED` / `WM_SETTINGCHANGE "ImmersiveColorSet"`.
  `DestroyIcon` what you create, after the shell has the replacement.
- `SetThumbnailClip` takes client pixels: XAML position x `XamlRoot.RasterizationScale`
  (verified: the XAML root fills the client area even with `ExtendsContentIntoTitleBar`).
- Thumbnail-button clicks go through the same view-model enable rules and
  `StartCaptureAsync` as the window buttons; Stop uses `PIN_STOP_DECK_AS_STARTED`.

## Build / verify

Use `scripts\build.ps1` (see the build-and-test skill). It builds the core,
runs ctest, then the GUI. The XAML/C# compiler errors appear before the copy
step. If it fails with MSB3027 "file is locked by MarvinCaptureGUI (pid)", the
app is running from `build\windows-x86_64\dist`: the code compiled fine, but ask the user to
close it (don't kill it) and re-run. Don't run `ctest` directly from
`build\windows-x86_64\core` (exit 0xc0000139 = DLL path not set); the script sets PATH.

Commit and push straight to main (single-developer repo).

## macOS GUI (gui/macos)

SwiftPM package, Swift 6.x toolchain in Swift 5 language mode, macOS 15, arm64.
AppKit owns the app, the window and the menus; SwiftUI is hosted inside the window
(`NSHostingController`). Docs: `gui/macos/README.md`.

### Where things are (gui/macos/Sources/MarvinCapture)

- `App/`: `main.swift` (entry; the body runs in `MainActor.assumeIsolated`, because
  top-level code is nonisolated for the compiler), `AppDelegate` (wires model,
  menus, Dock tile, close flow; terminate), `MainWindowController` (the one
  window, frame autosave, instance cascade), `MainMenu` (`MenuController`: menu
  bar and Dock menu, enable rules via `validateMenuItem`), `CloseFlow`
  (close / quit while capturing), `CaptureFlow` (start: plan, confirmation sheets,
  start), `Alerts` (one NSAlert sheet at a time), `NewWindow` (+ `Instance`),
  `Startup` (API version probe, firmware dir), `SnapshotHook`.
- `Core/Pin.swift`: stateless wrappers over `pin_api.h` (formatting, settings,
  script, devices). `CoreString.swift`: `cString` / `setCString` for the `char[N]`
  tuple fields. The C API comes in as module `CMarvinCore`
  (`Sources/CMarvinCore/module.modulemap`, no copy of the header): **no hand
  mirroring of structs / enums**; a new C function is usable in Swift as soon as
  it is in `pin_api.h`. Size-versioned structs: set `size` yourself (see the
  `make*` helpers), pass by `&`.
- `Models/WindowModel.swift`: all window state and logic (the macOS
  MainViewModel + the non-view part of MainWindow.xaml.cs): settings load / save,
  device watch, open / close session, `buildCaptureOpts`, `planCapture` /
  `startCapture`, the 100 ms `tick()` (status + events) and the 30 Hz meter timer,
  `applyStatus`, `handleEvent`. `AppModel`: command line (`pin_script_parse`).
  `KindSettingsModel` (DV / HDV tab), `ControlSliderModel`, `DeviceItem`,
  `FormatItem`. All `@MainActor @Observable`; non-observed state is
  `@ObservationIgnored`. `FormatOptionsModel.swift`: `FormatChoices` (stored deinterlace /
  quality / FFV1 values, same keys as Windows and Android), `FormatOptions`
  (`pin_format_options_t` as Swift strings) and `FormatOptionsModel` (one per format picker:
  `analogFormatOptions`, `KindSettingsModel.formatOptions`). The format lists are empty until
  `probeEncoders()` (a detached task started in `WindowModel.init`) returns and
  `refreshFormats()` fills them; never list formats on the main thread before that.
- `Services/`: `MediaSeams.swift` (the `PreviewSink` and `AudioMonitor` protocols
  and null implementations), `AudioMonitor.swift` (`CoreAudioMonitor`),
  `DockTile`, `Notifier`, `KeepAwake`, `ConsoleOutput`.
- `Preview/`: `PreviewRenderer` (render thread, Metal), `PreviewShaders` (MSL as a
  string), `MetalPreview` (the `PreviewSink`: view, visibility).
- `Views/`: SwiftUI (`Sidebar/`, `StatusBar/`, `PreviewArea`, ...). `Sidebar/FormatOptionsView.swift`:
  the gear right of each format picker (`FormatOptionsButton`) and its popover.
  `StatusBarLayout.swift` decides which status items fit (drop order free space,
  frames, storage, time, deck: same as `FitStatusBar` on Windows).

Rules like on Windows: every text and enable rule comes from the core
(`pin_format_*`, `pin_deck_cmd_allowed`, `pin_capture_action_allowed`); settings
are per device (`saveSetting` / `loadSetting` add the `dev_<GUID>.` scope) with
writes suppressed while `loading`; the window frame is AppKit's autosave, not a
setting. A model property a view reads is plain (observed); wiring (callbacks, timers,
the preview / audio seams) is `@ObservationIgnored`.

### Threading contracts (do not break)

- **Preview render thread.** `PreviewRenderer` blocks in `pin_preview_wait`, takes
  frames with `pin_preview_lock_due`, and only enters the core through `enterCore()`,
  which hands out the session while it is attached and marks `inCore`.
  `PreviewSink.detach()` is synchronous: when it returns the thread is out of the
  core. Locks: `state` (handshake, flags) and `gate` (all Metal / layer use), never
  taken gate -> state.
- **Audio render block** (`CoreAudioMonitor`): runs on CoreAudio's real-time thread
  and must not allocate, message objects or hop actors, so it only touches a
  manually allocated `RenderCore` struct behind an `os_unfair_lock`: the block only
  try-locks (silence for that buffer if the main thread holds it), `detach()` takes
  the lock and clears the session.
- **Close order** (`WindowModel.closeSession`): `audio.stop()`, `audio.detach()`,
  `preview.detach()`, then `pin_close` (which blocks until files are finalised).
  Never call into a session after detach. Callers stop a running capture and wait
  for READY first (`CloseFlow`).
- **Device watch thread**: blocked in `pin_devices_wait`; hands changes to the main
  actor with `DispatchQueue.main.async { MainActor.assumeIsolated { ... } }`;
  `shutdown()` sets its stop flag and calls `pin_devices_wake`.
- Everything else (timers, views, models) runs on the main thread; timers are
  added in `.common` modes so they keep running while menus are open and while
  AppKit waits for a `terminateLater` reply. The close flow always answers a
  pending terminate (`CloseFlow.replyPending`).

### Build and verify

```sh
bash scripts/build.sh                  # everything incl. tests, then dist/MarvinCapture.app
bash scripts/build.sh --skip-tests     # faster
swift build --package-path gui/macos --scratch-path build/macos-arm64/gui \
    -Xlinker -L$PWD/build/macos-arm64/core      # Swift only, core already built
```

A bare binary needs `DYLD_LIBRARY_PATH=build/macos-arm64/core`. Run the real app
from a shell with `build/macos-arm64/dist/MarvinCapture.app/Contents/MacOS/MarvinCapture`
(stdout visible, `--debug` works, e.g. with a replay file as `--device file.dv`);
`open ...app --args` loses stdout.

Agents have **no screen-recording permission**, so verify with the built-in aids
(env vars, all in `gui/macos/README.md`): `MARVIN_SNAPSHOT=/tmp/x.png`
(+ `MARVIN_SNAPSHOT_QUIT=seconds`) renders the window content to a PNG, then read
the PNG; `MARVIN_WINDOW_SIZE=WxH` and `MARVIN_APPEARANCE=dark|light` for layout and
theme; `MARVIN_PREVIEW_DUMP` for the preview (a CAMetalLayer is not in the
snapshot); `MARVIN_MENU_DUMP=1` for the menu tree and its enable state;
`MARVIN_DOCK_DUMP` for the Dock tile; `MARVIN_UNMUTE=1` for the audio monitor
(statistics with `--debug`); `MARVIN_PREVIEW_IGNORE_OCCLUSION=1` on a locked screen.
A new aid goes in the same style and into that list in the README.

### Gotchas

- **No Metal compiler in the Command Line Tools**: the shader is MSL source in
  `PreviewShaders.swift`, compiled at run time with `makeLibrary(source:)`. No
  `.metal` files, no `.metallib`.
- **No `actool`**: the icon is the committed `Resources/AppIcon.iconset` (PNGs of the logo,
  rendered by `scripts/make-icons.py`) packed with `iconutil` by `build.sh`. No asset catalog.
- **`Info.plist` is generated by `scripts/build.sh`** (version, bundle id, minimum
  system): edit it there. Firmware goes in `Contents/Resources/firmware`, not
  `Frameworks` (codesign). Signing is ad hoc, inner dylibs first, then the bundle.
- **Swift 5 language mode** (`swiftLanguageModes: [.v5]`): the models are
  `@MainActor`, callbacks from other threads must hop explicitly. Timer / observer
  closures that run on the main thread use `MainActor.assumeIsolated`; code that
  runs off the main thread (render thread, audio block, device watch) must not
  touch main-actor state.
- One window per process: "New Window" starts another process (`NewWindow`,
  `MARVIN_SECONDARY=1`); only the first instance saves the window frame.
- `UNUserNotificationCenter` and `NSApp.dockTile` badges need the `.app`; a bare
  binary skips notifications.
- Sheets: use `Alerts.shared.ask` (serialised, brings the window forward);
  never `runModal` on top of the window. `CaptureFlow.starting` and
  `WindowModel.captureStartPending` stop a second start while sheets are open or the
  core has not yet reported CAPTURING (the core starts asynchronously).
- `bash scripts/build.sh` must stay green (tests + app). Keep the README's
  MARVIN_* list complete (`grep -rn MARVIN_ gui/macos/Sources`).

## Android app (gui/android)

Java (no Kotlin), framework widgets and `Theme.DeviceDefault` only: **no AndroidX, no
Material, no dependencies** (a decision of the owner, for size; keep it). minSdk 30,
target 36. One device, one screen; no deck control, no automatic capture or
multi-pass, no command line. Docs: `gui/android/README.md`.

### Where things are

- `app/src/main/cpp/` (C, built by `scripts/build-android-native.sh`, not by Gradle):
  `marvin_jni.c` (the `pin_api.h` wrappers as static natives of `Core`), `preview.c`
  (EGL + GLES 3 renderer), `audio.c` (AAudio monitor + low-latency keep-alive).
- `app/src/main/java/jonascz/marvincapture/`: `Core` (natives, mirrored enum values and
  the array indices of `getStatus` / capture options: **keep them in step with
  `pin_api.h` and `marvin_jni.c`**), `CaptureService` (session, USB connection, capture,
  notification, wake lock, keep-alive, event queue, `buildPlan` = buildCaptureOpts),
  `MainActivity` (screen, USB discovery / permission, capture flow = planCapture),
  `OptionsPanel`, `Settings` (cached `gui.*` keys, global: no `dev_<GUID>.` scope),
  `Formats` (lists only after the encoder probe), `MarvinIdleView` (Canvas port of
  `docs/logo.html`; the launcher icon in `res/drawable/ic_launcher_foreground.xml` is
  the still pose: keep both in step with the logo).
- Views are built in code (`Ui` helpers), there is no layout XML.

### Adding a native call

Add the C function in `marvin_jni.c` with `MJ_FN(ret, name)` (= `Java_jonascz_marvincapture_Core_name`)
and the `static native` declaration in `Core.java`. Strings go through `mj_string` /
`mj_utf8` (UTF-16; never `NewStringUTF` / `GetStringUTFChars`). A new status field goes
into one of the `getStatus` arrays: bump `N_*` in C and `ST_COUNT` etc. in `Core`.
R8 keeps `Core`'s native methods by name (proguard-rules.pro); nothing uses reflection.
Then `scripts/build-android-native.sh --abi x86_64` and the Gradle build.

### Threading contracts (do not break)

- Main thread: everything Java. The service's tick (5 Hz visible, 1 Hz otherwise) is
  the **only** caller of `pin_get_status` / `pin_poll_event` for the session (audio peaks
  are "since the previous read"); the activity gets the snapshot from it.
- `pin_close` blocks while files are finalised: it runs on the service's single
  "Core open/close" thread, after `audioDetach` and `previewDetach` (both synchronous),
  and the `UsbDeviceConnection` is closed only after it returned. Opens queue on the
  same thread.
- Render thread (`preview.c`): enters the core only through `enter_core()`;
  `previewDetach` and `previewSetSurface` wait for it (the latter until the old EGL
  surface is gone, so `surfaceDestroyed` may return).
- AAudio callback: only try-locks the monitor's lock; `audioDetach` takes it.
- No blocking work on the main thread: the encoder probe runs on a worker thread at
  start-up (`Formats.probe`); `pin_formats()` is only called after it.

### Gotchas

- Rotation and most config changes are handled in place (`configChanges` in the
  manifest), so the bound service, session and surface survive; `MainLayout` re-measures.
  A recreated activity would unbind and the service would close the device.
- The foreground service type is `connectedDevice` for USB and `mediaPlayback` for the
  replay device (no USB permission, so `connectedDevice` would throw on API 34+).
- `ACTION_USB_DEVICE_DETACHED` during a capture: the service waits for the core to end
  the capture (it finalises and reports `PIN_STOP_DEVICE_LOST`) before closing, else the
  capture-ended event is lost with the session.
- The output folder must exist before `pin_set_output_hint`, or the core cannot measure
  free space (the service creates it).
- Back: `OnBackInvokedCallback` from API 33 (`enableOnBackInvokedCallback="true"`),
  `onBackPressed` for API 30-32.

### Build and verify

```sh
scripts/build-android.sh --debug --abi x86_64        # natives + APK for the emulator
cd gui/android && ./gradlew assembleDebug -PmarvinAbis=x86_64 && ./gradlew lintDebug
```

No real hardware in the emulator: use the replay device
(`adb shell am start -n jonascz.marvincapture/.MainActivity --es replay <file in the app's
external files dir>`). Logs: `adb logcat -s MarvinCapture marvin-core`. A software
emulator renders slowly: `adb exec-out screencap` may return black frames, use
`adb emu screenrecord screenshot <abs path>.png` (landscape shots come unrotated).

