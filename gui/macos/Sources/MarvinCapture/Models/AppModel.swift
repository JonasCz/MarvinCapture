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

import Foundation
import Observation
import CMarvinCore

/// Process-level state: the command line (parsed by the core, pin_script_parse), the one window's
/// model, and what the view layer shows because of the launch (help text, parse error, alerts).
/// Port of App.xaml.cs plus MainWindow.ParseCommandLine.
@MainActor @Observable
final class AppModel {
    let window = WindowModel()

    /// The command line asked for help (-h, --help, -?): show the help text in a sheet.
    private(set) var helpRequested = false
    /// A usage error from the parser (one line naming the offending argument): shown above the help text.
    private(set) var launchError: String?
    /// Set while the help sheet should be open (the view clears it).
    var showHelp = false
    /// An abnormal capture end the view shows as an alert (the view clears it).
    var alert: ModelAlert?

    /// The full help text (pin_script_help), always in step with the core in use.
    var helpText: String { Pin.scriptHelp() }

    @ObservationIgnored private let script: Pin.Script?

    init(arguments: [String] = CommandLine.arguments) {
        _ = ConsoleOutput.enabled   // sets up line buffering and ignores SIGPIPE before the first write
        let args = Self.userArguments(Array(arguments.dropFirst()))

        var parsed: Pin.Script?
        if args.contains(where: { $0 == "/?" || $0 == "-?" }) {
            helpRequested = true
        } else if !args.isEmpty {
            // No arguments at all is a normal start (the core would answer that with its help text).
            let r = Pin.parseScript(args)
            if r.status != PIN_OK || r.script == nil {
                launchError = r.error.isEmpty ? Pin.strerror(r.status) : r.error
            } else if r.script!.helpRequested {
                helpRequested = true
            } else {
                parsed = r.script
            }
        }
        script = parsed
        if let s = parsed, s.debug {
            Pin.setLogLevel(0)   // ConsoleOutput mirrors the core's debug lines
        }
        showHelp = helpRequested || launchError != nil
        window.configureLaunch(script: parsed)
        window.onAlert = { [unowned self] in alert = $0 }
    }

    /// Drops what AppKit and Launch Services add to argv: `-NS...` / `-Apple...` (each followed by its
    /// value) and `-psn_...`.
    static func userArguments(_ args: [String]) -> [String] {
        var out: [String] = []
        var i = 0
        while i < args.count {
            let a = args[i]
            if a.hasPrefix("-psn_") { i += 1; continue }
            if a.hasPrefix("-NS") || a.hasPrefix("-Apple") { i += 2; continue }
            out.append(a)
            i += 1
        }
        return out
    }

    /// Starts the window model (device list, open, timers, device watch). Call once the window is up.
    func start() { window.start() }

    /// Stops everything and closes the session (pin_close blocks until files are finalised).
    func shutdown() { window.shutdown() }
}
