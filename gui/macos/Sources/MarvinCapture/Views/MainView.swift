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

import SwiftUI

/// The window's content: sidebar (fixed width) + preview, status bar below. Port of MainWindow.xaml.
/// The title bar is transparent with the content running under it (MainWindowController keeps its title
/// in step); the safe area keeps controls clear of the traffic lights.
struct MainView: View {
    @Bindable var app: AppModel

    private var model: WindowModel { app.window }
    static let sidebarWidth: CGFloat = 320
    /// One padding everywhere: sidebar content and preview to the window / title bar / status bar edges
    /// and to each other.
    static let padding: CGFloat = 16

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 0) {
                Sidebar(app: app)
                    .frame(width: Self.sidebarWidth)
                PreviewArea(model: model)
            }
            Divider()
            StatusBar(model: model)
        }
        // The window background drawn explicitly: identical on screen, and it makes the snapshot hook's
        // cacheDisplay (which paints opaque white behind transparent views) correct in dark mode.
        .background { Color(nsColor: .windowBackgroundColor).ignoresSafeArea() }
        .overlay {
            if model.isFinalizingForClose { FinalizingOverlay() }
        }
        .sheet(isPresented: $app.showHelp) {
            HelpSheet(error: app.helpError, text: app.helpText)
        }
    }
}

/// Covers the window while a capture is being finished for a close / quit.
struct FinalizingOverlay: View {
    var body: some View {
        ZStack {
            Color.black.opacity(0.3)
            VStack(spacing: 14) {
                ProgressView().controlSize(.large)
                Text("Finalizing files…").font(.title3.weight(.semibold))
                Text("The window closes when the capture is safely written.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            .padding(.horizontal, 32).padding(.vertical, 24)
            .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12))
            .accessibilityElement(children: .combine)
        }
        .contentShape(Rectangle())   // swallows clicks
    }
}
