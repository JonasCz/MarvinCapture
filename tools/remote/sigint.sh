#!/bin/bash
# Verify the real Ctrl+C path: run without -t, send SIGINT, time the shutdown.
rm -f /home/jonas/sig.dv /home/jonas/sig.log
setsid nohup /home/jonas/pinnacle-driver/build/pincli \
    -o /home/jonas/sig.dv \
    -b /home/jonas/pinnacle-traces/fpga-bitstream-candidate.bin \
    > /home/jonas/sig.log 2>&1 < /dev/null &
PID=$!
echo "started pid=$PID, waiting 35s for bring-up + capture"
sleep 35
echo "sending SIGINT at $(date +%s.%N)"
T0=$(date +%s.%N)
kill -INT $PID
while kill -0 $PID 2>/dev/null; do sleep 0.1; done
T1=$(date +%s.%N)
echo "shutdown took $(echo "$T1 - $T0" | bc)s"
echo "--- log ---"
tr '\r' '\n' < /home/jonas/sig.log | grep -vE 'MB,' | tail -8
echo "--- file ---"
ls -la /home/jonas/sig.dv
python3 -c "
import os
n = os.path.getsize('/home/jonas/sig.dv')
print('bytes', n, 'seq', n/12000, 'frames', n/120000)
print('whole frames:', n % 120000 == 0)
"
