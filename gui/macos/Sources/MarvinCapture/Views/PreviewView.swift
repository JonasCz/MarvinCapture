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

/// The seam for the video renderer: the next step replaces this view's body with the Metal layer
/// (an NSViewRepresentable) and attaches it through `WindowModel.preview`. It fills whatever
/// frame PreviewArea gives it, which already has the picture's aspect, so nothing is letterboxed here.
struct PreviewView: View {
    let model: WindowModel

    var body: some View {
        Color.black
            .accessibilityLabel("Video preview")
    }
}
