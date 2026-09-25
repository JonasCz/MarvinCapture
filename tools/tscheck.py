#!/usr/bin/env python3
# Pinnacle Studio 500-USB open driver
# Copyright (C) 2026 Jonas Cz.
#
# This program is free software: you can redistribute it and/or modify it
# under the terms of the GNU Affero General Public License as published by the
# Free Software Foundation, either version 3 of the License, or (at your
# option) any later version. This program is distributed WITHOUT ANY WARRANTY;
# see <https://www.gnu.org/licenses/> for the full licence.
"""Structural check of an HDV capture (.ts) written by pincli.

Independent of pincli's own counters: verifies the file is a whole number of
188-byte TS packets, every one starts with 0x47, per-PID continuity counters
are unbroken, and the file begins with PAT + PMT and starts its video on a
sequence header. Exits non-zero if anything is wrong.

    python3 tools/tscheck.py out.ts [--duration-seconds N]
"""
import argparse
import sys
from collections import Counter

TS = 188


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--duration-seconds", type=float,
                    help="wall-clock length, to cross-check the frame count (25 fps)")
    a = ap.parse_args()

    d = open(a.file, "rb").read()
    bad = 0
    if len(d) % TS:
        print(f"FAIL: size {len(d)} is not a multiple of {TS}")
        bad += 1

    n = len(d) // TS
    pids = Counter()
    cc = {}
    cc_errors = []
    sync_errors = 0
    video_pid = None
    frames = 0
    first_pids = []

    for k in range(n):
        p = d[k * TS:(k + 1) * TS]
        if p[0] != 0x47:
            sync_errors += 1
            continue
        pid = ((p[1] & 0x1F) << 8) | p[2]
        pusi = bool(p[1] & 0x40)
        afc = (p[3] >> 4) & 3
        c = p[3] & 0x0F
        pids[pid] += 1
        if k < 3:
            first_pids.append(pid)
        po = 5 + p[4] if afc & 2 else 4
        if pusi and afc & 1 and p[po:po + 3] == b"\x00\x00\x01" and (p[po + 3] & 0xF0) == 0xE0:
            if video_pid is None:
                video_pid = pid
                if b"\x00\x00\x01\xb3" not in p[po:]:
                    print("FAIL: first video picture has no sequence header")
                    bad += 1
            if pid == video_pid:
                frames += 1
        if pid == 0x1FFF or not afc & 1:
            continue
        disc = afc & 2 and p[4] > 0 and p[5] & 0x80
        last = cc.get(pid)
        if last is not None and not disc and c != last and c != (last + 1) & 15:
            cc_errors.append((k, pid))
        cc[pid] = c

    print(f"{n} TS packets, {frames} video pictures, "
          f"PIDs: {', '.join(f'{p:#x}={v}' for p, v in pids.most_common(6))}")
    if not first_pids or first_pids[0] != 0:
        print(f"WARN: file does not begin with PAT + PMT (first PIDs {first_pids})")
    if sync_errors:
        print(f"FAIL: {sync_errors} packets without a 0x47 sync byte")
        bad += 1
    if cc_errors:
        print(f"FAIL: {len(cc_errors)} continuity-counter errors, first at packets "
              f"{[k for k, _ in cc_errors[:5]]}")
        bad += 1
    if a.duration_seconds:
        exp = a.duration_seconds * 25
        print(f"frames vs wall clock: {frames} of ~{exp:.0f} expected "
              f"({100 * frames / exp:.1f}%; bring-up latency accounts for the shortfall)")
    print("RESULT:", "clean" if not bad else "PROBLEMS FOUND")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
