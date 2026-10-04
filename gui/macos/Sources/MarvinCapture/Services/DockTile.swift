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

/// The Dock icon as the taskbar button of the Windows app: a progress bar along the bottom of the icon
/// (mode and value from the core, `pin_status_progress`, red while an error banner is open, like
/// MainWindow.UpdateTaskbar) and a "REC" badge while a capture runs (the Windows red overlay dot).
/// Also owns the Dock bounce for a capture that ended in the background (FlashIfInBackground).
///
/// A Dock tile is only repainted on `display()`, and there is no run loop animation for it, so every
/// state is static: INDETERMINATE (capturing with no known end, stopping) is the full-width bar at 40 %
/// opacity instead of a moving one. `display()` is called only when something visibly changed: the mode,
/// the badge, or the fraction by at least 1 %.
@MainActor
final class DockTile {
    private let tile = NSApp.dockTile
    private let view = DockTileView()
    private var shownMode: pin_progress_mode_t?
    private var shownFraction = -1.0
    private var shownBadge: String?
    private var dump: DockDump?

    /// What the badge says while capturing. "REC" over a bare dot: it reads on its own, and the badge
    /// capsule is red already.
    static let capturingBadge = "REC"

    init() {
        dump = DockDump.fromEnvironment()
    }

    /// Mode, fraction and badge for the model's state right now (also used by the dump hook).
    static func state(of model: WindowModel) -> (mode: pin_progress_mode_t, fraction: Double, badge: String?) {
        var mode = model.progressMode
        var fraction = model.progressFraction
        // An open error banner (device / capture failure) shows red until dismissed.
        if model.infoOpen && model.infoSeverity == .error {
            mode = PIN_PROGRESS_ERROR
            fraction = 1
        }
        switch mode {
        case PIN_PROGRESS_ERROR: fraction = 1
        case PIN_PROGRESS_NORMAL, PIN_PROGRESS_PAUSED: fraction = min(max(fraction, 0), 1)
        default: fraction = 0
        }
        return (mode, fraction, model.isCapturing ? capturingBadge : nil)
    }

    /// Called on every status tick; cheap when nothing changed.
    func update(_ model: WindowModel) {
        let s = Self.state(of: model)
        let modeChanged = s.mode != shownMode
        let fractionChanged = abs(s.fraction - shownFraction) >= 0.01
        let badgeChanged = s.badge != shownBadge
        guard modeChanged || fractionChanged || badgeChanged else { return }
        shownMode = s.mode
        shownFraction = s.fraction
        shownBadge = s.badge

        view.mode = s.mode
        view.fraction = s.fraction
        view.frame = NSRect(origin: .zero, size: tile.size)
        // With no progress the plain system icon is used (no custom view to keep drawn).
        tile.contentView = s.mode == PIN_PROGRESS_NONE ? nil : view
        tile.badgeLabel = s.badge
        tile.display()
        dump?.write(view: view, mode: s.mode, fraction: s.fraction, badge: s.badge, size: tile.size)
    }

    /// Dock bounce: once for a normal end, until the app is activated for an abnormal one. Only while
    /// the app is in the background (AppKit ignores it otherwise, this just states it).
    func requestAttention(critical: Bool) {
        guard !NSApp.isActive else { return }
        NSApp.requestUserAttention(critical ? .criticalRequest : .informationalRequest)
    }

    /// Quit: leave the Dock as it was.
    func clear() {
        tile.contentView = nil
        tile.badgeLabel = nil
        tile.display()
    }
}

/// The icon with the progress bar. Drawn at whatever size the Dock tile has (128 pt usually).
final class DockTileView: NSView {
    var mode: pin_progress_mode_t = PIN_PROGRESS_NONE
    var fraction = 0.0

    override var isFlipped: Bool { false }

