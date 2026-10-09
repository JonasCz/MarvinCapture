// Pinnacle Studio 500-USB open driver
// Copyright (C) 2026 Jonas Cz.
//
// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or (at your
// option) any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License
// for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
import AppKit
import CMarvinCore

/// The menu bar and the Dock menu, built in AppKit. Standard items go to the responder chain (Hide,
/// Quit, Minimize, full screen, ...); the app's own items target this controller, which talks to the
/// model: the same code paths as the buttons (`CaptureFlow` for starts, the model's stop calls), and
/// every enable rule is the model's, i.e. the core's (`captureEnabled`, `deckPlayEnabled`, ...).
///
/// Only the standard macOS shortcuts exist (New Window, Close, Quit, Hide, Minimize, full screen,
/// Help): a stray key must never start or stop a capture or move the tape.
@MainActor
final class MenuController: NSObject, NSMenuDelegate, NSMenuItemValidation {
    static let websiteURL = URL(string: "https://github.com/JonasCz/MarvinCapture")!

    /// nil when startup failed (no model): only New Window, About and the standard items work then.
    private let app: AppModel?
    private var model: WindowModel? { app?.window }

    private let captureMenu = NSMenu(title: "Capture")
    private(set) var mainMenu = NSMenu()

    init(app: AppModel?) {
        self.app = app
        super.init()
    }

    // MARK: building

    private func item(_ title: String, _ action: Selector?, key: String = "",
                      mods: NSEvent.ModifierFlags = .command, tag: Int = 0, target: AnyObject? = nil) -> NSMenuItem {
        let i = NSMenuItem(title: title, action: action, keyEquivalent: key)
        if !key.isEmpty { i.keyEquivalentModifierMask = mods }
        i.tag = tag
        // Standard items have no target (the responder chain finds NSApplication / NSWindow); ours do.
        i.target = target
        return i
    }

    private func own(_ title: String, _ action: Selector, key: String = "",
                     mods: NSEvent.ModifierFlags = .command, tag: Int = 0) -> NSMenuItem {
        item(title, action, key: key, mods: mods, tag: tag, target: self)
    }

    @discardableResult
    private func topLevel(_ menu: NSMenu) -> NSMenuItem {
        let i = NSMenuItem(title: menu.title, action: nil, keyEquivalent: "")
        i.submenu = menu
        mainMenu.addItem(i)
        return i
    }

