#!/usr/bin/env bash
# scripts/build-ffmpeg.sh
#
# Downloads, verifies and builds a minimal, static, LGPL-only FFmpeg for
# marvin-core. Only the codecs/muxers/demuxers/parsers/protocol that
# the core actually uses are enabled -- no GPL or nonfree code, no network,
# no autodetected external libraries.
#
# Works on MSYS2 UCRT64 (Windows), Linux and macOS, and cross-compiles for
# Android with the NDK (scripts/android-ndk.sh says which NDK and API level).
#
# Usage:
#   scripts/build-ffmpeg.sh                     # download, verify, configure, build, install
#   scripts/build-ffmpeg.sh --clean             # remove the build/install dirs for this OS/arch first
#   scripts/build-ffmpeg.sh --android ABI       # Android, ABI arm64-v8a or x86_64 (with --clean too)
#
# Output goes to third_party/ffmpeg-<os>-<arch>/ (the install prefix; Android:
# third_party/ffmpeg-android-arm64 or -x86_64), with sources fetched into
# third_party/ffmpeg-src/. Neither is committed to git.

set -euo pipefail

# ---------------------------------------------------------------------------
# Pinned release
# ---------------------------------------------------------------------------
FFMPEG_VERSION="8.1.3"
FFMPEG_TARBALL="ffmpeg-${FFMPEG_VERSION}.tar.xz"
FFMPEG_URL="https://ffmpeg.org/releases/${FFMPEG_TARBALL}"
# sha256 computed by hand from the file fetched above (ffmpeg.org does not
# publish a .sha256sum next to the .asc signature for this release).
FFMPEG_SHA256="7138d28c96d9d3e3af4ee3d8cad72741f8ffb40da90c1112235dea3ecd3178a3"

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
THIRD_PARTY="${REPO_ROOT}/third_party"
SRC_DIR="${THIRD_PARTY}/ffmpeg-src"
TARBALL_PATH="${SRC_DIR}/${FFMPEG_TARBALL}"

# ---------------------------------------------------------------------------
# Arguments
# ---------------------------------------------------------------------------
CLEAN=0
ANDROID_ABI=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean) CLEAN=1; shift ;;
        --android)
            [[ $# -ge 2 ]] || { echo "build-ffmpeg.sh: --android needs an ABI (arm64-v8a or x86_64)" >&2; exit 2; }
            ANDROID_ABI="$2"; shift 2 ;;
        --android=*) ANDROID_ABI="${1#--android=}"; shift ;;
        *) echo "build-ffmpeg.sh: unknown option '$1'" >&2; exit 2 ;;
    esac
done

# ---------------------------------------------------------------------------
# OS / arch detection (the target's: Android is cross-compiled)
# ---------------------------------------------------------------------------
UNAME_S="$(uname -s)"
[[ -n "${ANDROID_ABI}" ]] && UNAME_S="Android"
case "${UNAME_S}" in
    MINGW*|MSYS*|CYGWIN*|UCRT*|CLANG*)
        OS_NAME="windows"
        THREAD_FLAG="--enable-w32threads"
        ;;
    Linux)
        OS_NAME="linux"
        THREAD_FLAG="--enable-pthreads"
        ;;
    Darwin)
        OS_NAME="macos"
        THREAD_FLAG="--enable-pthreads"
        ;;
    Android)
        OS_NAME="android"
        THREAD_FLAG="--enable-pthreads"
        # shellcheck source=android-ndk.sh
        source "${SCRIPT_DIR}/android-ndk.sh"
        pin_android_abi "${ANDROID_ABI}" || exit 2
        pin_find_ndk || exit 1
        ;;
    *)
        echo "build-ffmpeg.sh: unrecognized OS '${UNAME_S}'" >&2
        exit 1
        ;;
esac

ARCH_NAME="$(uname -m)"
# MSYS2's own runtime is x86_64 even on Windows on Arm (it runs emulated there), so
# uname -m says x86_64 in the CLANGARM64 shell too; the target is the environment's
# (MSYSTEM_CARCH: x86_64 for UCRT64, aarch64 for CLANGARM64).
if [[ "${OS_NAME}" == windows && -n "${MSYSTEM_CARCH:-}" ]]; then
    ARCH_NAME="${MSYSTEM_CARCH}"
