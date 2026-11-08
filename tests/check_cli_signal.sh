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

# MarvinCaptureCLI stops and finalises its files on SIGTERM, SIGHUP and SIGQUIT (exit 130), also
# when stderr is a closed pipe (a lost terminal). Usage: check_cli_signal.sh CLI DATA_DIR
# The capture runs from the replay file for up to 30 s; the signal arrives after about 1 s.
set -u
cli="$1"; data="$2"
dir="$(mktemp -d)"; trap 'rm -rf "$dir"' EXIT
fail=0

run_with() {   # SIGNAL NAME [closed-stderr]
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

run_with TERM term
run_with HUP hup
run_with QUIT quit
run_with HUP hup_noterm closed
exit $fail
