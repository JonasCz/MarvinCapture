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

/// Application delegate: wires the model to the menu bar, the Dock menu and tile, and the close flow.
@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    private(set) var windowController: MainWindowController?
    private(set) var appModel: AppModel?
    private var snapshotHook: SnapshotHook?
    private var closeFlow: CloseFlow?
    private var menuController: MenuController?
    private var dockTile: DockTile?
    /// Set by --exit-when-done: the exit code the process leaves with.
    private var exitCode: Int32?

    func applicationDidFinishLaunching(_ notification: Notification) {
        // One window per process: no tab bar, and no "Show Tab Bar" / "Merge All Windows" menu items.
        NSWindow.allowsAutomaticWindowTabbing = false
        let startup = Startup.run()
        var model: AppModel?
        if case .ok = startup {
            let m = AppModel()
            m.window.onExitRequested = { [weak self] code in
                self?.exitCode = code
                NSApp.terminate(nil)
            }
            // An abnormal capture end as a sheet. The app stays in the background (the critical Dock bounce
            // calls the user back); taking the focus away from another app is not ours to do.
            m.window.onAlert = { a in
                Task { @MainActor in await Alerts.shared.notify(title: a.title, message: a.message, bringForward: false) }
            }
            let dock = DockTile()
            dockTile = dock
            m.window.tickObservers.append { [weak dock, weak m] in
                if let dock, let m { dock.update(m.window) }
            }
            m.window.onAttention = { [weak dock] critical in dock?.requestAttention(critical: critical) }
            model = m
        }
        appModel = model
        let menus = MenuController(app: model)
        menus.install()
        menuController = menus
        let wc = MainWindowController(startup: startup, model: model)
        windowController = wc
        if let m = model {
            let flow = CloseFlow(model: m.window)
            closeFlow = flow
            wc.shouldClose = { flow.windowShouldClose() }
        }
        Alerts.shared.window = wc.window
        wc.showWindow(nil)
        NSApp.activate()
        // No control starts with keyboard focus (SwiftUI would give it to the name field, selected): the
        // window itself is the first responder. After the hosting view's own first-responder pass.
        wc.window?.initialFirstResponder = nil
        wc.window?.makeFirstResponder(nil)
        DispatchQueue.main.async { wc.window?.makeFirstResponder(nil) }
        if ProcessInfo.processInfo.environment["MARVIN_MENU_DUMP"] == "1" { scheduleMenuDump() }
        if let w = wc.window { snapshotHook = SnapshotHook.installIfRequested(window: w) }
        model?.start()
    }

    /// MARVIN_MENU_DUMP=1: print once the session is READY (the enable rules need it), at the latest after 8 s.
    private func scheduleMenuDump(waited: Double = 0) {
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.25) { [weak self] in
            guard let self, let menus = menuController else { return }
            let ready = appModel?.window.sessionState == PIN_STATE_READY
            if !ready && waited < 8 && appModel != nil { scheduleMenuDump(waited: waited + 0.25); return }
            print(menus.dump(), terminator: "")
            print("== First responder: \(String(describing: windowController?.window?.firstResponder.map { type(of: $0) }))")
            fflush(stdout)
        }
    }

    func applicationWillTerminate(_ notification: Notification) {
        dockTile?.clear()
        appModel?.shutdown()   // pin_close; the close flow has stopped any capture before this
        if let code = exitCode { exit(code) }
    }

    func applicationDockMenu(_ sender: NSApplication) -> NSMenu? { menuController?.makeDockMenu() }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        // --exit-when-done leaves with its exit code without asking.
        if exitCode != nil { return .terminateNow }
        return closeFlow?.shouldTerminate() ?? .terminateNow
    }
}