fi
case "${ARCH_NAME}" in
    x86_64|amd64) ARCH_NAME="x86_64" ;;
    aarch64|arm64) ARCH_NAME="arm64" ;;
esac
[[ "${OS_NAME}" == android ]] && ARCH_NAME="${PIN_ABI_DIR}"

BUILD_DIR="${THIRD_PARTY}/ffmpeg-${OS_NAME}-${ARCH_NAME}-build"
PREFIX_DIR="${THIRD_PARTY}/ffmpeg-${OS_NAME}-${ARCH_NAME}"

if [[ "${CLEAN}" -eq 1 ]]; then
    echo "Removing ${BUILD_DIR} and ${PREFIX_DIR}"
    rm -rf "${BUILD_DIR}" "${PREFIX_DIR}"
fi

# sha256 of a file (or, with no argument, of stdin): sha256sum (Linux, MSYS2, macOS 15+) or
# shasum (older macOS).
sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$@" | awk '{print $1}'
    else
        shasum -a 256 "$@" | awk '{print $1}'
    fi
}

NPROC="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

# FFmpeg's configure refuses a source path with whitespace in it ("Out of
# tree builds are impossible ..."), and its Makefiles would not cope either.
# When the checkout lives under such a path (e.g. "~/Documents/Personal
# software dev/"), build through a whitespace-free symlink to third_party/
# instead. The symlink's path is derived from the checkout's path so it is
# the same on every run (the build tree's dependency files record it, so a
# random one would break incremental rebuilds); the installed .pc files are
# made prefix-relative afterwards (see below), so nothing outside the build
# tree depends on it.
WORK_THIRD_PARTY="${THIRD_PARTY}"
if [[ "${THIRD_PARTY}" == *[[:space:]]* ]]; then
    LINK_TMP="${TMPDIR:-/tmp}"
    LINK_TMP="${LINK_TMP%/}"
    LINK_DIR="${LINK_TMP}/marvin-ffmpeg-$(printf '%s' "${THIRD_PARTY}" | cksum | awk '{print $1}')"
    if [[ "${LINK_DIR}" == *[[:space:]]* ]]; then
        echo "build-ffmpeg.sh: ${THIRD_PARTY} contains whitespace and so does ${LINK_DIR}" >&2
        exit 1
    fi
    mkdir -p "${LINK_DIR}"
    ln -sfn "${THIRD_PARTY}" "${LINK_DIR}/third_party"
    WORK_THIRD_PARTY="${LINK_DIR}/third_party"
    echo "Path has whitespace: building through ${WORK_THIRD_PARTY}"
fi
WORK_BUILD_DIR="${WORK_THIRD_PARTY}/ffmpeg-${OS_NAME}-${ARCH_NAME}-build"
WORK_PREFIX_DIR="${WORK_THIRD_PARTY}/ffmpeg-${OS_NAME}-${ARCH_NAME}"
WORK_EXTRACT_DIR="${WORK_THIRD_PARTY}/ffmpeg-src/ffmpeg-${FFMPEG_VERSION}"

# ---------------------------------------------------------------------------
# Fetch + verify
# ---------------------------------------------------------------------------
mkdir -p "${SRC_DIR}"

download_tarball() {
    echo "Downloading ${FFMPEG_URL}"
    rm -f "${TARBALL_PATH}.part"
    curl -fL --retry 3 -o "${TARBALL_PATH}.part" "${FFMPEG_URL}"
    mv "${TARBALL_PATH}.part" "${TARBALL_PATH}"
}

fresh_download=0
if [[ ! -f "${TARBALL_PATH}" ]]; then
    download_tarball
    fresh_download=1
fi

