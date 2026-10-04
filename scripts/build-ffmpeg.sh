#!/usr/bin/env bash
# scripts/build-ffmpeg.sh
#
# Downloads, verifies and builds a minimal, static, LGPL-only FFmpeg for
# marvin-core. Only the codecs/muxers/demuxers/parsers/protocol that
# the core actually uses are enabled -- no GPL or nonfree code, no network,
# no autodetected external libraries.
#
# Works on MSYS2 UCRT64 (Windows), Linux and macOS.
#
# Usage:
#   scripts/build-ffmpeg.sh            # download, verify, configure, build, install
#   scripts/build-ffmpeg.sh --clean    # remove the build/install dirs for this OS/arch first
#
# Output goes to third_party/ffmpeg-<os>-<arch>/ (the install prefix), with
# sources fetched into third_party/ffmpeg-src/. Neither is committed to git.

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
# OS / arch detection
# ---------------------------------------------------------------------------
UNAME_S="$(uname -s)"
case "${UNAME_S}" in
    MINGW*|MSYS*|CYGWIN*)
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
    *)
        echo "build-ffmpeg.sh: unrecognized OS '${UNAME_S}'" >&2
        exit 1
        ;;
esac

ARCH_NAME="$(uname -m)"
case "${ARCH_NAME}" in
    x86_64|amd64) ARCH_NAME="x86_64" ;;
    aarch64|arm64) ARCH_NAME="arm64" ;;
esac

BUILD_DIR="${THIRD_PARTY}/ffmpeg-${OS_NAME}-${ARCH_NAME}-build"
PREFIX_DIR="${THIRD_PARTY}/ffmpeg-${OS_NAME}-${ARCH_NAME}"

if [[ "${1:-}" == "--clean" ]]; then
    echo "Removing ${BUILD_DIR} and ${PREFIX_DIR}"
    rm -rf "${BUILD_DIR}" "${PREFIX_DIR}"
    shift || true
fi

# sha256 of a file: sha256sum (Linux, MSYS2, macOS 15+) or shasum (older macOS).
sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
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

if [[ ! -f "${TARBALL_PATH}" ]]; then
    echo "Downloading ${FFMPEG_URL}"
    curl -fL --retry 3 -o "${TARBALL_PATH}.part" "${FFMPEG_URL}"
    mv "${TARBALL_PATH}.part" "${TARBALL_PATH}"
fi

echo "Verifying sha256"
computed_sha256="$(sha256_of "${TARBALL_PATH}")"
if [[ "${computed_sha256}" != "${FFMPEG_SHA256}" ]]; then
    echo "build-ffmpeg.sh: sha256 mismatch for ${TARBALL_PATH}" >&2
    echo "  expected: ${FFMPEG_SHA256}" >&2
    echo "  got:      ${computed_sha256}" >&2
    exit 1
fi

EXTRACT_DIR="${SRC_DIR}/ffmpeg-${FFMPEG_VERSION}"
if [[ ! -d "${EXTRACT_DIR}" ]]; then
    echo "Extracting ${FFMPEG_TARBALL}"
    tar -C "${SRC_DIR}" -xf "${TARBALL_PATH}"
fi

# ---------------------------------------------------------------------------
# Configure
# ---------------------------------------------------------------------------
mkdir -p "${BUILD_DIR}"
cd "${WORK_BUILD_DIR}"

# Components actually used by marvin-core:
#   encode:  FFV1 (analog capture) + PCM s16le
#   decode:  FFV1, DV (dvvideo), MPEG-2 video (HDV), PCM s16le/s16be, MP2 (HDV audio)
#   mux:     Matroska (FFV1+PCM sink), MOV, AVI, DV, MPEG-TS, NUT (analog to stdout)
#   demux:   DV, MPEG-TS, Matroska, MOV, AVI, NUT (rewrap sources / smoke test readback)
#   parse:   mpegvideo (MPEG-2), mpegaudio (MP2)
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
    --disable-avfilter
    --disable-iconv
    --disable-zlib
    --disable-bzlib
    --disable-lzma
    --disable-swscale
    --disable-swresample
    --disable-debug

    "${THREAD_FLAG}"

    --enable-encoder=ffv1
    --enable-encoder=pcm_s16le

    --enable-decoder=ffv1
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

    --enable-protocol=file

    --disable-bsfs
)

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

# scripts/build.ps1 and scripts/build.sh rebuild FFmpeg when this stamp no
# longer matches this script (the script holds the whole component list).
sha256_of "${SCRIPT_DIR}/build-ffmpeg.sh" > "${PREFIX_DIR}/build-script.sha256"

# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------
echo
echo "=== Installed static libs ==="
ls -la "${PREFIX_DIR}/lib"/*.a 2>/dev/null || true

echo
echo "=== pkg-config --static --libs libavformat ==="
PKG_CONFIG_PATH="${PREFIX_DIR}/lib/pkgconfig" pkg-config --static --libs libavformat
