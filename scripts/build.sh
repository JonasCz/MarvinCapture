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
#   3. (no macOS/Linux GUI yet; build/<os>-<arch>/gui is reserved for it.)
#   4. Assembles build/<os>-<arch>/dist:
#
#        MarvinCaptureCLI           the command-line program
#        libmarvin-core.dylib       the core library (.so on Linux)
#        libusb-1.0.0.dylib         macOS only: bundled libusb (Linux uses the
#                                   system's libusb-1.0)
#        firmware/                  FPGA bitstreams
#
#   MarvinCaptureCLI finds the core next to itself (rpath @loader_path /
#   $ORIGIN) and the core finds firmware/ next to itself, so dist/ can be
#   copied anywhere. On macOS the core loads the bundled libusb next to
#   itself, so dist/ doesn't need Homebrew; see docs/building.md.
#
# Requires cmake, ninja, pkg-config, libusb-1.0, make and a C compiler (and
# nasm on x86_64). See docs/building.md.
#
# Usage:
#   scripts/build.sh [--config Release|Debug] [--skip-tests] [--skip-gui] [--clean]
#
#   --config Release|Debug  build type (default Release)
#   --skip-tests            don't run ctest
#   --skip-gui              build the native core and CLI only (currently
#                           always the case: there is no macOS/Linux GUI yet)
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
cp -f "${CORE}/${CORE_LIB}" "${DIST}/"
[[ -n "${LIBUSB_LIB}" ]] && cp -f "${CORE}/${LIBUSB_LIB}" "${DIST}/"
rm -rf "${DIST}/firmware"
mkdir -p "${DIST}/firmware"
cp -f "${ROOT}/firmware/fpga-ohci.bin" "${ROOT}/firmware/fpga-capture.bin" \
      "${ROOT}/firmware/fx2-marvin.bin" "${ROOT}/firmware/README.md" "${DIST}/firmware/"

# --- 4. GUI --------------------------------------------------------------------------
# There is no macOS/Linux GUI yet; when there is, it builds here with its
# intermediates in build/<os>-<arch>/gui and its output in dist/.
if [[ "${SKIP_GUI}" -eq 1 ]]; then
    echo "(--skip-gui: nothing to skip, there is no ${OS} GUI yet)"
else
    echo "(no ${OS} GUI yet: core and CLI only)"
fi

# --- Check ---------------------------------------------------------------------------
step "Checking build/${PLATFORM}/dist"
expect=(MarvinCaptureCLI "${CORE_LIB}" ${LIBUSB_LIB}
        firmware/fpga-ohci.bin firmware/fpga-capture.bin firmware/fx2-marvin.bin firmware/README.md)
missing=()
for f in "${expect[@]}"; do
    [[ -e "${DIST}/${f}" ]] || missing+=("${f}")
done
if [[ ${#missing[@]} -gt 0 ]]; then
    die "missing from build/${PLATFORM}/dist: ${missing[*]}"
fi

printf '\n\033[32mDone: %s\033[0m\n' "${DIST}"