    override func draw(_ dirtyRect: NSRect) {
        NSApp.applicationIconImage.draw(in: bounds, from: .zero, operation: .sourceOver, fraction: 1)
        guard mode != PIN_PROGRESS_NONE else { return }

        let h = (bounds.height * 0.10).rounded()
        let bar = NSRect(x: (bounds.width * 0.14).rounded(), y: (bounds.height * 0.07).rounded(),
                         width: (bounds.width * 0.72).rounded(), height: h)
        let track = NSBezierPath(roundedRect: bar, xRadius: h / 2, yRadius: h / 2)
        NSColor.black.withAlphaComponent(0.6).setFill()
        track.fill()
        NSColor.white.withAlphaComponent(0.4).setStroke()
        track.lineWidth = 1
        track.stroke()

        var color = NSColor.systemGreen
        var amount = fraction
        switch mode {
        case PIN_PROGRESS_PAUSED: color = .systemYellow
        case PIN_PROGRESS_ERROR: color = .systemRed; amount = 1
        case PIN_PROGRESS_INDETERMINATE: color = NSColor.systemGreen.withAlphaComponent(0.4); amount = 1
        default: break
        }
        let inner = bar.insetBy(dx: 2, dy: 2)
        // At least a dot of fill once there is any (a 1 % sliver would not be a capsule).
        let w = amount > 0 ? max(inner.height, inner.width * amount) : 0
        guard w > 0 else { return }
        let fill = NSBezierPath(roundedRect: NSRect(x: inner.minX, y: inner.minY, width: w, height: inner.height),
                                xRadius: inner.height / 2, yRadius: inner.height / 2)
        color.setFill()
        fill.fill()
    }
}

/// Developer aid: `MARVIN_DOCK_DUMP=/path/dock.png` writes the dock tile view to PNG on every visible
/// change, as `dock.png` (latest) and `dock-0001.png`, `dock-0002.png`, ... with a `.txt` next to each
/// holding mode / fraction / badge. The badge is drawn by the Dock itself and is not part of the view,
/// so the dump paints an approximation of it (red capsule, top right) into the PNG.
@MainActor
final class DockDump {
    private let path: String
    private var count = 0

    static func fromEnvironment() -> DockDump? {
        guard let p = ProcessInfo.processInfo.environment["MARVIN_DOCK_DUMP"], !p.isEmpty else { return nil }
        return DockDump(path: p)
    }

    private init(path: String) { self.path = path }

    func write(view: DockTileView, mode: pin_progress_mode_t, fraction: Double, badge: String?, size: NSSize) {
        count += 1
        let w = Int(size.width), h = Int(size.height)
        guard let rep = NSBitmapImageRep(
            bitmapDataPlanes: nil, pixelsWide: w * 2, pixelsHigh: h * 2, bitsPerSample: 8, samplesPerPixel: 4,
            hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0),
              let ctx = NSGraphicsContext(bitmapImageRep: rep) else { return }
        rep.size = size
        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = ctx
        ctx.cgContext.scaleBy(x: 2, y: 2)   // 2x pixels for 1x points
        view.frame = NSRect(origin: .zero, size: size)
        view.draw(view.bounds)
        if let badge {
            let font = NSFont.systemFont(ofSize: size.height * 0.16, weight: .bold)
            let attrs: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: NSColor.white]
            let t = (badge as NSString).size(withAttributes: attrs)
            let bh = t.height + 4, bw = max(bh, t.width + 14)
            let r = NSRect(x: size.width - bw - 2, y: size.height - bh - 2, width: bw, height: bh)
            NSColor.systemRed.setFill()
            NSBezierPath(roundedRect: r, xRadius: bh / 2, yRadius: bh / 2).fill()
            (badge as NSString).draw(at: NSPoint(x: r.midX - t.width / 2, y: r.midY - t.height / 2), withAttributes: attrs)
        }
        NSGraphicsContext.restoreGraphicsState()
        guard let png = rep.representation(using: .png, properties: [:]) else { return }
        let base = (path as NSString).deletingPathExtension
        let numbered = String(format: "%@-%04d", base, count)
        let text = "mode=\(Self.modeName(mode)) fraction=\(String(format: "%.3f", fraction)) badge=\(badge ?? "none")\n"
        try? png.write(to: URL(fileURLWithPath: numbered + ".png"))
        try? text.write(toFile: numbered + ".txt", atomically: true, encoding: .utf8)
        try? png.write(to: URL(fileURLWithPath: path))
        try? text.write(toFile: path + ".txt", atomically: true, encoding: .utf8)
    }

    private static func modeName(_ m: pin_progress_mode_t) -> String {
        switch m {
        case PIN_PROGRESS_NONE: "none"
        case PIN_PROGRESS_INDETERMINATE: "indeterminate"
        case PIN_PROGRESS_NORMAL: "normal"
        case PIN_PROGRESS_PAUSED: "paused"
        case PIN_PROGRESS_ERROR: "error"
        default: "?"
        }
    }
}
