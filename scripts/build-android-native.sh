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

# scripts/build-android-native.sh
#
# Cross-builds the native part of the Android app with the NDK (version and API
# level in scripts/android-ndk.sh), per ABI:
#
#   1. (first time, or when the scripts changed) the minimal static FFmpeg:
#      scripts/build-ffmpeg.sh --android ABI -> third_party/ffmpeg-android-<arch>
#   2. (first time) libusb 1.0.30 from its release tarball (sha256 pinned below),
#      static, no udev -> third_party/libusb-android-<arch>
#   3. marvin-core and MarvinCaptureCLI with CMake and the NDK's toolchain file,
#      without the ctest suite (PIN_BUILD_TESTS=OFF) -> build/android-<abi>/core
#   4. the app's JNI layer (gui/android/app/src/main/cpp: pin_api.h wrappers, preview
#      renderer, audio monitor), linked against that core -> build/android-<abi>/jni
#   5. build/android-<abi>/dist, stripped:
#
#        libmarvin-core.so    the core, FFmpeg and libusb linked in
#        libmarvin-jni.so     the app's JNI layer (needs libmarvin-core.so)
#        MarvinCaptureCLI     the command line, for Termux (finds the .so next to it)
#        firmware/            FPGA bitstreams (the app copies them out of its assets)
#
#   and checks that the libraries need no libusb / FFmpeg shared library, have no text
#   relocations and that their LOAD segments are 16 KB aligned (Android 15+ devices with
#   16 KB pages; Google Play). The Gradle project (gui/android) packages the two .so files
#   from dist; scripts/build-android.sh runs this script and then Gradle.
#
# <arch> is arm64 or x86_64 (the layout of the other third_party/ builds); <abi> is
# Android's name: arm64-v8a or x86_64 (the emulator).
#
# Requires: the NDK (ANDROID_NDK_HOME, or ANDROID_HOME with ndk/<version>), cmake,
# ninja, make, pkg-config, curl, tar with bzip2, and nasm for x86_64.
#
# Usage:
#   scripts/build-android-native.sh [--abi arm64-v8a|x86_64]... [--clean]
#
#   --abi ABI   build only this ABI (repeatable; default: arm64-v8a and x86_64)
#   --clean     delete build/android-<abi>/ first (third_party/ builds are kept;
#               scripts/build-ffmpeg.sh --android ABI --clean redoes FFmpeg)

set -euo pipefail

usage() { sed -n '/^# Usage:/,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

ABIS=()
CLEAN=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --abi)
            [[ $# -ge 2 ]] || { echo "build-android-native.sh: --abi needs arm64-v8a or x86_64" >&2; exit 2; }
            ABIS+=("$2"); shift 2 ;;
        --abi=*) ABIS+=("${1#--abi=}"); shift ;;
        --clean) CLEAN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "build-android-native.sh: unknown option '$1'" >&2; usage >&2; exit 2 ;;
    esac
