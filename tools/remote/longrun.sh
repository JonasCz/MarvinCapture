#!/bin/bash
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
