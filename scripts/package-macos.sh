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
# scripts/package-macos.sh
#
# Builds an unsigned macOS installer, build/macos-arm64/installer/
# MarvinCapture-<version>-macos-arm64.pkg, from build/macos-arm64/dist
# (version from the top-level VERSION file). Apple Silicon, macOS 15+.
#
# The .pkg has two choices, both on by default:
#
#   MarvinCapture.app      -> /Applications/MarvinCapture.app
#   command-line tool      -> /usr/local/lib/marvincapture/{MarvinCaptureCLI,
#                             libmarvin-core.dylib, libusb-1.0.0.dylib,
#                             firmware/, uninstall.sh}
#                             and a symlink /usr/local/bin/MarvinCaptureCLI
#
# The package is neither signed nor notarised, so Gatekeeper blocks it when it
# was downloaded (see README). Static inputs are in packaging/macos/. Uses
# pkgbuild and productbuild (Command Line Tools). Staging is in
# build/macos-arm64/installer/stage.
#
# Usage:
#   scripts/package-macos.sh [--skip-build] [--config Release|Debug]
#
#   --skip-build            use the existing build/macos-arm64/dist instead of
#                           running scripts/build.sh first
#   --config Release|Debug  passed to scripts/build.sh (default Release)

set -euo pipefail

usage() { sed -n '/^# Usage:/,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

SKIP_BUILD=0
CONFIG="Release"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-build) SKIP_BUILD=1; shift ;;
        --config)
            [[ $# -ge 2 ]] || { echo "package-macos.sh: --config needs Release or Debug" >&2; exit 2; }
            CONFIG="$2"; shift 2 ;;
        --config=*) CONFIG="${1#--config=}"; shift ;;
        -h|--help)  usage; exit 0 ;;
        *) echo "package-macos.sh: unknown option '$1'" >&2; usage >&2; exit 2 ;;
    esac
done

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

step() { printf '\n\033[36m== %s\033[0m\n' "$*"; }
die()  { echo "package-macos.sh: $*" >&2; exit 1; }

[[ "$(uname -s)" == Darwin ]] || die "macOS only"
[[ "$(uname -m)" == arm64 ]] || die "macOS builds are arm64 (Apple Silicon) only for now"
for tool in pkgbuild productbuild codesign ditto; do
    command -v "${tool}" >/dev/null 2>&1 || die "'${tool}' not found (needs the Command Line Tools)"
done

PLATFORM="macos-arm64"
BUILD="${ROOT}/build/${PLATFORM}"
DIST="${BUILD}/dist"
OUT="${BUILD}/installer"
STAGE="${OUT}/stage"
RES="${ROOT}/packaging/macos"

[[ -f "${ROOT}/VERSION" ]] || die "missing ${ROOT}/VERSION"
VERSION="$(tr -d '[:space:]' < "${ROOT}/VERSION")"
[[ "${VERSION}" =~ ^[0-9]+(\.[0-9]+)*$ ]] || die "VERSION '${VERSION}' is not a dotted number"

# --- 1. Build -----------------------------------------------------------------------
if [[ "${SKIP_BUILD}" -eq 0 ]]; then
    step "Building (scripts/build.sh --config ${CONFIG})"
    "${ROOT}/scripts/build.sh" --config "${CONFIG}"
fi
for f in MarvinCapture.app MarvinCaptureCLI libmarvin-core.dylib libusb-1.0.0.dylib firmware; do
    [[ -e "${DIST}/${f}" ]] || die "${DIST}/${f} is missing; run scripts/build.sh (or leave out --skip-build)"
done
codesign --verify --deep --strict "${DIST}/MarvinCapture.app" || die "MarvinCapture.app has no valid signature"

# --- 2. Stage the two payloads ---------------------------------------------------------
step "Staging"
rm -rf "${OUT}"
mkdir -p "${STAGE}/app" "${STAGE}/cli/bin" "${STAGE}/cli/lib/marvincapture" "${OUT}/pkgs" "${OUT}/resources"

# ditto: keeps the bundle's symlinks and signature intact; no xattrs/resource forks.
ditto --norsrc --noextattr "${DIST}/MarvinCapture.app" "${STAGE}/app/MarvinCapture.app"
# The uninstaller also goes into the app, so an app-only install has it too; adding
# a resource changes the bundle's seal, so re-sign the bundle (the dylibs keep theirs).
install -m 755 "${RES}/uninstall.sh" "${STAGE}/app/MarvinCapture.app/Contents/Resources/uninstall.sh"
codesign --force --sign - "${STAGE}/app/MarvinCapture.app"
codesign --verify --deep --strict "${STAGE}/app/MarvinCapture.app"

LIBDIR="${STAGE}/cli/lib/marvincapture"
for f in MarvinCaptureCLI libmarvin-core.dylib libusb-1.0.0.dylib; do
    ditto --norsrc --noextattr "${DIST}/${f}" "${LIBDIR}/${f}"
done
ditto --norsrc --noextattr "${DIST}/firmware" "${LIBDIR}/firmware"
install -m 755 "${RES}/uninstall.sh" "${LIBDIR}/uninstall.sh"
# arm64 needs a signature on every binary; ad-hoc is enough (build.sh's output already has one).
for f in MarvinCaptureCLI libmarvin-core.dylib libusb-1.0.0.dylib; do
    codesign --force --sign - "${LIBDIR}/${f}" 2>/dev/null
done
# relative symlink: the CLI resolves @loader_path and firmware/ through the real path
ln -s ../lib/marvincapture/MarvinCaptureCLI "${STAGE}/cli/bin/MarvinCaptureCLI"
find "${STAGE}" -name .DS_Store -delete
chmod -R u=rwX,go=rX "${STAGE}"
# com.apple.provenance etc. would end up in the payload as ._ AppleDouble files
xattr -cr "${STAGE}" 2>/dev/null || true
export COPYFILE_DISABLE=1

# --- 3. Component packages --------------------------------------------------------------
step "pkgbuild"
pkgbuild --root "${STAGE}/app" --component-plist "${RES}/app-component.plist" \
    --identifier jonascz.MarvinCapture.app --version "${VERSION}" \
    --install-location /Applications --ownership recommended \
    "${OUT}/pkgs/MarvinCapture-app.pkg"
pkgbuild --root "${STAGE}/cli" \
    --identifier jonascz.MarvinCapture.cli --version "${VERSION}" \
    --install-location /usr/local --ownership recommended \
    "${OUT}/pkgs/MarvinCapture-cli.pkg"

# --- 4. Product archive ------------------------------------------------------------------
step "productbuild"
cp "${RES}/resources/"*.html "${OUT}/resources/"
cp "${ROOT}/LICENSE" "${OUT}/resources/license.txt"
sed "s/@VERSION@/${VERSION}/g" "${RES}/distribution.xml" > "${OUT}/distribution.xml"
PKG="${OUT}/MarvinCapture-${VERSION}-${PLATFORM}.pkg"
productbuild --distribution "${OUT}/distribution.xml" --resources "${OUT}/resources" \
    --package-path "${OUT}/pkgs" "${PKG}"

printf '\n\033[32mDone: %s\033[0m\n' "${PKG}"
echo "Unsigned: when downloaded, macOS blocks it until allowed in System Settings > Privacy & Security."
