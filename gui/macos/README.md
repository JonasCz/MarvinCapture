# MarvinCapture (macOS)

Native front-end for the Pinnacle Studio 500-USB open driver, the macOS
counterpart of the [Windows app](../windows/README.md). It is a thin GUI over the
`marvin-core` C library: device list, option rules, file naming, status text and
settings all come from the core through `src/api/pin_api.h`. The app holds
widgets, the Metal preview, the audio monitor, the file dialogs, the menus, the
Dock integration and the window lifecycle.

- macOS 15 or later, Apple Silicon (arm64) only
- Swift 6.x (Swift 5 language mode), SwiftPM, AppKit for the app shell and
  SwiftUI inside the window
- Metal for the preview, AVAudioEngine for the audio monitor
- No Xcode needed: the Command Line Tools are enough

## Build

```sh
bash scripts/build.sh
```

builds the core, runs the tests, builds the GUI with SwiftPM (intermediates in
`build/macos-arm64/gui`) and assembles `build/macos-arm64/dist/MarvinCapture.app`
(bundle id `jonascz.MarvinCapture`, ad-hoc signed). `--skip-gui` leaves the app
out. The bundle holds the app binary, `libmarvin-core.dylib` and
`libusb-1.0.0.dylib` in `Contents/Frameworks`, `firmware/` in
`Contents/Resources`, the icon and a generated `Info.plist`; see
[docs/building.md](../../docs/building.md).

### Quick iteration

With the core already built, build only the Swift package:

```sh
swift build --package-path gui/macos --scratch-path build/macos-arm64/gui \
    -Xlinker -L$PWD/build/macos-arm64/core
```

`-Xlinker -L...` tells the linker where `libmarvin-core.dylib` is (the module
map only says `link "marvin-core"`). The resulting bare binary
(`swift build ... --show-bin-path`) finds the dylib through
`DYLD_LIBRARY_PATH`:

```sh
DYLD_LIBRARY_PATH=$PWD/build/macos-arm64/core \
    build/macos-arm64/gui/arm64-apple-macosx/debug/MarvinCapture
```

A bare binary has no bundle: no notifications, no firmware directory from the
bundle (the core searches next to itself), "About" shows version "development".
Use the `.app` for anything else.

## Run

```sh
open build/macos-arm64/dist/MarvinCapture.app
```

If `libmarvin-core.dylib` can't be loaded the process dies at launch (dyld); if
it reports an API version other than the one the app was built for, the window
shows an error instead of the UI.

From a terminal, run the binary inside the bundle, for command-line use and
`--debug` output (stdout is the terminal):

```sh
build/macos-arm64/dist/MarvinCapture.app/Contents/MacOS/MarvinCapture --help
```

`open MarvinCapture.app --args ...` passes the arguments but loses stdout (Launch
Services points it at /dev/null; the app detects that and stays quiet).

### Connection via USB hub

