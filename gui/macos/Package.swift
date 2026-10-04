// swift-tools-version: 6.0
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

import PackageDescription

// MarvinCapture for macOS: a SwiftUI/AppKit front end over marvin-core
// (src/api/pin_api.h). Built by scripts/build.sh, which passes the core's
// library directory:  swift build -Xlinker -L<build>/core
// (`swift build` alone only links when given that -L).
let package = Package(
    name: "MarvinCapture",
    platforms: [.macOS(.v15)],
    targets: [
        // pin_api.h as Swift module CMarvinCore (module map, no header copy).
        .systemLibrary(name: "CMarvinCore", path: "Sources/CMarvinCore"),
        .executableTarget(
            name: "MarvinCapture",
            dependencies: ["CMarvinCore"],
            path: "Sources/MarvinCapture",
            linkerSettings: [
                // The core ships in the bundle's Contents/Frameworks.
                .unsafeFlags(["-Xlinker", "-rpath", "-Xlinker", "@executable_path/../Frameworks"]),
            ]
        ),
    ],
    swiftLanguageModes: [.v5]
)
