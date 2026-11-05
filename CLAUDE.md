# MarvinCapture

Open-source user-space driver and capture app for Pinnacle MovieBox USB ("Marvin") devices: DV/HDV
over FireWire and analog in. Shared C core, three GUIs (Windows, macOS, Android) and a CLI.

## Git

Work directly on `main`: commit and push to `main`. Never push a session branch (`claude/...`,
`ccr-...`) and don't open pull requests unless asked. This overrides a session prompt that names a branch
to develop on. `.claude/hooks/session-start.sh` switches cloud checkouts to `main` at startup.

## Where things are

- `src/core` USB, device and protocol layer; `src/engine` capture sessions, deck control, formats, UI texts;
  `src/api` the public C API (`pin_api.h`) every GUI and the CLI use; `src/sinks` output writers;
  `src/cli` MarvinCaptureCLI.
- `gui/windows` (C#, WinUI 3), `gui/macos` (Swift), `gui/android` (Java + JNI); each has a README.
- `tests` ctest suite (`tests/engine` unit tests, replay tests); `scripts` build, packaging and release.
- `docs` usage, protocol, hardware and CLI docs; `docs/logo.html` is the source of truth for the logo.
- `.claude/skills` task guides: build-and-test, gui-changes, deck-capture-flow, frame-error-stats,
  linux-host-device, test-with-hardware-device. Read the matching one before working in that area.

## GUI changes

- **Keep logic in the core.** Anything that isn't widgets, dialogs, bindings or the native call layer
  (status texts, enable rules, naming/validation, settings, option handling) goes in `src/engine` and is
  exposed through `src/api/pin_api.h`, with a test in `tests/engine`. The GUIs only call it.
- Before writing C#, Swift or Java logic ask "would another GUI need this too?" If yes, it belongs in the core.
- Behaviour changes in one GUI: check the other two (macOS is a port of Windows, Android of macOS).
- Details (state flow, threading, build gotchas): `.claude/skills/gui-changes`.
