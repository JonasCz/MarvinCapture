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
- `Sources/MarvinCapture/Views` SwiftUI views.
- `Tools/make-icon.swift` draws the app icon at build time.

If `libmarvin-core.dylib` is missing or unloadable the process dies at launch (dyld); an API
version mismatch shows an error in the window instead.