done
[[ ${#ABIS[@]} -gt 0 ]] || ABIS=(arm64-v8a x86_64)

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=android-ndk.sh
source "${ROOT}/scripts/android-ndk.sh"

step() { printf '\n\033[36m== %s\033[0m\n' "$*"; }
die()  { echo "build-android-native.sh: $*" >&2; exit 1; }

for abi in "${ABIS[@]}"; do
    pin_android_abi "${abi}" || exit 2
done
pin_find_ndk || exit 1
echo "NDK: ${PIN_NDK} ($(pin_ndk_version_of "${PIN_NDK}")), API ${PIN_ANDROID_API}"
for tool in cmake ninja make pkg-config curl tar; do
    command -v "${tool}" >/dev/null 2>&1 || die "'${tool}' not found"
done

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$@" | awk '{print $1}'
    else
        shasum -a 256 "$@" | awk '{print $1}'
    fi
}

NPROC="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
READELF="$(pin_ndk_tool llvm-readelf)"
STRIP="$(pin_ndk_tool llvm-strip)"

# --- libusb ------------------------------------------------------------------------
# Built from the release tarball (not the git tag: that would need autoreconf). 1.0.30
# has the Android fixes for wrapped file descriptors. LGPL-2.1+, linked statically
# into libmarvin-core.so; see third_party/README.md.
LIBUSB_VERSION="1.0.30"
LIBUSB_TARBALL="libusb-${LIBUSB_VERSION}.tar.bz2"
LIBUSB_URL="https://github.com/libusb/libusb/releases/download/v${LIBUSB_VERSION}/${LIBUSB_TARBALL}"
# sha256 computed once from the file at LIBUSB_URL.
LIBUSB_SHA256="fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf"
LIBUSB_SRC_DIR="${ROOT}/third_party/libusb-src"

fetch_libusb() {
    mkdir -p "${LIBUSB_SRC_DIR}"
    local tarball="${LIBUSB_SRC_DIR}/${LIBUSB_TARBALL}"
    local fresh=0 got
    download_libusb() {
        echo "Downloading ${LIBUSB_URL}"
        rm -f "${tarball}.part"
        curl -fL --retry 3 -o "${tarball}.part" "${LIBUSB_URL}"
        mv "${tarball}.part" "${tarball}"
    }
    if [[ ! -f "${tarball}" ]]; then
        download_libusb
        fresh=1
    fi
    got="$(sha256_of "${tarball}")"
    if [[ "${got}" != "${LIBUSB_SHA256}" && "${fresh}" -eq 0 ]]; then
        # a damaged or partial file from an earlier run: never trusted again
        echo "cached ${tarball} fails its sha256; deleting it and downloading again" >&2
        rm -f "${tarball}"
        download_libusb
        got="$(sha256_of "${tarball}")"
    fi
    if [[ "${got}" != "${LIBUSB_SHA256}" ]]; then
        rm -f "${tarball}"
        die "sha256 mismatch for ${tarball}: expected ${LIBUSB_SHA256}, got ${got}"
    fi
    # extracted aside and renamed into place only when tar finished: an interrupted extraction
    # must not leave a half tree that the next run trusts
    if [[ ! -d "${LIBUSB_SRC_DIR}/libusb-${LIBUSB_VERSION}" ]]; then
        local tmp="${LIBUSB_SRC_DIR}/.extract-$$"
        rm -rf "${tmp}"
        mkdir -p "${tmp}"
        if ! tar -C "${tmp}" -xjf "${tarball}" || [[ ! -d "${tmp}/libusb-${LIBUSB_VERSION}" ]]; then
            rm -rf "${tmp}"
            die "extracting ${tarball} failed"
        fi
        mv "${tmp}/libusb-${LIBUSB_VERSION}" "${LIBUSB_SRC_DIR}/libusb-${LIBUSB_VERSION}"
        rm -rf "${tmp}"
    fi
}

# build_libusb ABI PREFIX
build_libusb() {
    local abi="$1" prefix="$2"
    local stamp="${prefix}/build-stamp"
    # rebuilt when the version, the NDK or the API level changes
    local want="libusb ${LIBUSB_VERSION} ndk ${PIN_NDK_VERSION} api ${PIN_ANDROID_API}"
    if [[ -f "${prefix}/lib/libusb-1.0.a" && -f "${stamp}" && "$(cat "${stamp}")" == "${want}" ]]; then
        return 0
    fi
    step "Building libusb ${LIBUSB_VERSION} for ${abi}"
    fetch_libusb
    local build="${prefix}-build"
    rm -rf "${build}" "${prefix}"
    mkdir -p "${build}"
    local cc
    cc="$(pin_ndk_tool "${PIN_ABI_TRIPLE}${PIN_ANDROID_API}-clang")"
    [[ -x "${cc}" ]] || die "${cc} not found"
    # No udev on Android: devices come as file descriptors (libusb_wrap_sys_device).
    (cd "${build}" &&
        "${LIBUSB_SRC_DIR}/libusb-${LIBUSB_VERSION}/configure" \
            --host="${PIN_ABI_TRIPLE}" --prefix="${prefix}" \
            --disable-udev --enable-static --disable-shared --with-pic \
            --disable-examples-build --disable-tests-build \
            CC="${cc}" AR="$(pin_ndk_tool llvm-ar)" RANLIB="$(pin_ndk_tool llvm-ranlib)" \
            STRIP="${STRIP}" CFLAGS="-O2" > configure.log &&
        make -j"${NPROC}" > make.log &&
        make install > install.log) || die "libusb build failed (see ${build}/*.log)"
    echo "${want}" > "${stamp}"
}

# --- per ABI -------------------------------------------------------------------------
TOOLCHAIN_FILE="${PIN_NDK}/build/cmake/android.toolchain.cmake"
[[ -f "${TOOLCHAIN_FILE}" ]] || die "${TOOLCHAIN_FILE} not found"
FF_HASH="$(cat "${ROOT}/scripts/build-ffmpeg.sh" "${ROOT}/scripts/android-ndk.sh" | sha256_of)"
SUMMARY=()

for abi in "${ABIS[@]}"; do
    pin_android_abi "${abi}"
    BUILD="${ROOT}/build/android-${abi}"
    CORE="${BUILD}/core"
    DIST="${BUILD}/dist"
    if [[ "${CLEAN}" -eq 1 && -d "${BUILD}" ]]; then
        step "Cleaning build/android-${abi}/"
        rm -rf "${BUILD}"
    fi

    # 1. FFmpeg (the same stamp as scripts/build.sh's, plus android-ndk.sh)
    FF_PREFIX="${ROOT}/third_party/ffmpeg-android-${PIN_ABI_DIR}"
    FF_STAMP="${FF_PREFIX}/build-script.sha256"
    if [[ ! -d "${FF_PREFIX}/lib/pkgconfig" || ! -f "${FF_STAMP}" ||
          "$(tr -d '[:space:]' < "${FF_STAMP}")" != "${FF_HASH}" ]]; then
        step "Building the minimal static FFmpeg for ${abi} (first time, or the script changed)"
        bash "${ROOT}/scripts/build-ffmpeg.sh" --android "${abi}"
    fi

    # 2. libusb
    USB_PREFIX="${ROOT}/third_party/libusb-android-${PIN_ABI_DIR}"
    build_libusb "${abi}" "${USB_PREFIX}"

    # 3. core + CLI. pkg-config sees only the Android libusb (PKG_CONFIG_LIBDIR) and,
    # through src/sinks, the Android FFmpeg; never the build machine's libraries. The
    # prefixes go into CMAKE_FIND_ROOT_PATH so the toolchain file's "only in the sysroot"
    # library search still finds them.
    step "Configuring marvin-core for ${abi}"
    PKG_CONFIG_LIBDIR="${USB_PREFIX}/lib/pkgconfig" PKG_CONFIG_PATH="" \
    cmake -G Ninja -S "${ROOT}" -B "${CORE}" \
        -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
        -DANDROID_ABI="${abi}" \
        -DANDROID_PLATFORM="android-${PIN_ANDROID_API}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_FIND_ROOT_PATH="${USB_PREFIX};${FF_PREFIX}" \
        -DPIN_BUILD_TESTS=OFF \
        -DPIN_FFMPEG_PREFIX="${FF_PREFIX}"
    step "Building marvin-core for ${abi}"
    PKG_CONFIG_LIBDIR="${USB_PREFIX}/lib/pkgconfig" PKG_CONFIG_PATH="" cmake --build "${CORE}"

    # 4. the app's JNI layer, against the core just built
    JNI="${BUILD}/jni"
    step "Building the JNI layer (libmarvin-jni.so) for ${abi}"
    cmake -G Ninja -S "${ROOT}/gui/android/app/src/main/cpp" -B "${JNI}" \
        -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
        -DANDROID_ABI="${abi}" \
        -DANDROID_PLATFORM="android-${PIN_ANDROID_API}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DPIN_SOURCE_DIR="${ROOT}" \
        -DPIN_CORE_LIBRARY="${CORE}/libmarvin-core.so"
    cmake --build "${JNI}"

    # 5. dist
    step "Assembling build/android-${abi}/dist"
    rm -rf "${DIST}"
    mkdir -p "${DIST}/firmware"
    cp -f "${CORE}/libmarvin-core.so" "${CORE}/MarvinCaptureCLI" "${JNI}/libmarvin-jni.so" "${DIST}/"
    "${STRIP}" --strip-unneeded "${DIST}/libmarvin-core.so" "${DIST}/libmarvin-jni.so"
    "${STRIP}" "${DIST}/MarvinCaptureCLI"
    cp -f "${ROOT}/firmware/fpga-ohci.bin" "${ROOT}/firmware/fpga-capture.bin" \
          "${ROOT}/firmware/fx2-marvin.bin" "${ROOT}/firmware/README.md" "${DIST}/firmware/"

    # checks
    for SO in "${DIST}/libmarvin-core.so" "${DIST}/libmarvin-jni.so"; do
        needed="$("${READELF}" -d "${SO}" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | tr '\n' ' ')"
        if grep -Eq 'libusb|libav|libsw' <<< "${needed}"; then
            die "${SO} needs a shared libusb / FFmpeg library: ${needed}"
        fi
        if "${READELF}" -d "${SO}" | grep -q TEXTREL; then
            die "${SO} has text relocations (Android refuses to load those)"
        fi
        bad_align="$("${READELF}" -lW "${SO}" | awk '$1 == "LOAD" && $NF != "0x4000" { print $NF }')"
        [[ -z "${bad_align}" ]] || die "${SO}: LOAD segments not 16 KB aligned (${bad_align})"
        [[ -n "$("${READELF}" -lW "${SO}" | awk '$1 == "LOAD"')" ]] || die "${SO}: no LOAD segments?"
        size="$(wc -c < "${SO}" | tr -d '[:space:]')"
        SUMMARY+=("${abi}: $(basename "${SO}") ${size} bytes, needs: ${needed}")
    done
    runpath="$("${READELF}" -d "${DIST}/MarvinCaptureCLI" | sed -n 's/.*(RUNPATH).*\[\(.*\)\]/\1/p')"
    [[ "${runpath}" == '$ORIGIN' ]] || die "MarvinCaptureCLI: RUNPATH is '${runpath}', not \$ORIGIN"
done

printf '\n\033[32mDone.\033[0m\n'
for line in "${SUMMARY[@]}"; do
    echo "  ${line}"
done
