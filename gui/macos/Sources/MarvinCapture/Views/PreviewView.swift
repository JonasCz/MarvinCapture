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

/// The video preview: the Metal layer view (see Preview/) filling the frame PreviewArea gives it, which
/// already has the picture's aspect, so nothing is letterboxed here. The model's `preview` sink owns
/// the view and attaches to the session.
struct PreviewView: View {
    let model: WindowModel

    var body: some View {
        if let metal = model.preview as? MetalPreview {
            PreviewLayerRepresentable(preview: metal)
                .accessibilityLabel("Video preview")
        } else {
            Color.black.accessibilityLabel("Video preview")
        }
    }
}
