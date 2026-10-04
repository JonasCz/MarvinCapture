# MarvinCapture for macOS

Native SwiftUI/AppKit front end over marvin-core (`src/api/pin_api.h`). Requires macOS 15 and
the Xcode Command Line Tools (no Xcode). Swift 6.1 toolchain, Swift 5 language mode.

## Build and run

    bash scripts/build.sh            # core + tests + build/macos-arm64/dist/MarvinCapture.app
    open build/macos-arm64/dist/MarvinCapture.app
    build/macos-arm64/dist/MarvinCapture.app/Contents/MacOS/MarvinCapture   # from a shell, stderr visible

Package only (core already built): `swift build --package-path gui/macos --scratch-path build/macos-arm64/gui -Xlinker -L$PWD/build/macos-arm64/core`.
Running that binary outside the .app needs `DYLD_LIBRARY_PATH=build/macos-arm64/core`.

## Layout

- `Sources/CMarvinCore` module map exposing `pin_api.h` (no copy) and linking `marvin-core`.
- `Sources/MarvinCapture/App` entry point (AppKit main), `AppDelegate`, `MainWindowController`, startup probe.
- `Sources/MarvinCapture/Core` Swift helpers over the C API (`Pin`, `CoreString`).
- `Sources/MarvinCapture/Models` `@Observable` models: `WindowModel` (all window state and logic), `AppModel` (command line, launch), `KindSettingsModel`, `DeviceItem`, `ControlSliderModel`, `FormatItem`.
- `Sources/MarvinCapture/Services` console output, notifications, Dock tile (`DockTile.swift`), keep-awake, preview seam and the AVAudioEngine audio monitor (`AudioMonitor.swift`).
- `Sources/MarvinCapture/Preview` Metal preview: `PreviewRenderer` (render thread, textures, pacing), `PreviewShaders` (MSL source, compiled at run time), `MetalPreview` (view, sink, visibility).
- `Sources/MarvinCapture/Views` SwiftUI views.
- `Tools/make-icon.swift` draws the app icon at build time.

If `libmarvin-core.dylib` is missing or unloadable the process dies at launch (dyld); an API
version mismatch shows an error in the window instead.

## Developer aids

- `MARVIN_SNAPSHOT=/path/file.png` renders the main window's content view into that PNG every 2 seconds
  (works without screen-recording permission, also while the window is behind others);
  `MARVIN_SNAPSHOT_QUIT=seconds` quits the app after that long. Both are harmless in every build.
- `MARVIN_PREVIEW_DUMP=/path/frame.png` writes the first frame the Metal preview presents to that PNG once
  (the snapshot hook cannot capture a CAMetalLayer): the real shader rendered into an offscreen texture and
  read back, plus `/path/frame.png.cpu.png`, the same picture converted on the CPU from the core's planes.
- `MARVIN_PREVIEW_IGNORE_OCCLUSION=1` keeps the preview running although macOS reports the window as occluded (locked or sleeping screen).
- `MARVIN_UNMUTE=1` starts unmuted for this run without saving `gui.muted` (default is muted, which keeps the audio engine stopped and no output device held). With `--debug` the audio monitor prints one statistics line per second (callbacks, frames asked / given by the core, short reads, underruns).
- `--debug` mirrors the core's debug log and all engine events to stdout (line buffered; skipped when stdout is /dev/null).
- Capture-end notifications (user notifications while the app is in the background) only work from the .app bundle.
- `MARVIN_MENU_DUMP=1` prints the menu bar and Dock menu tree (title, shortcut, `[disabled]`, `[checked]`, `[hidden]`) to stdout once the session is READY (at the latest after 8 s), after validation, plus the window's first responder (it must be `NSWindow`: no control starts focused). Window-bound items (Close, Minimize, Zoom, full screen) show as disabled when the window is not key (locked screen, background).
- `MARVIN_DOCK_DUMP=/path/dock.png` writes the Dock tile view to PNG on every visible change: `dock.png` (latest) and `dock-0001.png`, ... each with a `.txt` (mode, fraction, badge). The Dock draws the badge itself, so the dump paints an approximation of it.

## Menus, Dock, windows

- Menu bar (`App/MainMenu.swift`, `MenuController`): only standard shortcuts (New Window, Close, Quit, Hide, Hide Others, Minimize, full screen, Help); no shortcut starts or stops a capture or moves the tape. The Capture menu is rebuilt for the current input each time it opens. Enable rules are the model's (the core's). The sidebar "…" menu calls the same actions.
- Dock tile: the icon with a progress bar (mode/fraction from `pin_status_progress`; red while an error banner is open). Static looks only, since a Dock tile redraws on `display()` only: green = fraction, yellow = paused (fraction or full), red = full, INDETERMINATE = full bar at 40 % opacity. Badge "REC" while capturing (including stopping and rewinding). `display()` runs only when the mode, badge or fraction (by 1 %) changed. Everything is cleared on quit.
- Dock menu: Start Capture (analog Capture, DV/HDV Manual Capture, with the usual dialogs) and Stop Capture (stops as started).
- Attention: a capture that ended or failed while the app is in the background bounces the Dock icon once (normal end) or until activated (abnormal end, no video), in exactly the situations where the user notification is posted. The abnormal-end alert is a sheet that no longer activates the app.
- New Window starts another process. The first instance autosaves the window frame; later instances (another process of the same bundle id is older, or `MARVIN_SECONDARY=1`, which New Window sets) read the saved frame, offset it by 30 pt per older instance (wrapping after 8) and never write it.
- Keep-awake: `KeepAwake` holds a `PreventUserIdleSystemSleep` assertion ("Capturing video") exactly while the state is capturing / stopping / rewinding, released otherwise and on quit. Check with `pmset -g assertions`.
