#!/usr/bin/env bash
# Pinnacle Studio 500-USB open driver
# Copyright (C) 2026 Jonas Cz.
#
# This program is free software: you can redistribute it and/or modify it
# under the terms of the GNU Affero General Public License as published by
# the Free Software Foundation, either version 3 of the License, or (at your
# option) any later version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License
# for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.

# scripts/build.sh
#
# Builds everything and assembles a runnable output in build/<os>-<arch>/dist,
# on macOS (Apple Silicon only, for now) and Linux. The Windows counterpart is
# scripts/build.ps1; both use the same layout.
#
#   1. (first time only) builds the minimal static FFmpeg into third_party/.
#   2. Configures and builds the native core (libmarvin-core.dylib/.so,
#      MarvinCaptureCLI) into build/<os>-<arch>/core, and runs the ctest suite.
#   3. Assembles build/<os>-<arch>/dist:
#
#        MarvinCaptureCLI           the command-line program
#        libmarvin-core.dylib       the core library (.so on Linux)
#        libusb-1.0.0.dylib         macOS only: bundled libusb (Linux uses the
#                                   system's libusb-1.0)
#        firmware/                  FPGA bitstreams
#        MarvinCapture.app          macOS only: the native GUI (gui/macos), ad-hoc
#                                   signed; core, libusb and firmware are inside
#                                   (Contents/Frameworks, Contents/Resources/firmware)
#
#   4. macOS only: builds the Swift GUI with SwiftPM (intermediates in
#      build/macos-arm64/gui) and assembles dist/MarvinCapture.app. Needs only the
#      Command Line Tools (no Xcode): the app icon is the committed
#      gui/macos/Resources/AppIcon.iconset packed with iconutil. There is no Linux GUI yet.
#
#   MarvinCaptureCLI finds the core next to itself (rpath @loader_path /
#   $ORIGIN) and the core finds firmware/ next to itself, so dist/ can be
#   copied anywhere. On macOS the core loads the bundled libusb next to
#   itself, so dist/ doesn't need Homebrew; see docs/building.md.
#
# Requires cmake, ninja, pkg-config, libusb-1.0, make and a C compiler (and
# nasm on x86_64); for the macOS GUI also swift (Command Line Tools, Swift 6.x)
# and iconutil. See docs/building.md.
#
# Usage:
#   scripts/build.sh [--config Release|Debug] [--skip-tests] [--skip-gui] [--clean]
#
#   --config Release|Debug  build type (default Release)
#   --skip-tests            don't run ctest
#   --skip-gui              build the native core and CLI only (macOS: no
#                           MarvinCapture.app; Linux has no GUI yet, so no effect)
#   --clean                 delete build/<os>-<arch>/ first (other platforms'
#                           output is kept)

set -euo pipefail

usage() { sed -n '/^# Usage:/,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

CONFIG="Release"
SKIP_TESTS=0
SKIP_GUI=0
CLEAN=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --config)
            [[ $# -ge 2 ]] || { echo "build.sh: --config needs Release or Debug" >&2; exit 2; }
            CONFIG="$2"; shift 2 ;;
        --config=*) CONFIG="${1#--config=}"; shift ;;
        --skip-tests) SKIP_TESTS=1; shift ;;
        --skip-gui)   SKIP_GUI=1; shift ;;
        --clean)      CLEAN=1; shift ;;
        -h|--help)    usage; exit 0 ;;
        *) echo "build.sh: unknown option '$1'" >&2; usage >&2; exit 2 ;;
    esac
done
case "${CONFIG}" in
    Release|Debug) ;;
    *) echo "build.sh: --config must be Release or Debug, not '${CONFIG}'" >&2; exit 2 ;;
esac

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

step() { printf '\n\033[36m== %s\033[0m\n' "$*"; }
die()  { echo "build.sh: $*" >&2; exit 1; }

# --- Platform: os/arch named like scripts/build-ffmpeg.sh -----------------------
case "$(uname -s)" in
    Darwin) OS="macos"; LIB_EXT="dylib" ;;
    Linux)  OS="linux"; LIB_EXT="so" ;;
    *) die "unsupported OS '$(uname -s)' (Windows: use scripts\\build.ps1)" ;;
esac
ARCH="$(uname -m)"
case "${ARCH}" in
    x86_64|amd64)  ARCH="x86_64" ;;
    aarch64|arm64) ARCH="arm64" ;;
