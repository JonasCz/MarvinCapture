---
name: gui-changes
description: Make changes to the Pinnacle Capture GUI (WinUI 3 / C#, gui/windows/MarvinCaptureGUI): where things live, how settings and capture options flow, the core-first rule, build/lock gotchas. Use before editing GUI XAML, view models or the native interop.
---

# GUI changes

## Core-first rule

Each platform gets its own GUI codebase (today only `gui/windows/`, a C# WinUI 3
app; macOS/Linux GUIs will be separate code). So **put every piece of logic that
isn't UI in the core** (`src/engine`, `src/api`) and keep the per-platform code
to widgets, dialogs, bindings and P/Invoke. Before writing C# logic, ask
"would the macOS GUI need this too?". If yes, it goes in the core and the GUI
calls it through `pin_api.h`. Examples already done that way: output naming
and collision/free-space checks (`pin_check_output`), file name validation
(`pin_naming_validate`), settings, command-line parsing, scene splitting.

Core change checklist: function in `src/engine/*.c` + declaration in its
header, expose through `src/api/pin_api.[ch]` if the GUI needs it, add a test in
`tests/engine/` (registered in `src/engine/CMakeLists.txt`), and mirror any new
API/struct/enum in `Interop/Native*.cs`. Prefer reusing an existing call (e.g.
`pin_check_output` already returns a message the GUI shows) over a new export.

## Where things are (gui/windows/MarvinCaptureGUI)

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
  `pin_capture_opts_t` must be added in the same order/size in C and C#.
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
snapshot; counters (`frames_error`, `clip_*`, `total_bytes_written`,
`est_seconds_left`) and the format label (`video_label`) are the core's, never
computed in C#. Low-disk uses two copies of the storage elements toggled by
`DiskLow`. Minimum window width is 1000 DIP.

## Debugging

- Crashes / unhandled exceptions: `%LOCALAPPDATA%\PinnacleOSS\crash.log`
  (absent = clean run). If the core DLL fails to load or reports an API
  version other than 3, the app shows an error window instead of the UI.
- Quick GUI-only build: `dotnet build gui\windows\MarvinCaptureGUI -c Debug
  -p:Platform=x64`; the run target is `bind\Debug
et10.0-windows10.0.19041.0\win-x64\`
  and needs `marvin-core.dll` copied from `build\core` (done by the csproj).
- Hardware/replay testing of capture behaviour: see test-with-hardware-device.
- Packages: WindowsAppSDK 2.5.1, CommunityToolkit.Mvvm 8.4.2, Vortice D3D11.
  App is unpackaged, self-contained, x64, JIT (field-style `[ObservableProperty]`
  is fine).

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
  `MainViewModel` (`DeckRewEnabled` etc., driven by `DeckState`); any new
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
app is running from `build\dist`: the code compiled fine, but ask the user to
close it (don't kill it) and re-run. Don't run `ctest` directly from
`build\core` (exit 0xc0000139 = DLL path not set); the script sets PATH.

Commit and push straight to main (single-developer repo).