    /// Builds the menu bar and installs it (including the Services, Window and Help roles).
    func install() {
        let name = "MarvinCapture"
        mainMenu = NSMenu()

        // MarvinCapture
        let appMenu = NSMenu(title: name)
        appMenu.addItem(own("About \(name)", #selector(showAbout(_:))))
        appMenu.addItem(.separator())
        let services = NSMenu(title: "Services")
        let servicesItem = NSMenuItem(title: "Services", action: nil, keyEquivalent: "")
        servicesItem.submenu = services
        appMenu.addItem(servicesItem)
        NSApp.servicesMenu = services
        appMenu.addItem(.separator())
        appMenu.addItem(item("Hide \(name)", #selector(NSApplication.hide(_:)), key: "h"))
        appMenu.addItem(item("Hide Others", #selector(NSApplication.hideOtherApplications(_:)), key: "h",
                             mods: [.command, .option]))
        appMenu.addItem(item("Show All", #selector(NSApplication.unhideAllApplications(_:))))
        appMenu.addItem(.separator())
        appMenu.addItem(item("Quit \(name)", #selector(NSApplication.terminate(_:)), key: "q"))
        topLevel(appMenu)

        // File
        let file = NSMenu(title: "File")
        file.addItem(own("New Window", #selector(newWindow(_:)), key: "n"))
        file.addItem(.separator())
        file.addItem(own("Choose Output Folder…", #selector(chooseOutputFolder(_:))))
        file.addItem(.separator())
        // performClose goes to the window delegate, which runs the close-while-capturing flow.
        file.addItem(item("Close", #selector(NSWindow.performClose(_:)), key: "w"))
        topLevel(file)

        // Capture: rebuilt for the current input whenever it opens (menuNeedsUpdate).
        captureMenu.delegate = self
        captureMenu.autoenablesItems = true
        topLevel(captureMenu)
        rebuildCaptureMenu()

        // Deck
        let deck = NSMenu(title: "Deck")
        deck.addItem(own("Play", #selector(deckCommand(_:)), tag: Self.tag(.play)))
        deck.addItem(own("Stop", #selector(deckCommand(_:)), tag: Self.tag(.stop)))
        deck.addItem(own("Rewind", #selector(deckCommand(_:)), tag: Self.tag(.rew)))
        deck.addItem(own("Fast Forward", #selector(deckCommand(_:)), tag: Self.tag(.ff)))
        topLevel(deck)

        // Input
        let input = NSMenu(title: "Input")
        input.addItem(own("DV / HDV", #selector(selectInput(_:)), tag: 0))
        input.addItem(own("S-Video", #selector(selectInput(_:)), tag: 1))
        input.addItem(own("Composite", #selector(selectInput(_:)), tag: 2))
        topLevel(input)

        // View
        let view = NSMenu(title: "View")
        view.addItem(own("Mute Audio Monitor", #selector(toggleMute(_:))))
        view.addItem(.separator())
        // AppKit flips the title to "Exit Full Screen" itself.
        view.addItem(item("Enter Full Screen", #selector(NSWindow.toggleFullScreen(_:)), key: "f",
                          mods: [.command, .control]))
        topLevel(view)

        // Window
        let window = NSMenu(title: "Window")
        window.addItem(item("Minimize", #selector(NSWindow.performMiniaturize(_:)), key: "m"))
        window.addItem(item("Zoom", #selector(NSWindow.performZoom(_:))))
        window.addItem(.separator())
        window.addItem(item("Bring All to Front", #selector(NSApplication.arrangeInFront(_:))))
        topLevel(window)
        NSApp.windowsMenu = window

        // Help (setting helpMenu adds the search field)
        let help = NSMenu(title: "Help")
        help.addItem(own("\(name) Command-Line Help", #selector(showCommandLineHelp(_:)), key: "?"))
        help.addItem(own("Project Website", #selector(openWebsite(_:))))
        topLevel(help)
        NSApp.helpMenu = help

        NSApp.mainMenu = mainMenu
    }

    // MARK: capture menu (rebuilt per input)

    private func rebuildCaptureMenu() {
        captureMenu.removeAllItems()
        guard let m = model else { return }
        if m.isDvInput {
            captureMenu.addItem(own("Manual Capture", #selector(manualCapture(_:))))
            captureMenu.addItem(own("Automatic Rewind & Capture", #selector(automaticCapture(_:))))
            captureMenu.addItem(.separator())
            captureMenu.addItem(own("Stop Capture", #selector(stopContinueTape(_:))))
            captureMenu.addItem(own("Stop Capture & Stop Tape", #selector(stopStopTape(_:))))
        } else {
            captureMenu.addItem(own(m.isCapturing ? "Stop Capture" : "Capture", #selector(analogCapture(_:))))
        }
    }

    func menuNeedsUpdate(_ menu: NSMenu) {
        if menu === captureMenu { rebuildCaptureMenu() }
    }

    // MARK: Dock menu

    /// "Start Capture" / "Stop Capture": the taskbar thumbnail buttons of the Windows app.
    func makeDockMenu() -> NSMenu {
        let menu = NSMenu(title: "Dock")
        menu.addItem(own("Start Capture", #selector(dockStart(_:))))
        menu.addItem(own("Stop Capture", #selector(dockStop(_:))))
        return menu
    }

    // MARK: actions

    private static func tag(_ c: DeckCommand) -> Int {
        switch c { case .play: 1; case .pause: 2; case .stop: 3; case .ff: 4; case .rew: 5 }
    }
    private static func command(forTag t: Int) -> DeckCommand? {
        switch t { case 1: .play; case 2: .pause; case 3: .stop; case 4: .ff; case 5: .rew; default: nil }
    }

    @objc private func showAbout(_ sender: Any?) { About.show() }
    @objc private func newWindow(_ sender: Any?) { NewWindow.open(reportingTo: model) }
    @objc private func chooseOutputFolder(_ sender: Any?) { model?.chooseOutputFolder() }

    @objc private func analogCapture(_ sender: Any?) { if let m = model { CaptureFlow.captureClicked(m) } }
    @objc private func manualCapture(_ sender: Any?) { if let m = model, !m.isCapturing { CaptureFlow.start(m, playFirst: false) } }
    @objc private func automaticCapture(_ sender: Any?) { if let m = model, !m.isCapturing { CaptureFlow.start(m, playFirst: true) } }
    @objc private func stopContinueTape(_ sender: Any?) { if let m = model, m.isCapturing { m.stopCapture(stopDeck: false) } }
    @objc private func stopStopTape(_ sender: Any?) { if let m = model, m.isCapturing { m.stopCapture(stopDeck: true) } }

    @objc private func deckCommand(_ sender: NSMenuItem) {
        if let c = Self.command(forTag: sender.tag) { model?.userRequestedDeck(c) }
    }
    @objc private func selectInput(_ sender: NSMenuItem) {
        if let m = model, m.isIdle { m.inputIndex = sender.tag }
    }
    @objc private func toggleMute(_ sender: Any?) { model?.isMuted.toggle() }
    @objc private func showCommandLineHelp(_ sender: Any?) { app?.openHelpFromMenu() }
    @objc private func openWebsite(_ sender: Any?) { NSWorkspace.shared.open(Self.websiteURL) }

    @objc private func dockStart(_ sender: Any?) {
        guard let m = model, m.dockStartEnabled else { return }
        CaptureFlow.start(m, playFirst: false)   // dialogs bring the window forward themselves
    }
    @objc private func dockStop(_ sender: Any?) {
        guard let m = model, m.dockStopEnabled else { return }
        m.stopCaptureAsStarted()
    }

    // MARK: validation

    func validateMenuItem(_ item: NSMenuItem) -> Bool {
        guard let action = item.action else { return false }
        if action == #selector(showAbout(_:)) || action == #selector(newWindow(_:)) || action == #selector(openWebsite(_:)) {
            return true
        }
        guard let m = model else { return false }
        item.state = .off
        switch action {
        case #selector(chooseOutputFolder(_:)): return m.isIdle
        case #selector(analogCapture(_:)): return m.captureEnabled
        case #selector(manualCapture(_:)): return !m.isCapturing && m.playAndCaptureEnabled
        case #selector(automaticCapture(_:)): return !m.isCapturing && m.dvAutoCaptureEnabled
        case #selector(stopContinueTape(_:)): return m.isCapturing && m.playAndCaptureEnabled
        case #selector(stopStopTape(_:)): return m.isCapturing && m.dvAutoCaptureEnabled
        case #selector(deckCommand(_:)):
            guard m.isDvInput, let c = Self.command(forTag: item.tag) else { return false }
            switch c {
            case .play: item.state = m.isPlayChecked ? .on : .off; return m.deckPlayEnabled
            case .stop: item.state = m.isStopChecked ? .on : .off; return m.deckStopEnabled
            case .rew: item.state = m.isRewChecked ? .on : .off; return m.deckRewEnabled
            case .ff: item.state = m.isFfChecked ? .on : .off; return m.deckFfEnabled
            case .pause: return false
            }
        case #selector(selectInput(_:)):
            item.state = m.inputIndex == item.tag ? .on : .off
            return m.isIdle
        case #selector(toggleMute(_:)):
            item.state = m.isMuted ? .on : .off
            return true
        case #selector(showCommandLineHelp(_:)): return true
        case #selector(dockStart(_:)): return m.dockStartEnabled
        case #selector(dockStop(_:)): return m.dockStopEnabled
        default: return true
        }
    }

    // MARK: developer aid

    /// `MARVIN_MENU_DUMP=1`: the menu tree (and the Dock menu) with enabled / checked state, printed once
    /// after startup, after validation as the menus would show it when opened.
    func dump() -> String {
        var out = ""
        func walk(_ menu: NSMenu, _ depth: Int) {
            menu.delegate?.menuNeedsUpdate?(menu)
            menu.update()
            for i in menu.items {
                let pad = String(repeating: "  ", count: depth)
                if i.isSeparatorItem { out += pad + "---\n"; continue }
                var flags: [String] = []
                if !i.isEnabled { flags.append("disabled") }
                if i.state == .on { flags.append("checked") }
                if i.state == .mixed { flags.append("mixed") }
                if i.isHidden { flags.append("hidden") }
                var key = ""
                if !i.keyEquivalent.isEmpty {
                    let m = i.keyEquivalentModifierMask
                    key = " <" + (m.contains(.control) ? "ctrl+" : "") + (m.contains(.option) ? "opt+" : "")
                        + (m.contains(.shift) ? "shift+" : "") + (m.contains(.command) ? "cmd+" : "") + i.keyEquivalent + ">"
                }
                out += pad + i.title + key + (flags.isEmpty ? "" : "  [" + flags.joined(separator: ", ") + "]") + "\n"
                if let sub = i.submenu, sub !== NSApp.servicesMenu { walk(sub, depth + 1) }
            }
        }
        out += "== Menu bar\n"
        walk(mainMenu, 0)
        out += "== Dock menu\n"
        walk(makeDockMenu(), 0)
        return out
    }
}
