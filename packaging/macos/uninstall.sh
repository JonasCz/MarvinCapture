#!/bin/bash
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

# uninstall.sh: removes what the MarvinCapture .pkg installed.
#
#   sudo /usr/local/lib/marvincapture/uninstall.sh
#   sudo /Applications/MarvinCapture.app/Contents/Resources/uninstall.sh
#
# Removes /Applications/MarvinCapture.app, /usr/local/bin/MarvinCaptureCLI and
# /usr/local/lib/marvincapture, then makes the Installer forget the packages.
# Your recordings and settings are left alone.
# (The installer puts a copy in both places, so whichever part was installed has one.)

set -euo pipefail

# Running from inside the app bundle that is about to be deleted is fine:
# bash has already read this short script.

[[ $EUID -eq 0 ]] || { echo "uninstall.sh: run with sudo" >&2; exit 1; }

rm -rf /Applications/MarvinCapture.app
# only remove the symlink if it is ours
if [[ -L /usr/local/bin/MarvinCaptureCLI ]]; then
    rm -f /usr/local/bin/MarvinCaptureCLI
fi
rm -rf /usr/local/lib/marvincapture

for id in jonascz.MarvinCapture.app jonascz.MarvinCapture.cli; do
    pkgutil --pkg-info "$id" >/dev/null 2>&1 && pkgutil --forget "$id" >/dev/null
done
echo "MarvinCapture removed."
