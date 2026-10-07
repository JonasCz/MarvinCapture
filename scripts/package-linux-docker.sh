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

# scripts/package-linux-docker.sh
#
# Builds the Linux .deb (scripts/package-linux.sh) inside an Ubuntu 22.04
# container, from macOS or any other host with Docker. The package's libc6
# dependency follows the glibc it was built against, so building on the oldest
# supported release (22.04, glibc 2.35) makes one .deb that installs on Ubuntu
# 22.04+ and Debian 12+.
#
# The repository is mounted at /src, so the output lands in the usual places:
#
#   build/linux-<arch>/installer/marvincapture_<version>_<debarch>.deb
#   third_party/ffmpeg-linux-<arch>/   (built once per arch, then reused)
#
# On Apple Silicon, arm64 runs natively and amd64 under emulation (slower,
# mostly the first FFmpeg build).
#
# Usage:
#   scripts/package-linux-docker.sh [--arch amd64|arm64|all] [--image IMAGE] [--skip-tests]
#
#   --arch amd64|arm64|all  target architecture(s) (default amd64)
#   --image IMAGE           base image (default ubuntu:22.04)
#   --skip-tests            don't run ctest

set -euo pipefail

usage() { sed -n '/^# Usage:/,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

ARCHES=(amd64)
IMAGE="ubuntu:22.04"
BUILD_ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch)
            [[ $# -ge 2 ]] || { echo "package-linux-docker.sh: --arch needs amd64, arm64 or all" >&2; exit 2; }
            case "$2" in
                amd64|arm64) ARCHES=("$2") ;;
                all) ARCHES=(amd64 arm64) ;;
                *) echo "package-linux-docker.sh: --arch must be amd64, arm64 or all, not '$2'" >&2; exit 2 ;;
            esac
            shift 2 ;;
        --image)
            [[ $# -ge 2 ]] || { echo "package-linux-docker.sh: --image needs a name" >&2; exit 2; }
            IMAGE="$2"; shift 2 ;;
        --skip-tests) BUILD_ARGS+=(--skip-tests); shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "package-linux-docker.sh: unknown option '$1'" >&2; usage >&2; exit 2 ;;
    esac
done

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

step() { printf '\n\033[36m== %s\033[0m\n' "$*"; }
die()  { echo "package-linux-docker.sh: $*" >&2; exit 1; }

command -v docker >/dev/null 2>&1 || die "'docker' not found (Docker Desktop, OrbStack or colima)"
docker info >/dev/null 2>&1 || die "the Docker daemon is not running"

# Runs as root in the container, then hands the files it created back to the
# calling user so build/ and third_party/ stay writable on the host.
INNER='
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
    build-essential cmake ninja-build pkg-config libusb-1.0-0-dev nasm git \
    binutils dpkg-dev file curl xz-utils ca-certificates diffutils >/dev/null
git config --global --add safe.directory /src
status=0
scripts/package-linux.sh "$@" || status=$?
chown -R "${HOST_UID}:${HOST_GID}" build third_party 2>/dev/null || true
exit $status
'

for arch in "${ARCHES[@]}"; do
    step "Building the .deb for ${arch} in ${IMAGE}"
    docker run --rm --platform "linux/${arch}" \
        -v "${ROOT}:/src" -w /src \
        -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
        "${IMAGE}" bash -c "${INNER}" inner ${BUILD_ARGS[@]+"${BUILD_ARGS[@]}"}
done

step "Packages"
ls -l "${ROOT}"/build/linux-*/installer/*.deb