echo "Verifying sha256"
computed_sha256="$(sha256_of "${TARBALL_PATH}")"
if [[ "${computed_sha256}" != "${FFMPEG_SHA256}" && "${fresh_download}" -eq 0 ]]; then
    # a damaged or partial file from an earlier run: never trusted again
    echo "build-ffmpeg.sh: cached ${TARBALL_PATH} fails its sha256; deleting it and downloading again" >&2
    rm -f "${TARBALL_PATH}"
    download_tarball
    computed_sha256="$(sha256_of "${TARBALL_PATH}")"
fi
if [[ "${computed_sha256}" != "${FFMPEG_SHA256}" ]]; then
    echo "build-ffmpeg.sh: sha256 mismatch for ${TARBALL_PATH}" >&2
    echo "  expected: ${FFMPEG_SHA256}" >&2
    echo "  got:      ${computed_sha256}" >&2
    rm -f "${TARBALL_PATH}"
    exit 1
fi

# Extracted next to the final folder and renamed into place only when tar finished, so an
# interrupted extraction is never mistaken for the source tree.
EXTRACT_DIR="${SRC_DIR}/ffmpeg-${FFMPEG_VERSION}"
if [[ ! -d "${EXTRACT_DIR}" ]]; then
    echo "Extracting ${FFMPEG_TARBALL}"
    EXTRACT_TMP="${SRC_DIR}/.extract-$$"
    rm -rf "${EXTRACT_TMP}"
    mkdir -p "${EXTRACT_TMP}"
    if ! tar -C "${EXTRACT_TMP}" -xf "${TARBALL_PATH}" || [[ ! -d "${EXTRACT_TMP}/ffmpeg-${FFMPEG_VERSION}" ]]; then
        rm -rf "${EXTRACT_TMP}"
        echo "build-ffmpeg.sh: extracting ${TARBALL_PATH} failed" >&2
        exit 1
    fi
    mv "${EXTRACT_TMP}/ffmpeg-${FFMPEG_VERSION}" "${EXTRACT_DIR}"
    rm -rf "${EXTRACT_TMP}"
fi

# ---------------------------------------------------------------------------
# Configure
# ---------------------------------------------------------------------------
mkdir -p "${BUILD_DIR}"
cd "${WORK_BUILD_DIR}"

# Components actually used by marvin-core:
#   encode:  FFV1 + FLAC (analog capture), PCM s16le; MPEG-2 video + MP2 (the HDV
#            MOV/MKV writer re-encodes damaged GOPs and fills lost audio with silence,
#            sink_hdvmux.c; both LGPL); per platform the OS's own H.264 / HEVC
#            encoder (the hardware-encoder sink, sink_hwenc.c; see the case below)
#   decode:  FFV1, FLAC, DV (dvvideo: preview, and DV to H.264 / HEVC), MPEG-2 video (HDV),
#            PCM s16le/s16be, MP2 (HDV audio)
#   filter:  bwdif (the H.264 / HEVC sink's "deinterlace" option); configure always adds the
#            buffer source and sink a filter graph needs. No swscale: the sink converts
#            the few pixel formats it sees itself.
#   mux:     Matroska (FFV1+FLAC sink), MOV, AVI, DV, MPEG-TS, NUT (analog to stdout)
#   demux:   DV, MPEG-TS, Matroska, MOV, AVI, NUT (rewrap sources / smoke test readback)
#   parse:   mpegvideo (MPEG-2), mpegaudio (MP2), flac (FLAC-in-Matroska readback)
#   proto:   file
CONFIGURE_ARGS=(
    --prefix="${WORK_PREFIX_DIR}"

    # LGPL-only: no --enable-gpl, no --enable-nonfree, no --enable-version3
    # forced (LGPL v2.1+ is fine as-is).

    # Static libs only.
    --enable-static
    --disable-shared
    --enable-pic

    # Start from nothing and opt in explicitly.
    --disable-everything
    --disable-programs
    --disable-doc
    --disable-network
    --disable-autodetect
    --disable-avdevice
    --enable-avfilter
    --disable-iconv
    --disable-zlib
    --disable-bzlib
    --disable-lzma
    --disable-swscale
    --disable-swresample
    --disable-debug

    "${THREAD_FLAG}"

    --enable-encoder=ffv1
    --enable-encoder=flac
    --enable-encoder=pcm_s16le
    --enable-encoder=mpeg2video
    --enable-encoder=mp2

    --enable-decoder=ffv1
    --enable-decoder=flac
    --enable-decoder=dvvideo
    --enable-decoder=mpeg2video
    --enable-decoder=pcm_s16le
    --enable-decoder=pcm_s16be
    --enable-decoder=mp2
    --enable-decoder=mp2float

    --enable-muxer=matroska
    --enable-muxer=mov
    --enable-muxer=avi
    --enable-muxer=dv
    --enable-muxer=mpegts
    --enable-muxer=nut

    --enable-demuxer=dv
    --enable-demuxer=mpegts
    --enable-demuxer=matroska
    --enable-demuxer=mov
    --enable-demuxer=avi
    --enable-demuxer=nut

    --enable-parser=mpegvideo
    --enable-parser=mpegaudio
    --enable-parser=flac

    --enable-protocol=file

    --enable-filter=bwdif

    --disable-bsfs
)

