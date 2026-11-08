#!/bin/sh
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

# MarvinCaptureCLI stops and finalises its files on SIGINT, SIGTERM, SIGHUP and SIGQUIT (exit 130),
# also when stderr is a closed pipe (a lost terminal). SIGTERM / SIGHUP / SIGQUIT are idempotent:
# a burst of them (a closing terminal sends several) still finalises. A second SIGINT quits at once.
# A command line with settings but no action is a usage error (exit 1).
# Usage: check_cli_signal.sh CLI DATA_DIR
# The capture runs from the replay file for up to 30 s; the signal arrives after about 1 s.
set -u
cli="$1"; data="$2"
dir="$(mktemp -d)"; trap 'rm -rf "$dir"' EXIT
fail=0

run_with() {   # SIGNAL NAME [closed-stderr | twice]
    sig="$1"; name="$2"; out="$dir/$name.dv"
    if [ "${3:-}" = closed ]; then
        # stderr is a pipe whose reader is gone: every status write fails with EPIPE
        ( "$cli" -d "$data/dv-pal.dv" --capture "$out" --wait wallclock=00:00:30 2>&1 >/dev/null | true ) &
        sleep 1.5
        pid="$(pgrep -n -f "capture $out")"
    else
        "$cli" -d "$data/dv-pal.dv" --capture "$out" --wait wallclock=00:00:30 >"$dir/$name.log" 2>&1 &
        pid=$!
        sleep 1.5
    fi
    kill "-$sig" "$pid"
    if [ "${3:-}" = twice ]; then
        kill "-$sig" "$pid" 2>/dev/null
        sleep 0.05
        kill -HUP "$pid" 2>/dev/null
    fi
    n=0
    while kill -0 "$pid" 2>/dev/null && [ "$n" -lt 100 ]; do sleep 0.1; n=$((n + 1)); done
    if kill -0 "$pid" 2>/dev/null; then
        echo "FAIL: $name: still running 10 s after SIG$sig"; kill -KILL "$pid"; fail=1; return
    fi
    if [ "${3:-}" != closed ]; then
        wait "$pid"; rc=$?
        [ "$rc" -eq 130 ] || { echo "FAIL: $name: exit $rc, want 130"; cat "$dir/$name.log"; fail=1; }
    fi
    size="$(wc -c <"$out" 2>/dev/null || echo 0)"
    # whole DV frames only: a finalised file is a multiple of the frame size (144000 bytes, PAL)
    if [ "$size" -lt 144000 ] || [ $((size % 144000)) -ne 0 ]; then
        echo "FAIL: $name: file is $size bytes (want a whole number of 144000-byte frames, at least one)"
        fail=1
    else
        echo "OK: SIG$sig ($name): $size bytes"
    fi
}

run_with INT int
run_with TERM term
run_with TERM term_burst twice
run_with HUP hup_burst twice
run_with QUIT quit_burst twice
run_with HUP hup
run_with QUIT quit
run_with HUP hup_noterm closed
# a second Ctrl+C gives up on the orderly stop: the process ends at once with 130
"$cli" -d "$data/dv-pal.dv" --capture "$dir/int2.dv" --wait wallclock=00:00:30 >"$dir/int2.log" 2>&1 &
pid=$!
sleep 1.5
kill -INT "$pid"; kill -INT "$pid" 2>/dev/null
n=0
while kill -0 "$pid" 2>/dev/null && [ "$n" -lt 50 ]; do sleep 0.1; n=$((n + 1)); done
if kill -0 "$pid" 2>/dev/null; then
    echo "FAIL: int2: still running 5 s after two SIGINT"; kill -KILL "$pid"; fail=1
else
    wait "$pid"; rc=$?
    # either the clean stop was quicker than the second signal (130) or the second one forced it (130)
    [ "$rc" -eq 130 ] && echo "OK: two SIGINT: exit 130" || { echo "FAIL: int2: exit $rc, want 130"; fail=1; }
fi

# settings but no action: a usage error, not "Nothing to do" with exit 0
"$cli" -i svideo --std pal >"$dir/noaction.log" 2>&1
rc=$?
if [ "$rc" -eq 1 ]; then
    echo "OK: settings without an action: exit 1"
else
    echo "FAIL: settings without an action: exit $rc, want 1"; cat "$dir/noaction.log"; fail=1
fi
exit $fail
