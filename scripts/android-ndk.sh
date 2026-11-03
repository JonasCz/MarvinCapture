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

# scripts/android-ndk.sh -- sourced, not run: the Android NDK settings shared by
# scripts/build-android-native.sh and scripts/build-ffmpeg.sh --android.
#
# The NDK version is pinned: it is the one GitHub's ubuntu-24.04 runner has
# preinstalled (under $ANDROID_HOME/ndk/), so CI needs no download. API level 30
# (Android 11) is the app's minSdk.
#
# Defines PIN_NDK_VERSION, PIN_ANDROID_API and:
#   pin_find_ndk           sets PIN_NDK (the NDK root) or fails
#   pin_android_abi ABI    sets PIN_ABI_TRIPLE (clang target, e.g. aarch64-linux-android),
#                          PIN_ABI_FFARCH (FFmpeg --arch) and PIN_ABI_DIR (the arch part of
#                          third_party/*-android-<arch>: arm64 or x86_64)
#   pin_ndk_tool NAME      prints the path of an NDK LLVM tool (clang, llvm-ar, ...)

PIN_NDK_VERSION="29.0.14206865"
PIN_ANDROID_API=30

pin_ndk_version_of() {
    sed -n 's/^Pkg\.Revision *= *//p' "$1/source.properties" 2>/dev/null | tr -d '[:space:]'
}

# Order: $ANDROID_NDK_HOME if it is the pinned version, else the pinned version under
# $ANDROID_HOME / $ANDROID_SDK_ROOT / /opt/android-sdk, else $ANDROID_NDK_HOME of another
# version (with a warning). The runner's ANDROID_NDK_HOME points at its default NDK, which
# is not necessarily this one.
pin_find_ndk() {
    local cand
    for cand in "${ANDROID_NDK_HOME:-}" \
                "${ANDROID_HOME:+${ANDROID_HOME}/ndk/${PIN_NDK_VERSION}}" \
                "${ANDROID_SDK_ROOT:+${ANDROID_SDK_ROOT}/ndk/${PIN_NDK_VERSION}}" \
                "/opt/android-sdk/ndk/${PIN_NDK_VERSION}"; do
        [[ -n "${cand}" && -f "${cand}/source.properties" ]] || continue
        if [[ "$(pin_ndk_version_of "${cand}")" == "${PIN_NDK_VERSION}" ]]; then
            PIN_NDK="${cand}"
            return 0
        fi
    done
    if [[ -n "${ANDROID_NDK_HOME:-}" && -f "${ANDROID_NDK_HOME}/source.properties" ]]; then
        echo "warning: using the NDK in ANDROID_NDK_HOME ($(pin_ndk_version_of "${ANDROID_NDK_HOME}")," \
             "not the pinned ${PIN_NDK_VERSION})" >&2
        PIN_NDK="${ANDROID_NDK_HOME}"
        return 0
    fi
    echo "Android NDK ${PIN_NDK_VERSION} not found: set ANDROID_NDK_HOME, or ANDROID_HOME to an" \
         "SDK that has ndk/${PIN_NDK_VERSION} (sdkmanager \"ndk;${PIN_NDK_VERSION}\")" >&2
    return 1
}

pin_android_abi() {
    case "$1" in
        arm64-v8a) PIN_ABI_TRIPLE="aarch64-linux-android"; PIN_ABI_FFARCH="aarch64"; PIN_ABI_DIR="arm64" ;;
        x86_64)    PIN_ABI_TRIPLE="x86_64-linux-android";  PIN_ABI_FFARCH="x86_64";  PIN_ABI_DIR="x86_64" ;;
        *) echo "unsupported Android ABI '$1' (arm64-v8a or x86_64)" >&2; return 1 ;;
    esac
}

pin_ndk_tool() {
    local host
    case "$(uname -s)" in
        Linux)  host="linux-x86_64" ;;
        Darwin) host="darwin-x86_64" ;;   # the NDK ships universal binaries under this name
        *) echo "unsupported build host $(uname -s)" >&2; return 1 ;;
    esac
    printf '%s/toolchains/llvm/prebuilt/%s/bin/%s\n' "${PIN_NDK}" "${host}" "$1"
}