# Per-platform additions: the target, the toolchain, and each platform's own H.264 / HEVC
# encoder for the hardware-encoder sink (src/sinks/sink_hwenc.c). All of them are wrappers
# around an encoder that comes with the OS, so the build stays LGPL: nothing GPL (x264,
# x265) or nonfree goes in. --disable-autodetect (above) means each system framework has
# to be named here. Linux gets none: it has no encoder of its own that is LGPL-safe to
# use (VAAPI needs libva and the driver), so the core does not offer those formats there.
case "${OS_NAME}" in
    windows)
        # Name the target explicitly, configure would take it from uname (MSYS2's
        # own, see above). The CLANGARM64 environment has clang, not gcc.
        CONFIGURE_ARGS+=(--target-os=mingw32)
        [[ "${ARCH_NAME}" == arm64 ]] && CONFIGURE_ARGS+=(--arch=aarch64)
        command -v gcc >/dev/null 2>&1 || CONFIGURE_ARGS+=(--cc=clang)
        # Media Foundation: the GPU vendor's encoder MFT (hw_encoding=1) or Microsoft's
        # software one. mfplat.dll is loaded at run time; the static libs add mfuuid,
        # ole32 and strmiids (libavcodec.pc). FFmpeg 8.1's mfenc.c uses the D3D11 types
        # unconditionally (it compiles only with d3d11va), so that is on as well: only
        # libavutil's D3D11 device context, d3d11.dll and dxgi.dll loaded at run time (no
        # import libraries), no hardware decoding (no hwaccels are enabled).
        CONFIGURE_ARGS+=(
            --enable-mediafoundation
            --enable-d3d11va
            --enable-encoder=h264_mf
            --enable-encoder=hevc_mf
        )
        ;;
    macos)
        # VideoToolbox (CoreFoundation, CoreMedia, CoreVideo and VideoToolbox frameworks,
        # listed in libavcodec.pc / libavutil.pc as "-framework X").
        CONFIGURE_ARGS+=(
            --enable-videotoolbox
            --enable-encoder=h264_videotoolbox
            --enable-encoder=hevc_videotoolbox
        )
        ;;
    android)
        # NDK clang for the app's minimum API level; static libraries with PIC, linked
        # into libmarvin-core.so.
        NDK_CC="$(pin_ndk_tool "${PIN_ABI_TRIPLE}${PIN_ANDROID_API}-clang")"
        [[ -x "${NDK_CC}" ]] || { echo "build-ffmpeg.sh: ${NDK_CC} not found" >&2; exit 1; }
        CONFIGURE_ARGS+=(
            --target-os=android
            --enable-cross-compile
            --arch="${PIN_ABI_FFARCH}"
            --cc="${NDK_CC}"
            --cxx="${NDK_CC}++"
            --ar="$(pin_ndk_tool llvm-ar)"
            --nm="$(pin_ndk_tool llvm-nm)"
            --ranlib="$(pin_ndk_tool llvm-ranlib)"
            --strip="$(pin_ndk_tool llvm-strip)"
            --sysroot="$(dirname "$(dirname "${NDK_CC}")")/sysroot"
        )
        # MediaCodec through the NDK (AMediaCodec; FFmpeg dlopens libmediandk.so, so no
        # JVM is needed: the sink sets ndk_codec=1). configure insists on --enable-jni
        # for it. The encoders need these bitstream filters (--disable-bsfs above turns
        # them off, and a disabled dependency would silently drop the encoder):
        # extract_extradata for the global header Matroska wants, h264_metadata /
        # hevc_metadata to crop the 16-aligned picture back (not needed for 720 x 576
        # or 480, but the encoders do not build without them).
        CONFIGURE_ARGS+=(
            --enable-jni
            --enable-mediacodec
            --enable-encoder=h264_mediacodec
            --enable-encoder=hevc_mediacodec
            --enable-bsf=extract_extradata
            --enable-bsf=h264_metadata
            --enable-bsf=hevc_metadata
        )
        ;;
