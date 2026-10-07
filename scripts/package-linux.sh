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

# scripts/package-linux.sh
#
# Builds a Debian package of the command-line program (there is no Linux GUI):
#
#   build/linux-<arch>/installer/marvincapture_<version>_<debarch>.deb
#
# Runs scripts/build.sh --skip-gui first (unless --skip-build), then stages:
#
#   /usr/lib/marvincapture/        MarvinCaptureCLI, libmarvin-core.so, firmware/
#   /usr/bin/MarvinCaptureCLI      symlink to the above (rpath $ORIGIN and the
#                                  core's firmware lookup both use real paths)
#   /usr/lib/udev/rules.d/         packaging/linux/*.rules (device access for the
#                                  five Marvin PIDs; /dev/cpu_dma_latency access)
#   /usr/share/doc/marvincapture/copyright
#
# and runs dpkg-deb --build --root-owner-group. Needs dpkg-deb, plus objdump and
# strip (binutils). The version comes from the top-level VERSION file. Depends is
# libusb plus the glibc minimum computed from the built binaries; FFmpeg is
# linked statically. See packaging/linux/README.md.
#
# Usage:
#   scripts/package-linux.sh [--skip-build] [--skip-tests]
#
#   --skip-build    package the existing build/linux-<arch>/dist as it is
#   --skip-tests    passed to scripts/build.sh: don't run ctest

set -euo pipefail

usage() { sed -n '/^# Usage:/,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

SKIP_BUILD=0
BUILD_ARGS=(--skip-gui)
while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-build) SKIP_BUILD=1; shift ;;
        --skip-tests) BUILD_ARGS+=(--skip-tests); shift ;;
        -h|--help)    usage; exit 0 ;;
        *) echo "package-linux.sh: unknown option '$1'" >&2; usage >&2; exit 2 ;;
    esac
done

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

step() { printf '\n\033[36m== %s\033[0m\n' "$*"; }
die()  { echo "package-linux.sh: $*" >&2; exit 1; }

[[ "$(uname -s)" == Linux ]] || die "this builds the Linux package; run it on Debian/Ubuntu"
for tool in dpkg-deb dpkg objdump strip; do
    command -v "${tool}" >/dev/null 2>&1 || die "'${tool}' not found (apt install dpkg binutils)"
done

# Same directory naming as scripts/build.sh.
ARCH="$(uname -m)"
case "${ARCH}" in
    x86_64|amd64)  ARCH="x86_64" ;;
    aarch64|arm64) ARCH="arm64" ;;
esac
DEB_ARCH="$(dpkg --print-architecture)"
BUILD="${ROOT}/build/linux-${ARCH}"
DIST="${BUILD}/dist"
OUT="${BUILD}/installer"
VERSION="$(sed -n 's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "${ROOT}/VERSION" | head -n 1)"
[[ "${VERSION}" =~ ^[0-9][0-9A-Za-z.+~-]*$ ]] || die "VERSION '${VERSION}' is not a valid Debian version"
PKG="marvincapture"
DEB="${OUT}/${PKG}_${VERSION}_${DEB_ARCH}.deb"
RULES=(60-marvincapture.rules 60-marvincapture-cpu-dma-latency.rules)

# --- 1. Build ---------------------------------------------------------------------
if [[ "${SKIP_BUILD}" -eq 0 ]]; then
    step "Building (scripts/build.sh ${BUILD_ARGS[*]})"
    "${ROOT}/scripts/build.sh" "${BUILD_ARGS[@]}"
fi
for f in MarvinCaptureCLI libmarvin-core.so firmware/fpga-ohci.bin firmware/fpga-capture.bin \
         firmware/fx2-marvin.bin firmware/README.md; do
    [[ -e "${DIST}/${f}" ]] || die "build/linux-${ARCH}/dist/${f} is missing (run scripts/build.sh --skip-gui)"
done

# --- 2. Stage the package tree ----------------------------------------------------
step "Staging ${PKG} ${VERSION} (${DEB_ARCH})"
STAGE="$(mktemp -d)"
trap 'rm -rf "${STAGE}"' EXIT
chmod 755 "${STAGE}"
LIBDIR="${STAGE}/usr/lib/${PKG}"
DOCDIR="${STAGE}/usr/share/doc/${PKG}"
install -d -m 755 "${LIBDIR}/firmware" "${STAGE}/usr/bin" "${STAGE}/usr/lib/udev/rules.d" \
                  "${DOCDIR}" "${STAGE}/DEBIAN"

install -m 755 "${DIST}/MarvinCaptureCLI" "${DIST}/libmarvin-core.so" "${LIBDIR}/"
strip --strip-unneeded "${LIBDIR}/MarvinCaptureCLI" "${LIBDIR}/libmarvin-core.so"
install -m 644 "${DIST}/firmware/"* "${LIBDIR}/firmware/"
# Relative link: the loader resolves $ORIGIN, and the core resolves firmware/, via the real path.
ln -s "../lib/${PKG}/MarvinCaptureCLI" "${STAGE}/usr/bin/MarvinCaptureCLI"
for r in "${RULES[@]}"; do
    install -m 644 "${ROOT}/packaging/linux/${r}" "${STAGE}/usr/lib/udev/rules.d/${r}"
