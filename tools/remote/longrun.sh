#!/bin/bash
# Pinnacle Studio 500-USB open driver
# Copyright (C) 2026 Jonas Cz.
#
# This program is free software: you can redistribute it and/or modify it
# under the terms of the GNU Affero General Public License as published by the
# Free Software Foundation, either version 3 of the License, or (at your
# option) any later version. This program is distributed WITHOUT ANY WARRANTY;
# see <https://www.gnu.org/licenses/> for the full licence.
# Launch a long pincli capture fully detached, so an ssh channel timeout
# cannot SIGHUP it. Prints the pid and returns immediately.
DUR=${1:-300}
OUT=${2:-/home/jonas/long.dv}
LOG=${3:-/home/jonas/long.log}
rm -f "$OUT" "$LOG"
setsid nohup /home/jonas/pinnacle-driver/build/pincli \
    -o "$OUT" \
    -b /home/jonas/pinnacle-traces/fpga-bitstream-candidate.bin \
    -t "$DUR" > "$LOG" 2>&1 < /dev/null &
echo "pid=$!  dur=${DUR}s  out=$OUT  log=$LOG"
