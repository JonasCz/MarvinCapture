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
import Darwin
import CMarvinCore

/// Mirrors every engine event and core log line to stdout (port of ConsoleOutput.cs). On macOS stdout
/// just works when the app is started from a terminal or with the output redirected; started from
/// Finder it points at /dev/null, which is detected and skipped. `--debug` adds the core's debug lines
/// (AppModel sets the log level).
enum ConsoleOutput {
    private static let lock = NSLock()
    nonisolated(unsafe) private static var disabled = false

    /// stdout is a terminal, a file, a pipe or a socket (not /dev/null, not closed).
    static let enabled: Bool = {
        signal(SIGPIPE, SIG_IGN)               // a closed pipe must not kill the app
        setvbuf(stdout, nil, _IOLBF, 0)        // line buffered, also into files and pipes
        var st = stat()
        guard fstat(STDOUT_FILENO, &st) == 0 else { return false }
        if isatty(STDOUT_FILENO) != 0 { return true }
        switch st.st_mode & S_IFMT {
        case S_IFREG, S_IFIFO, S_IFSOCK: return true
        default: return false
        }
    }()

    private static let timeFormatter: DateFormatter = {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "HH:mm:ss.SSS"
        return f
    }()

    static func write(event e: Pin.Event) {
        guard enabled else { return }
        let text = e.text.trimmingCharacters(in: .whitespacesAndNewlines)
        if e.kind == PIN_EVT_LOG {
            write("\(levelName(e.a)): \(text)")
        } else {
            write("event \(kindName(e.kind)) a=\(e.a)" + (text.isEmpty ? "" : " \(text)"))
        }
    }

    static func write(_ line: String) {
        guard enabled else { return }
        lock.lock()
        defer { lock.unlock() }
        if disabled { return }
        let ts = timeFormatter.string(from: Date())
        if fputs("[\(ts)] \(line)\n", stdout) < 0 { disabled = true }   // pipe went away
    }

    private static func levelName(_ level: Int) -> String {
        switch level {
        case 0: "debug"
        case 1: "info"
        case 2: "warning"
        default: "error"
        }
    }

    /// The event kind as the C# enum prints it (State, FileOpened, ...).
    private static func kindName(_ k: pin_event_kind_t) -> String {
        switch k {
        case PIN_EVT_STATE: "State"
        case PIN_EVT_FILE_OPENED: "FileOpened"
        case PIN_EVT_FILE_CLOSED: "FileClosed"
        case PIN_EVT_SCENE: "Scene"
        case PIN_EVT_PASS: "Pass"
        case PIN_EVT_DECK: "Deck"
        case PIN_EVT_INPUT_FORMAT: "InputFormat"
        case PIN_EVT_LOG: "Log"
        case PIN_EVT_ERROR: "Error"
        case PIN_EVT_DONE: "Done"
        case PIN_EVT_DEVICES: "Devices"
        case PIN_EVT_CAPTURE_ENDED: "CaptureEnded"
        case PIN_EVT_STEP: "Step"
        case PIN_EVT_NO_VIDEO: "NoVideo"
        default: "\(k.rawValue)"
        }
    }
}