esac
if [[ "${OS}" == macos && "${ARCH}" != arm64 ]]; then
    # Only Apple Silicon is built and tested on macOS for now.
    die "macOS builds are arm64 (Apple Silicon) only for now; this Mac is ${ARCH}"
fi

PLATFORM="${OS}-${ARCH}"
BUILD="${ROOT}/build/${PLATFORM}"
CORE="${BUILD}/core"
DIST="${BUILD}/dist"
CORE_LIB="libmarvin-core.${LIB_EXT}"
# copied next to the core by CMakeLists.txt (macOS only)
LIBUSB_LIB=""
[[ "${OS}" == macos ]] && LIBUSB_LIB="libusb-1.0.0.dylib"

for tool in cmake ninja pkg-config make cc; do
    command -v "${tool}" >/dev/null 2>&1 || die "'${tool}' not found (see docs/building.md for the prerequisites)"
done
pkg-config --exists libusb-1.0 || die "libusb-1.0 not found by pkg-config (see docs/building.md)"

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

if [[ "${CLEAN}" -eq 1 && -d "${BUILD}" ]]; then
    step "Cleaning build/${PLATFORM}/"
    rm -rf "${BUILD}"
fi

# --- 1. FFmpeg -------------------------------------------------------------------
FFMPEG="${ROOT}/third_party/ffmpeg-${PLATFORM}/lib/pkgconfig"
# rebuilt when scripts/build-ffmpeg.sh changed since the installed build (a component was added)
FF_STAMP="${ROOT}/third_party/ffmpeg-${PLATFORM}/build-script.sha256"
FF_HASH="$(sha256_of "${ROOT}/scripts/build-ffmpeg.sh")"
if [[ ! -d "${FFMPEG}" || ! -f "${FF_STAMP}" || "$(tr -d '[:space:]' < "${FF_STAMP}")" != "${FF_HASH}" ]]; then
    step "Building the minimal static FFmpeg (first time, or build-ffmpeg.sh changed)"
    bash "${ROOT}/scripts/build-ffmpeg.sh"
fi

# --- 2. Native core, CLI, tests ----------------------------------------------------
step "Configuring the native core (${CONFIG})"
cmake -G Ninja -S "${ROOT}" -B "${CORE}" "-DCMAKE_BUILD_TYPE=${CONFIG}"
step "Building the native core"
cmake --build "${CORE}"

if [[ "${SKIP_TESTS}" -eq 0 ]]; then
    step "Running tests"
    ctest --test-dir "${CORE}" --output-on-failure
fi

# --- 3. Assemble build/<os>-<arch>/dist ----------------------------------------------
step "Assembling build/${PLATFORM}/dist"
mkdir -p "${DIST}"
cp -f "${CORE}/MarvinCaptureCLI" "${DIST}/"
"${ROOT}/scripts/sync-cli-help.sh" "${DIST}/MarvinCaptureCLI"   # help text -> docs/cli.md
cp -f "${CORE}/${CORE_LIB}" "${DIST}/"
[[ -n "${LIBUSB_LIB}" ]] && cp -f "${CORE}/${LIBUSB_LIB}" "${DIST}/"
rm -rf "${DIST}/firmware"
mkdir -p "${DIST}/firmware"
cp -f "${ROOT}/firmware/fpga-ohci.bin" "${ROOT}/firmware/fpga-capture.bin" \
      "${ROOT}/firmware/fx2-marvin.bin" "${ROOT}/firmware/README.md" "${DIST}/firmware/"

# --- 4. GUI --------------------------------------------------------------------------
# macOS: gui/macos (SwiftPM) -> dist/MarvinCapture.app. Linux: none yet.
APP="${DIST}/MarvinCapture.app"
if [[ "${SKIP_GUI}" -eq 1 ]]; then
    echo "(--skip-gui)"
elif [[ "${OS}" != macos ]]; then
    echo "(no ${OS} GUI yet: core and CLI only)"
