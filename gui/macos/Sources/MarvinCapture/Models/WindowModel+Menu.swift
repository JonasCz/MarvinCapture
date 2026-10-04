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

extension WindowModel {
    /// "Choose Output Folder…" for the current input's folder (analog or DV/HDV), as a sheet on the
    /// window. Shared by the folder buttons and the File menu; the folder can't change mid-capture.
    func chooseOutputFolder() {
        guard isIdle else { return }
        if isDvInput {
            chooseFolder(current: dvOutputDir) { [self] in dvOutputDir = $0 }
        } else {
            chooseFolder(current: analogOutputDir) { [self] in analogOutputDir = $0 }
        }
    }
}
