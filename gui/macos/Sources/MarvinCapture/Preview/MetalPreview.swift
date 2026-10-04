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
import QuartzCore
import SwiftUI

/// The NSView whose layer is the CAMetalLayer. It only reports geometry, window changes and display
/// link ticks; all drawing is the renderer's.
///
/// The display link is the view's own (`NSView.displayLink`, macOS 14+): it follows the screen the
/// window is on. It lives on the main run loop but stays paused unless the render thread has frames
/// queued, so an idle preview wakes nothing.
@MainActor
final class PreviewLayerView: NSView {
    var renderer: PreviewRenderer? { didSet { updateLayout(); updateLink() } }
    var onWindowChange: ((NSWindow?) -> Void)?
    private var link: CADisplayLink?
    private var linkWanted = false

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layerContentsRedrawPolicy = .never
        setAccessibilityRole(.image)
        setAccessibilityLabel("Video preview")
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    override func makeBackingLayer() -> CALayer {
        let l = CAMetalLayer()
        l.backgroundColor = CGColor(gray: 0, alpha: 1)
        return l
    }

    var metalLayer: CAMetalLayer { layer as! CAMetalLayer }

    override var isOpaque: Bool { true }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        link?.invalidate()
        link = nil
        if window != nil {
            let l = displayLink(target: self, selector: #selector(tick(_:)))
            l.isPaused = true
            l.add(to: .main, forMode: .common)
            link = l
        }
        renderer?.setLinkAvailable(link != nil)
        updateLayout()
        updateLink()
        onWindowChange?(window)
    }

    override func viewDidChangeBackingProperties() {
        super.viewDidChangeBackingProperties()
        updateLayout()
    }

    override func setFrameSize(_ newSize: NSSize) {
        super.setFrameSize(newSize)
        updateLayout()
    }

    override func layout() {
        super.layout()
        updateLayout()
    }

    /// drawableSize in pixels, contentsScale = the window's backing scale.
    private func updateLayout() {
        guard let renderer else { return }
        let scale = window?.backingScaleFactor ?? NSScreen.main?.backingScaleFactor ?? 2
        let px = convertToBacking(bounds).size
        renderer.resize(pixelWidth: Int(px.width.rounded()), pixelHeight: Int(px.height.rounded()), scale: scale)
    }

    /// Called from the render thread's request (hopped to main).
    func setLinkWanted(_ wanted: Bool) {
        linkWanted = wanted
        updateLink()
    }

    private func updateLink() { link?.isPaused = !linkWanted }

    @objc private func tick(_ l: CADisplayLink) {
        renderer?.displayLinkTick(target: l.targetTimestamp)
    }
}

/// The PreviewSink: owns the view and the renderer, tells the renderer when the picture can't be seen.
/// If Metal is unavailable the sink does nothing (black view); `unavailableReason` is for the banner.
@MainActor
final class MetalPreview: PreviewSink {
    let view = PreviewLayerView(frame: NSRect(x: 0, y: 0, width: 64, height: 48))
    private(set) var unavailableReason: String?
    private var renderer: PreviewRenderer?
    private var session: Pin.Session?
    private var visible = false
    private var observers: [NSObjectProtocol] = []

    init() {
        var failure = ""
        if let r = PreviewRenderer(layer: view.metalLayer, failure: &failure) {
            renderer = r
            view.renderer = r
            let v = view
            r.setLinkActive = { wanted in DispatchQueue.main.async { MainActor.assumeIsolated { v.setLinkWanted(wanted) } } }
        } else {
            unavailableReason = failure
        }
        view.onWindowChange = { [weak self] w in self?.windowChanged(w) }
        let nc = NotificationCenter.default
        for name in [NSApplication.didHideNotification, NSApplication.didUnhideNotification] {
            observers.append(nc.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.updateVisibility() }
            })
        }
    }

    // MARK: PreviewSink

    func attach(session s: Pin.Session) {
        session = s
        updateVisibility()   // (sets `visible` first so a hidden window attaches paused)
        renderer?.attach(s, enabled: visible)
    }

    func detach() {
        session = nil
        renderer?.detach()
    }

    var frameDar: (num: Int, den: Int)? { renderer?.currentDar }
    var hasRecentFrame: Bool { renderer?.hasRecentFrame ?? false }

    // MARK: visibility

    private var windowObserver: NSObjectProtocol?
    private static let ignoreOcclusion = ProcessInfo.processInfo.environment["MARVIN_PREVIEW_IGNORE_OCCLUSION"] == "1"

    private func windowChanged(_ w: NSWindow?) {
        if let o = windowObserver { NotificationCenter.default.removeObserver(o); windowObserver = nil }
        if let w {
            windowObserver = NotificationCenter.default.addObserver(
                forName: NSWindow.didChangeOcclusionStateNotification, object: w, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.updateVisibility() }
            }
        }
        updateVisibility()
    }

    /// Visible = in a window that is at least partly on screen, and the app is not hidden. Minimised
    /// windows and windows fully covered by others report no `.visible` occlusion state.
    private func updateVisibility() {
        let w = view.window
        // MARVIN_PREVIEW_IGNORE_OCCLUSION=1 (developer aid): a locked / sleeping screen reports every
        // window as occluded, which would keep the preview off while testing remotely.
        let v = w != nil && (Self.ignoreOcclusion || (w!.occlusionState.contains(.visible) && !w!.isMiniaturized && !NSApp.isHidden))
        guard v != visible else { return }
        visible = v
        if session != nil { renderer?.setVisible(v) }
    }
}

/// Hosts the model's preview view in SwiftUI. It fills whatever frame PreviewArea gives it, which
/// already has the picture's aspect (pin_fit_rect), so nothing is letterboxed here.
struct PreviewLayerRepresentable: NSViewRepresentable {
    let preview: MetalPreview

    func makeNSView(context: Context) -> PreviewLayerView { preview.view }
    func updateNSView(_ nsView: PreviewLayerView, context: Context) {}
}
