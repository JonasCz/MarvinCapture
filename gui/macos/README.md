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
- `Sources/MarvinCapture/Services` console output, notifications, keep-awake, preview/audio seams.
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
- `--debug` mirrors the core's debug log and all engine events to stdout (line buffered; skipped when stdout is /dev/null).
- Capture-end notifications (user notifications while the app is in the background) only work from the .app bundle.