esac

# x86 assembly needs nasm. Without it FFmpeg's configure stops; a build
# without asm is slower but fine for what we use (FFV1 is mostly C), so
# degrade rather than make nasm a hard requirement.
if [[ "${ARCH_NAME}" == x86_64 || "${ARCH_NAME}" == i?86 ]] && ! command -v nasm >/dev/null 2>&1; then
    echo "nasm not found: building without x86 assembly (slower)."
    CONFIGURE_ARGS+=(--disable-x86asm)
fi

echo "Configuring FFmpeg ${FFMPEG_VERSION} for ${OS_NAME}-${ARCH_NAME}"
echo "${WORK_EXTRACT_DIR}/configure ${CONFIGURE_ARGS[*]}"
"${WORK_EXTRACT_DIR}/configure" "${CONFIGURE_ARGS[@]}" 2>&1 | tee configure.log

echo
echo "=== ffbuild/config.log tail (auto-selected deps, if any) ==="
grep -iE "requires|selecting|error" ffbuild/config.log 2>/dev/null | tail -40 || true

# ---------------------------------------------------------------------------
# Build + install
# ---------------------------------------------------------------------------
make -j"${NPROC}"
make install

if [[ "${WORK_PREFIX_DIR}" != "${PREFIX_DIR}" ]]; then
    # Installed through the symlink: make the .pc files relative to their own
    # location so they keep working after the symlink is gone.
    for pc in "${PREFIX_DIR}"/lib/pkgconfig/*.pc; do
        sed -e 's|^prefix=.*$|prefix=${pcfiledir}/../..|' \
            -e 's|^libdir=.*$|libdir=${prefix}/lib|' \
            -e 's|^includedir=.*$|includedir=${prefix}/include|' \
            "${pc}" > "${pc}.tmp"
        mv "${pc}.tmp" "${pc}"
    done
fi

# scripts/build.ps1, scripts/build.sh and scripts/build-android-native.sh rebuild
# FFmpeg when this stamp no longer matches this script (the script holds the whole
# component list); for Android it also covers android-ndk.sh (NDK, API level).
if [[ "${OS_NAME}" == android ]]; then
    cat "${SCRIPT_DIR}/build-ffmpeg.sh" "${SCRIPT_DIR}/android-ndk.sh" | sha256_of > "${PREFIX_DIR}/build-script.sha256"
else
    sha256_of "${SCRIPT_DIR}/build-ffmpeg.sh" > "${PREFIX_DIR}/build-script.sha256"
fi

# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------
echo
echo "=== Installed static libs ==="
ls -la "${PREFIX_DIR}/lib"/*.a 2>/dev/null || true

echo
echo "=== pkg-config --static --libs libavfilter libavformat ==="
PKG_CONFIG_PATH="${PREFIX_DIR}/lib/pkgconfig" pkg-config --static --libs libavfilter libavformat || true

echo
echo "=== Encoders and filters built in ==="
grep -E '^#define CONFIG_[A-Z0-9_]+_(ENCODER|FILTER) 1' "${WORK_BUILD_DIR}/config_components.h" || true
