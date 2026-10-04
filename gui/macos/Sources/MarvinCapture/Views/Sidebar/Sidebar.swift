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

/// The left column, one pane on the sidebar material: info banner, source section, then the analog or the
/// DV/HDV sections. Scrolls when the window is short.
struct Sidebar: View {
    let app: AppModel
    private var model: WindowModel { app.window }

    var body: some View {
        ScrollView {
            VStack(spacing: 14) {
                InfoBanner(model: model)
                SourceCard(app: app)
                Divider()
                if model.isDvInput { DvPanel(model: model) } else { AnalogPanel(model: model) }
            }
            .padding(MainView.padding)
        }
        // The system sidebar material, running up under the (transparent) title bar like Finder's.
        .background { SidebarMaterial().ignoresSafeArea() }
        .overlay(alignment: .trailing) { Divider().ignoresSafeArea() }
        .accessibilityLabel("Capture settings")
    }
}

/// Where the folder button starts: the window the sidebar is in.
@MainActor
func chooseFolder(current: String, apply: @escaping (String) -> Void) {
    FolderPicker.choose(startingAt: current, in: Alerts.shared.window ?? NSApp.keyWindow, completion: apply)
}
