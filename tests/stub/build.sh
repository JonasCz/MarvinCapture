#!/usr/bin/env bash
# Thin wrapper around the MSYS2 UCRT64 cmake/ninja build for the
# pinnacle-oss-core stub DLL (tests/stub/**). Run this from an MSYS2
# UCRT64 shell, or from PowerShell/Bash via:
#
#   C:\msys64\usr\bin\bash.exe -lc "/c/Users/Jonas/Desktop/pinnacle-500-usb-open-driver/tests/stub/build.sh"
#
# Produces tests/stub/build-stub/pinnacle-oss-core.dll.
set -euo pipefail

export PATH="/ucrt64/bin:$PATH"
cd "$(dirname "${BASH_SOURCE[0]}")"

cmake -G Ninja -B build-stub
cmake --build build-stub

echo
echo "Built: $(pwd)/build-stub/pinnacle-oss-core.dll"