else
    command -v swift >/dev/null 2>&1 || die "'swift' not found (install the Xcode Command Line Tools: xcode-select --install)"
    GUI_SRC="${ROOT}/gui/macos"
    GUI_BUILD="${BUILD}/gui"
    SWIFT_CONFIG="$(echo "${CONFIG}" | tr '[:upper:]' '[:lower:]')"

    step "Building the macOS GUI (${CONFIG})"
    # -L: where libmarvin-core.dylib is (the module map only says `link "marvin-core"`).
    swift build -c "${SWIFT_CONFIG}" --package-path "${GUI_SRC}" --scratch-path "${GUI_BUILD}" \
        -Xlinker -L"${CORE}"
    GUI_BIN="$(swift build -c "${SWIFT_CONFIG}" --package-path "${GUI_SRC}" --scratch-path "${GUI_BUILD}" \
        --show-bin-path)/MarvinCapture"

    # App icon: the committed iconset (the logo, rendered by scripts/make-icons.py) packed with iconutil.
    ICON_ICNS="${GUI_BUILD}/AppIcon.icns"
    iconutil -c icns "${GUI_SRC}/Resources/AppIcon.iconset" -o "${ICON_ICNS}"

    step "Assembling build/${PLATFORM}/dist/MarvinCapture.app"
    APP_VERSION="0.1.0"
    APP_BUILD="$(git -C "${ROOT}" rev-list --count HEAD 2>/dev/null || echo 1)"
    rm -rf "${APP}"
    mkdir -p "${APP}/Contents/MacOS" "${APP}/Contents/Frameworks" "${APP}/Contents/Resources"
    cp -f "${GUI_BIN}" "${APP}/Contents/MacOS/MarvinCapture"
    cp -f "${CORE}/${CORE_LIB}" "${CORE}/${LIBUSB_LIB}" "${APP}/Contents/Frameworks/"
    cp -R "${DIST}/firmware" "${APP}/Contents/Resources/firmware"   # not in Frameworks: non-code files there break codesign
    cp -f "${ICON_ICNS}" "${APP}/Contents/Resources/AppIcon.icns"

    # Info.plist (generated so the version is set here).
    # UIDesignRequiresCompatibility: keeps the classic (pre-Liquid Glass) look if the
    # app is ever built with the macOS 26 SDK; harmless on macOS 15.
    cat > "${APP}/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key><string>en</string>
    <key>CFBundleExecutable</key><string>MarvinCapture</string>
    <key>CFBundleIdentifier</key><string>jonascz.MarvinCapture</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>CFBundleName</key><string>MarvinCapture</string>
    <key>CFBundleDisplayName</key><string>MarvinCapture</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>${APP_VERSION}</string>
    <key>CFBundleVersion</key><string>${APP_BUILD}</string>
    <key>CFBundleIconFile</key><string>AppIcon</string>
    <key>LSMinimumSystemVersion</key><string>15.0</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.video</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>NSPrincipalClass</key><string>NSApplication</string>
    <key>UIDesignRequiresCompatibility</key><true/>
    <key>NSHumanReadableCopyright</key><string>Copyright © 2026 Jonas Cz. Free software under the GNU Affero General Public License, version 3 or later.</string>
</dict>
</plist>
PLIST
    plutil -lint "${APP}/Contents/Info.plist" >/dev/null

    # Ad-hoc signing: inner code first, then the bundle (no --deep).
    for lib in "${APP}/Contents/Frameworks/"*.dylib; do
        codesign --force --sign - "${lib}"
    done
    codesign --force --sign - "${APP}"
    codesign --verify --strict --verbose=2 "${APP}"
fi

# --- Check ---------------------------------------------------------------------------
step "Checking build/${PLATFORM}/dist"
expect=(MarvinCaptureCLI "${CORE_LIB}" ${LIBUSB_LIB}
        firmware/fpga-ohci.bin firmware/fpga-capture.bin firmware/fx2-marvin.bin firmware/README.md)
[[ "${OS}" == macos && "${SKIP_GUI}" -eq 0 ]] && expect+=(MarvinCapture.app/Contents/MacOS/MarvinCapture
        MarvinCapture.app/Contents/Info.plist MarvinCapture.app/Contents/Resources/AppIcon.icns
        MarvinCapture.app/Contents/Frameworks/"${CORE_LIB}" MarvinCapture.app/Contents/Frameworks/"${LIBUSB_LIB}"
        MarvinCapture.app/Contents/Resources/firmware/fpga-ohci.bin)
missing=()
for f in "${expect[@]}"; do
    [[ -e "${DIST}/${f}" ]] || missing+=("${f}")
done
if [[ ${#missing[@]} -gt 0 ]]; then
    die "missing from build/${PLATFORM}/dist: ${missing[*]}"
fi

printf '\n\033[32mDone: %s\033[0m\n' "${DIST}"
