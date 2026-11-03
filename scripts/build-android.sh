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

# scripts/build-android.sh
#
# Builds the Android app (gui/android) into one APK:
#
#   build/android/MarvinCapture-<version>-android.apk         (--release, the default)
#   build/android/MarvinCapture-<version>-android-debug.apk   (--debug)
#
#   1. scripts/build-android-native.sh --abi ... : FFmpeg, libusb, the core and the JNI layer
#      (libmarvin-core.so, libmarvin-jni.so in build/android-<abi>/dist)
#   2. Gradle (gui/android/gradlew, Gradle and AGP from the wrapper) packages those prebuilt
#      libraries, the FPGA bitstreams and the Java code; release = R8 + resource shrinking
#   3. copies the APK to build/android/, checks its signature and 16 KB alignment, and prints
#      its size per part
#
# Release signing comes from the environment (else the debug key, with a warning):
#   MARVIN_ANDROID_KEYSTORE           path of the keystore (.jks / .p12)
#   MARVIN_ANDROID_KEYSTORE_PASSWORD  its password
#   MARVIN_ANDROID_KEY_ALIAS          the key's alias
#   MARVIN_ANDROID_KEY_PASSWORD       the key's password (default: the keystore password)
#
# Requires what build-android-native.sh does (NDK 29.0.14206865, cmake, ninja, ...), a JDK 17
# or newer, and the Android SDK with platforms;android-36 and build-tools;36.0.0 (ANDROID_HOME;
# /opt/android-sdk is tried when it is unset). Non-interactive.
#
# Usage:
#   scripts/build-android.sh [--debug|--release] [--abi arm64-v8a|x86_64]... [--skip-native]
#
#   --abi ABI       package this ABI (repeatable; default arm64-v8a, the phones; x86_64 is
#                   for the emulator)
#   --skip-native   use the libraries already in build/android-<abi>/dist

set -euo pipefail

usage() { sed -n '/^# Usage:/,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

MODE=release
ABIS=()
SKIP_NATIVE=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --debug) MODE=debug; shift ;;
        --release) MODE=release; shift ;;
        --abi)
            [[ $# -ge 2 ]] || { echo "build-android.sh: --abi needs arm64-v8a or x86_64" >&2; exit 2; }
            ABIS+=("$2"); shift 2 ;;
        --abi=*) ABIS+=("${1#--abi=}"); shift ;;
        --skip-native) SKIP_NATIVE=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "build-android.sh: unknown option '$1'" >&2; usage >&2; exit 2 ;;
    esac
done
[[ ${#ABIS[@]} -gt 0 ]] || ABIS=(arm64-v8a)

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
step() { printf '\n\033[36m== %s\033[0m\n' "$*"; }
die()  { echo "build-android.sh: $*" >&2; exit 1; }

if [[ -z "${ANDROID_HOME:-}" ]]; then
    if [[ -n "${ANDROID_SDK_ROOT:-}" ]]; then export ANDROID_HOME="${ANDROID_SDK_ROOT}"
    elif [[ -d /opt/android-sdk ]]; then export ANDROID_HOME=/opt/android-sdk
    else die "set ANDROID_HOME to the Android SDK"; fi
fi
command -v java >/dev/null 2>&1 || die "no java on PATH (JDK 17 or newer)"
VERSION="$(sed -n 's/.*"version" *: *"\([^"]*\)".*/\1/p' "${ROOT}/VERSION" | head -n 1)"
[[ -n "${VERSION}" ]] || die "no version in VERSION"

# 1. native libraries
if [[ "${SKIP_NATIVE}" -eq 0 ]]; then
    args=()
    for abi in "${ABIS[@]}"; do args+=(--abi "${abi}"); done
    bash "${ROOT}/scripts/build-android-native.sh" "${args[@]}"
fi
for abi in "${ABIS[@]}"; do
    for lib in libmarvin-core.so libmarvin-jni.so; do
        [[ -f "${ROOT}/build/android-${abi}/dist/${lib}" ]] || die "build/android-${abi}/dist/${lib} missing"
    done
done

# 2. Gradle
step "Building the ${MODE} APK (${ABIS[*]})"
TASK="assemble${MODE^}"
ABI_LIST="$(IFS=,; echo "${ABIS[*]}")"
(cd "${ROOT}/gui/android" && ./gradlew --console=plain "${TASK}" "-PmarvinAbis=${ABI_LIST}")

# 3. the APK
SRC="${ROOT}/gui/android/app/build/outputs/apk/${MODE}/app-${MODE}.apk"
[[ -f "${SRC}" ]] || die "${SRC} not found"
OUT_DIR="${ROOT}/build/android"
mkdir -p "${OUT_DIR}"
SUFFIX=""
[[ "${MODE}" == debug ]] && SUFFIX="-debug"
APK="${OUT_DIR}/MarvinCapture-${VERSION}-android${SUFFIX}.apk"
cp -f "${SRC}" "${APK}"

BT="${ANDROID_HOME}/build-tools/36.0.0"
if [[ -x "${BT}/apksigner" ]]; then
    "${BT}/apksigner" verify --min-sdk-version 30 "${APK}" || die "apksigner: the APK does not verify"
    signer="$("${BT}/apksigner" verify --print-certs "${APK}" | sed -n 's/^Signer #1 certificate DN: //p')"
    echo "Signed by: ${signer}"
    if [[ "${MODE}" == release && "${signer}" == *"Android Debug"* ]]; then
        echo "warning: the release APK is signed with the debug key (MARVIN_ANDROID_KEYSTORE not set)" >&2
    fi
fi
if [[ -x "${BT}/zipalign" ]]; then
    "${BT}/zipalign" -c -P 16 4 "${APK}" >/dev/null || die "zipalign: native libraries not 16 KB aligned"
fi

step "Size of ${APK#"${ROOT}/"}"
python3 - "${APK}" <<'EOF'
import sys, zipfile, collections
z = zipfile.ZipFile(sys.argv[1])
parts = collections.OrderedDict()
for i in z.infolist():
    n = i.filename
    if n.startswith("lib/"): key = n
    elif n.startswith("assets/"): key = "assets/ (firmware)"
    elif n.endswith(".dex"): key = "classes*.dex (app code)"
    elif n == "resources.arsc" or n.startswith("res/"): key = "res/ + resources.arsc"
    elif n.startswith("META-INF/"): key = "META-INF/ (signature)"
    else: key = "other (manifest, ...)"
    s = parts.setdefault(key, [0, 0])
    s[0] += i.compress_size
    s[1] += i.file_size
for k, (c, u) in parts.items():
    print(f"  {k:44s} {c / 1e6:7.2f} MB in the APK ({u / 1e6:.2f} MB unpacked)")
EOF
size="$(wc -c < "${APK}" | tr -d '[:space:]')"
printf '\n\033[32mDone:\033[0m %s (%s bytes)\n' "${APK}" "${size}"
if command -v sha256sum >/dev/null 2>&1; then sha256sum "${APK}"; fi