done

# --- Dependencies -----------------------------------------------------------------
# Only libc (+ libm/libdl/libpthread/librt, all in libc6), libgcc_s and libusb may be
# linked dynamically; anything else would need its own Depends entry.
for bin in "${LIBDIR}/MarvinCaptureCLI" "${LIBDIR}/libmarvin-core.so"; do
    while read -r lib; do
        case "${lib}" in
            libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|librt.so.*|libgcc_s.so.*|ld-linux*|libusb-1.0.so.*|libmarvin-core.so) ;;
            *) die "$(basename "${bin}") links ${lib}: add it to Depends in scripts/package-linux.sh" ;;
        esac
    done < <(objdump -p "${bin}" | awk '/^  NEEDED/ {print $2}')
done
# glibc: the highest versioned symbol the binaries import.
GLIBC="$(objdump -T "${LIBDIR}/MarvinCaptureCLI" "${LIBDIR}/libmarvin-core.so" \
    | grep -o 'GLIBC_[0-9][0-9.]*' | sed 's/^GLIBC_//' | sort -uV | tail -n1 || true)"
[[ -n "${GLIBC}" ]] || GLIBC="2.31"
# libusb: 1.0.23 (Ubuntu 20.04 / Debian 11) is the oldest supported baseline. A core built
# against libusb >= 1.0.28 (LIBUSB_API_VERSION 0x0100010C) imports the raw-IO calls.
LIBUSB_MIN="2:1.0.23"
if objdump -T "${LIBDIR}/libmarvin-core.so" | grep -q 'libusb_endpoint_set_raw_io'; then
    LIBUSB_MIN="2:1.0.28"
fi
DEPENDS="libc6 (>= ${GLIBC}), libusb-1.0-0 (>= ${LIBUSB_MIN})"
echo "Depends: ${DEPENDS}"

# --- copyright (DEP-5) --------------------------------------------------------------
# The AGPL is not in /usr/share/common-licenses, so its text is included in full.
{
    cat <<'COPYRIGHT'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: MarvinCapture
Upstream-Contact: Jonas Cz.
Source: https://github.com/JonasCz/MarvinCapture
Comment: FFmpeg (libavformat, libavcodec, libavutil; LGPL-2.1-or-later, built
 without GPL or nonfree components) is statically linked into libmarvin-core.so.
 The FPGA bitstreams in firmware/ are Pinnacle's copyright and are not licensed
 by this package; see firmware/README.md.

Files: *
Copyright: 2026 Jonas Cz.
License: AGPL-3+

Files: firmware/*.bin
Copyright: Pinnacle Systems / Avid Technology (see firmware/README.md)
License: Pinnacle-proprietary
 Extracted from the vendor driver of the discontinued device and shipped
 because the hardware is inert without them. No claim of ownership is made and
 no licence is granted by this package.

Files: third_party/*
Copyright: FFmpeg developers
License: LGPL-2.1+

License: LGPL-2.1+
 On Debian systems, the full text of the GNU Lesser General Public License
 version 2.1 can be found in /usr/share/common-licenses/LGPL-2.1.

License: AGPL-3+
COPYRIGHT
    sed -e 's/[[:space:]]*$//' -e 's/^$/ ./' -e 's/^/ /' "${ROOT}/LICENSE"
} > "${DOCDIR}/copyright"
chmod 644 "${DOCDIR}/copyright"

# --- control ------------------------------------------------------------------------
INSTALLED_KB="$(du -sk --exclude=DEBIAN "${STAGE}" | cut -f1)"
cat > "${STAGE}/DEBIAN/control" <<CONTROL
Package: ${PKG}
Version: ${VERSION}
Section: video
Priority: optional
Architecture: ${DEB_ARCH}
Maintainer: Jonas Cz.
Installed-Size: ${INSTALLED_KB}
Depends: ${DEPENDS}
Recommends: libcurl4t64 | libcurl4
Homepage: https://github.com/JonasCz/MarvinCapture
Description: capture driver and command-line tool for Pinnacle Marvin USB DV boxes
 MarvinCapture is a user-space (libusb) driver and command-line program for
 the Pinnacle Studio 500-USB, 510-USB, 700-USB and 710-USB and the MovieBox
 Deluxe capture boxes.
 .
 It captures DV and HDV over FireWire, with deck control, and composite or
 S-video analog input, to DV, AVI, MOV, TS and MKV files.
 .
 Udev rules are included so that no root access is needed for the device or for
 /dev/cpu_dma_latency (which the program uses to avoid lost USB data).
CONTROL
install -m 755 "${ROOT}/packaging/linux/postinst" "${ROOT}/packaging/linux/postrm" "${STAGE}/DEBIAN/"

# --- 3. Build ---------------------------------------------------------------------------
step "Building ${DEB##*/}"
mkdir -p "${OUT}"
rm -f "${DEB}"
dpkg-deb --build --root-owner-group "${STAGE}" "${DEB}"

dpkg-deb -I "${DEB}" | sed -n '/^ Package:/,/^ Depends:/p'
printf '\n\033[32mDone: %s\033[0m\n' "${DEB}"