"⚠ connected via USB hub" under a device in the device list, and the matching
hint in the status bar's tooltip, mean the device is plugged in through a USB hub
and shares the hub's bandwidth with the other devices on it, which can drop
frames. Plug it directly into a USB port of the computer (not a hub, dock or
monitor). Some computers route their own ports through a built-in hub; then the
warning can be ignored. Hover over the entry for the same advice. (The core
reports it: `pin_device_info_t.hub_depth > 0`, text from `pin_usb_hub_hint()`;
see also the [main README](../../README.md#connection-via-usb-hub).)

## Command line

The core parses the command line (`pin_script_parse`), so every front-end accepts
the same language, documented in [docs/cli.md](../../docs/cli.md): settings and
actions processed left to right. The settings the window has fields for
(`--device`, `--input`, `--std`, `--format`, `--aspect`, `--split`, the analog
controls) are applied over the selected device's saved settings; `--title` and
`--keep-raw` have no field here and only affect the script's own captures. If
there are actions (`--rew`, `--play`, `--capture PATH`, `--wait`, ...), they run
(`pin_script_run`) once the device is ready, and the window shows them like any
other capture. `--help` (or `-h`, `-?`) shows the option list in a sheet; so does
a usage error, with the error on top.

```sh
APP=build/macos-arm64/dist/MarvinCapture.app/Contents/MacOS/MarvinCapture

# open a specific device on the composite input, PAL, 16:9 anamorphic
$APP --device usb:1-4 --input composite --std pal --aspect 16:9

# DV tape: rewind, play, capture until the tape ends, then quit with the script's exit code
$APP --rew --wait --play --capture ~/Movies/tape07.dv --wait --exit-when-done
```

`--exit-when-done` quits the app (without asking) when the steps finish, with
the exit code of [docs/cli.md](../../docs/cli.md). `--debug` sets the core's log
level to debug and mirrors the core's log lines and every engine event to stdout
(line buffered; skipped when stdout is /dev/null or closed).

## Dock

The Dock icon is the counterpart of the Windows taskbar button. A progress bar
runs along the bottom of the icon (mode and value come from the core,
`pin_status_progress`; `Services/DockTile.swift` only draws). A Dock tile is only
repainted on `display()` and does not animate, so every state is static:

| State | Dock icon |
|---|---|
| Device initialisation | green bar, same value as the in-window progress bar (full bar at 40 % opacity if unmeasurable) |
| Capturing, no signal, "no signal" timeout set | green bar counting down from full to empty |
| Capturing with "Stop after (min)" | green bar, elapsed / limit of the current pass |
| Capturing, no limit | full bar at 40 % opacity (indeterminate) |
| Capturing, waiting for signal (no timeout), or rewinding between passes | yellow, full |
| Capturing and the core reports low disk space (under 1 hour or 50 GB left) | yellow; keeps the value of a countdown / time limit bar, else full. Beaten only by an error |
| Stopping / finalising | full bar at 40 % opacity (indeterminate) |
| Device error, or an error banner open | red; clears when the banner is dismissed, or when the next capture starts |
| Idle / ready | plain icon |

The tile is redrawn only when the mode, the badge or the fraction (by 1 %)
changed. It is reset on quit.

While a capture runs (including stopping and rewinding between passes) the icon
carries a red "REC" badge.

When a capture finishes or fails while the app is in the background, the Dock icon
bounces once (normal end) or until the app is activated (abnormal end, or no video
received), and a user notification is posted in the same situations
(see Notifications). The abnormal-end alert is a sheet that does not take the focus
from the other app.

### Dock menu

| Item | Does | Enabled |
|---|---|---|
| Start Capture | the main start action of the current mode, through the same code as the in-window button (output checks, free-space / overwrite dialogs; the window is brought to the front if a dialog is needed): analog = Capture, DV/HDV = Manual capture ("Automatic rewind & capture" stays in the window / the Capture menu) | idle and the in-window button is enabled |
| Stop Capture | `pin_capture_stop_ex(PIN_STOP_DECK_AS_STARTED)`: a manual capture leaves the tape running, an automatic rewind & capture stops it | while a capture can be stopped (not while finalising) |

## Notifications

A capture that ended while the app is not the active application posts a user
notification (capture finished, stopped abnormally, no video received). The
authorisation is requested at the first capture start from the window, not at
launch. Notifications need the `.app` bundle; a bare binary has none.

## Capture buttons

DV/HDV has two buttons. **Manual capture** records what the camera or deck is sending
without any deck command; it is enabled when the core is READY and a video signal is
arriving (otherwise its tooltip says why). **Automatic rewind & capture** rewinds, plays,
captures and stops the tape at the end; it needs a camera. While a capture runs:

| Button | Title | Enabled | Does |
|---|---|---|---|
| Manual | Stop capture | while a stop is allowed, however the capture was started | stops without a deck command |
| Auto | Stop capture & stop tape (red) | any capture, with a camera (a manual one too) | stops the capture and the tape |

While the core finalises the file both show its state text and are disabled. Only the
button the capture was started with has a second line: "Stopping in 4m12s" (manual),
"Stopping capture and tape in 4m12s" (automatic, last pass) or "Next pass in 4m12s (pass 1 of 2)",
each with ", no signal" while the no-signal timeout is the running countdown, else the time
limit; none while rewinding between passes or with no limit. The analog button reads
"Capture" with "Capture S-Video input to file" (or composite) and becomes "Stop capture"
with the same countdown line.

## Menus

The menu bar (`App/MainMenu.swift`) has MarvinCapture, File (New Window, Choose
Output Folder, Close), Capture (rebuilt for the current input each time it opens),
Deck, Input, View (mute, full screen), Window and Help (command-line help, project
website). Enable rules are the model's, i.e. the core's, the same as for the
buttons. The DV/HDV Capture menu has Manual Capture, Automatic Rewind & Capture, then Stop Capture (no deck command) and Stop Capture & Stop Tape (any capture, with a camera). About is in the MarvinCapture menu; New Window and Command-Line Help are in File and Help (there is no "…" button in the window).

There are only the standard macOS shortcuts (New Window, Close, Quit, Hide, Hide
Others, Minimize, full screen, Help), on purpose: a stray key must never start or
stop a capture or move the tape. Controls have accessibility labels.

## Keep awake

While the core is capturing, stopping or rewinding, the app holds a
`PreventUserIdleSystemSleep` assertion ("Capturing video"; the display may still
sleep) and disables sudden termination, so a logout or shutdown asks the app first.
Both are released when the capture ends and on quit. Check with
`pmset -g assertions`.

## Windows

Each window runs as its own process: File > New Window
starts another instance of the app, for example to use a second device. Only
the first instance saves the window frame (`NSWindow` frame autosave
`MarvinCaptureMain`); later instances read it, offset it by 30 pt per older
instance (wrapping after 8) and never write it. Closing or quitting while a
capture runs asks first; confirming stops the capture, shows "Finalizing
files…" and quits once the core reports READY.

## Settings

The settings file is shared with the command-line program:
`~/Library/Application Support/PinnacleOSS/settings.ini`. As on Windows, options
are per device (`dev_<GUID>.gui.*`); the last used device and mute are global.
The window frame is stored by AppKit, not in that file.

## Layout

```
Package.swift                SwiftPM manifest (macOS 15, Swift 5 language mode)
Resources/AppIcon.iconset   the app icon (the logo) as PNGs; scripts/make-icons.py renders them, build.sh packs them
Sources/
  CMarvinCore/               module map exposing pin_api.h (no copy) and linking marvin-core
  MarvinCapture/
    App/                     main.swift (AppKit entry), AppDelegate, MainWindowController, MainMenu,
                             CloseFlow, CaptureFlow, Alerts (sheets), NewWindow, Startup probe, About,
                             FolderPicker, SnapshotHook
    Core/                    Pin (thin Swift layer over the C API), CoreString (char[N] fields)
    Models/                  @Observable: WindowModel (all window state and logic), AppModel (command
                             line, launch), KindSettingsModel, DeviceItem, ControlSliderModel, FormatItem
    Services/                MediaSeams (PreviewSink / AudioMonitor protocols), AudioMonitor (AVAudioEngine),
                             DockTile, Notifier, KeepAwake, ConsoleOutput
    Preview/                 PreviewRenderer (render thread, textures, pacing), PreviewShaders (MSL source,
                             compiled at run time), MetalPreview (view, sink, visibility)
    Views/                   SwiftUI: MainView, Sidebar/, StatusBar/, PreviewArea, MarvinIdleView (the animated logo on the
                             no-signal screen, Canvas), InfoBanner, HelpSheet, ...
```

## Notes

- Look: Finder-like. The title bar is transparent (`fullSizeContentView`) and the sidebar is one pane on the
  system sidebar material (`NSVisualEffectView`, `.sidebar`) running up under it; sections are separated by
  hairlines, not boxes. All outer paddings are 16 pt (`MainView.padding`).
- The C API is imported through the module `CMarvinCore`; there are no
  hand-written mirrors of structs or enums (unlike `Interop/` on Windows).
- The preview has a dedicated render thread that blocks in `pin_preview_wait()`
  and presents on the view's display link, only while frames are queued. Without
  a session, or while the window is minimised, fully covered or the app is
  hidden, the thread sleeps and the core stops decoding (`pin_preview_enable(0)`).
  The model detaches the preview before `pin_close`, and `detach()` waits until the
  thread is out of the core.
- The audio monitor pumps `pin_monitor_read` into an `AVAudioSourceNode`. The
  render block try-locks the session pointer, `detach()` takes the lock, so the
  model may `pin_close` right after. It is muted by default and then holds no audio
  device.
- Status and events use one 100 ms timer (`pin_get_status` plus draining
  `pin_poll_event`) and a 30 Hz timer for the meters. The device list is driven
  by a background thread blocked in `pin_devices_wait` (plug / unplug, or another
  window taking or releasing a device), not by polling.
- The status bar drops items in the same order as on Windows when the window is
  narrow: free space / time left, frames, storage, time, deck; the file text just
  trims (minimum window width 1000 pt).
- Bundle layout and signing: the firmware sits in `Contents/Resources/firmware`
  (non-code files in `Contents/Frameworks` break `codesign`). The core is found
  through the rpath `@executable_path/../Frameworks`.
- The Metal shader is compiled at run time from source: the Command Line Tools
  have no Metal compiler. The icon comes from `iconutil` (no `actool` either).

## Developer aids

Environment variables, all harmless in a normal run:

- `MARVIN_SNAPSHOT=/path/file.png` renders the main window's content view into
  that PNG every 2 seconds (works without screen-recording permission, also while
  the window is behind others); `MARVIN_SNAPSHOT_QUIT=seconds` quits the app after
  that long, with a last snapshot.
- `MARVIN_PREVIEW_DUMP=/path/frame.png` writes the first frame the Metal preview
  presents to that PNG once (the snapshot hook cannot capture a CAMetalLayer): the
  real shader rendered into an offscreen texture and read back, plus
  `/path/frame.png.cpu.png`, the same picture converted on the CPU from the core's
  planes.
- `MARVIN_PREVIEW_IGNORE_OCCLUSION=1` keeps the preview running although macOS
  reports the window as occluded (locked or sleeping screen).
- `MARVIN_UNMUTE=1` starts unmuted for this run without saving `gui.muted` (the
  default is muted, which keeps the audio engine stopped and no output device
  held). With `--debug` the audio monitor prints one statistics line per second
  (callbacks, frames asked / given by the core, short reads, underruns).
- `MARVIN_MENU_DUMP=1` prints the menu bar and Dock menu tree (title, shortcut,
  `[disabled]`, `[checked]`, `[hidden]`) to stdout once the session is READY (at the
  latest after 8 s), after validation, plus the window's first responder (it must
  be `NSWindow`: no control starts focused). Window-bound items (Close, Minimize,
  Zoom, full screen) show as disabled when the window is not key (locked screen,
  background).
- `MARVIN_DOCK_DUMP=/path/dock.png` writes the Dock tile view to PNG on every
  visible change: `dock.png` (latest) and `dock-0001.png`, ..., each with a `.txt`
  (mode, fraction, badge). The Dock draws the badge itself, so the dump paints an
  approximation of it.
- `MARVIN_WINDOW_SIZE=WxH` forces the window's content size and lifts the minimum,
  to look at narrow layouts and take snapshots of a known size.
- `MARVIN_APPEARANCE=dark|light` forces the window's appearance (the
  `-AppleInterfaceStyle` argument is not honoured on every AppKit path).
- `MARVIN_UPDATE_URL=<http(s) URL or file path>` overrides where the core looks for the
  latest version (a VERSION JSON); empty disables the update check. For testing the
  "update available" banner, point it at a local file such as
  `{"version":"9.9","release_notes":"Test","download_url":"https://github.com/JonasCz/MarvinCapture/releases/latest"}`.
- `MARVIN_SECONDARY=1` makes this process behave as a later instance (it never
  writes the saved window frame and cascades). "New Window" sets it for the process
  it starts, which is how a bare binary (no bundle id) knows.

## Distribution

The app is ad-hoc signed (`codesign --sign -`): it runs on the Mac that built it. A
copy that is downloaded (browser, AirDrop, chat) carries the quarantine attribute and
Gatekeeper refuses it as from an unidentified developer. Right-click the app and
choose Open (once), or remove the attribute:

```sh
xattr -dr com.apple.quarantine MarvinCapture.app
```

Notarisation needs an Apple Developer ID and is not done.
