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

# scripts/install-deps.sh
#
# Installs what scripts/build.sh and the package scripts need, on Debian/Ubuntu
# (apt, with sudo unless root) or macOS (Homebrew; the Xcode Command Line Tools
# must already be installed). Installs only what is missing. The Windows
# counterpart is scripts/install-deps.ps1. See docs/building.md.
#
# Usage:
#   scripts/install-deps.sh [--tests]
#
#   --tests   also the ffmpeg command-line tool: the ctest suite makes its test recordings
#             with it (tests/data/README.md); without it those tests skip

set -euo pipefail

die() { echo "install-deps.sh: $*" >&2; exit 1; }

tests=0
for arg in "$@"; do
    case "${arg}" in
        --tests) tests=1 ;;
        *) die "unknown option '${arg}' (usage: scripts/install-deps.sh [--tests])" ;;
    esac
done

case "$(uname -s)" in
    Linux)
        command -v apt-get >/dev/null 2>&1 || die "no apt-get: install the equivalents of the Debian packages listed in this script"
        # build.sh: compiler, make, cmake, ninja, pkg-config, libusb, nasm (x86 FFmpeg asm);
        # package-linux.sh: dpkg-deb, objdump/strip (binutils, in build-essential)
        # package:check (a command, or "pc:<module>" for a pkg-config module); a tool that is
        # already on PATH from elsewhere (e.g. a newer cmake) is left alone
        pkgs=(build-essential:cc build-essential:make cmake:cmake ninja-build:ninja pkg-config:pkg-config
              libusb-1.0-0-dev:pc:libusb-1.0 nasm:nasm dpkg-dev:dpkg-architecture binutils:objdump
              curl:curl xz-utils:xz)
        [[ ${tests} -eq 0 ]] || pkgs+=(ffmpeg:ffmpeg)
        missing=()
        for entry in "${pkgs[@]}"; do
            p="${entry%%:*}"; check="${entry#*:}"
            if [[ "${check}" == pc:* ]]; then
                pkg-config --exists "${check#pc:}" 2>/dev/null && continue
            else
                command -v "${check}" >/dev/null 2>&1 && continue
            fi
            [[ " ${missing[*]} " == *" ${p} "* ]] || missing+=("${p}")
        done
        if [[ ${#missing[@]} -eq 0 ]]; then
            echo "All packages installed."
            exit 0
        fi
        SUDO=""
        [[ "$(id -u)" -eq 0 ]] || SUDO="sudo"
        ${SUDO} apt-get update -qq
        DEBIAN_FRONTEND=noninteractive ${SUDO} apt-get install -y --no-install-recommends "${missing[@]}"
        ;;
    Darwin)
        command -v brew >/dev/null 2>&1 || die "Homebrew not found (https://brew.sh)"
        command -v swift >/dev/null 2>&1 || die "swift not found: xcode-select --install"
        export HOMEBREW_NO_AUTO_UPDATE="${HOMEBREW_NO_AUTO_UPDATE:-1}"
        export HOMEBREW_NO_INSTALL_CLEANUP=1
        export HOMEBREW_NO_INSTALLED_DEPENDENTS_CHECK=1
        # nasm is only used for x86_64 FFmpeg builds; ffmpeg (the command-line tool) for the tests
        formulae=(cmake ninja nasm libusb pkgconf)
        [[ ${tests} -eq 0 ]] || formulae+=(ffmpeg)
        for f in "${formulae[@]}"; do
            brew list --formula "${f}" >/dev/null 2>&1 || brew install --formula "${f}"
        done
        ;;
    *) die "unsupported OS '$(uname -s)' (Windows: scripts\\install-deps.ps1)" ;;
esac
echo "Done."
