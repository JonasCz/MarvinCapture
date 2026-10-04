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
import CMarvinCore

/// Placeholder: proves the core loads and the C interop (incl. fixed char
/// arrays) works. Replaced by the real UI in the next step.
struct ContentView: View {
    @State private var devices = Pin.enumerateDevices()

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("MarvinCapture").font(.largeTitle.bold())
            Text(Pin.versionString)
            Text("API version \(Pin.apiVersion)").foregroundStyle(.secondary)
            Divider()
            HStack {
                Text("Devices (\(devices.count))").font(.headline)
                Spacer()
                Button("Refresh") { devices = Pin.enumerateDevices() }
            }
            if devices.isEmpty {
                Text(String(cString: pin_no_devices_hint())).foregroundStyle(.secondary)
            } else {
                ForEach(Array(devices.enumerated()), id: \.offset) { _, d in
                    VStack(alignment: .leading, spacing: 2) {
                        Text(d.nameString).bold()
                        Text("id \(d.idString)  serial \(d.serialString.isEmpty ? "-" : d.serialString)  state \(d.state.rawValue)  hub depth \(d.hub_depth)")
                            .font(.callout.monospaced()).foregroundStyle(.secondary)
                    }
                }
            }
            Spacer()
        }
        .padding(24)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    }
}
