#!/usr/bin/env python3
# Pinnacle Studio 500-USB open driver
# Copyright (C) 2026 Jonas Cz.
#
# This program is free software: you can redistribute it and/or modify it
# under the terms of the GNU Affero General Public License as published by the
# Free Software Foundation, either version 3 of the License, or (at your
# option) any later version. This program is distributed WITHOUT ANY WARRANTY;
# see <https://www.gnu.org/licenses/> for the full licence.

"""
Structural integrity check for a raw DV (.dv) file produced by pincli.

Answers "did we drop anything?" three independent ways:

  1. Structure     — every frame is a whole number of 12,000-byte DIF
                     sequences, block IDs (SCT / Dseq / DBN) are where the
                     DV spec says they should be, and no sequence is the
                     all-zero placeholder the reassembler writes for a gap.
  2. Timecode      — the SMPTE timecode pack (0x13) in the subcode DIF
                     blocks should advance by exactly one frame each frame.
                     Camcorders only emit a running timecode off tape; in
                     live/E-E mode many emit a frozen or absent value, so
                     this check reports "not usable" rather than failing.
  3. Frame count   — total frames vs. wall-clock duration, if given.

Usage:
    python dvcheck.py capture.dv [--duration-seconds N] [--max-report N]
"""

import argparse
import sys

SEQ_BYTES = 150 * 80
BLOCK = 80
# DIF block section types, by the top nibble of ID0.
SCT_HEADER, SCT_SUBCODE, SCT_VAUX, SCT_AUDIO, SCT_VIDEO = 0, 1, 2, 3, 4

# Expected section type for each of the 150 blocks in a DIF sequence:
# 1 header, 2 subcode, 3 VAUX, then 9 x (1 audio + 15 video).
EXPECTED_SCT = ([SCT_HEADER] + [SCT_SUBCODE] * 2 + [SCT_VAUX] * 3 +
                ([SCT_AUDIO] + [SCT_VIDEO] * 15) * 9)
assert len(EXPECTED_SCT) == 150

SCT_FROM_ID0 = {0x1: SCT_HEADER, 0x3: SCT_SUBCODE, 0x5: SCT_VAUX,
                0x7: SCT_AUDIO, 0x9: SCT_VIDEO}


def bcd(byte):
    return (byte >> 4) * 10 + (byte & 0x0F)


def iter_packs(seq):
    """Yields every 5-byte pack in one DIF sequence, as (area, pack_bytes).

    Subcode blocks (1, 2): 3 ID bytes, then 6 sync blocks of 8 bytes. Each
    sync block is 3 bytes of SSYB ID followed by a 5-byte pack, so the pack
    sits at offset 3 + i*8 + 3.

    VAUX blocks (3, 4, 5): 3 ID bytes, then 15 consecutive 5-byte packs.
    """
    for blk in (1, 2):
        b = seq[blk * BLOCK:(blk + 1) * BLOCK]
        for i in range(6):
            p = 6 + i * 8
            if p + 5 <= len(b):
                yield "subcode", b[p:p + 5]
    for blk in (3, 4, 5):
        b = seq[blk * BLOCK:(blk + 1) * BLOCK]
        for i in range(15):
            p = 3 + i * 5
            if p + 5 <= len(b):
                yield "vaux", b[p:p + 5]


def timecode_from_sequence(seq):
    """SMPTE timecode (pack 0x13, TITLE TIMECODE) from one DIF sequence.

    Pack payload: d0 = frames, d1 = seconds, d2 = minutes, d3 = hours, all
    BCD with flag bits in the high positions that must be masked off.
    Returns (h, m, s, f) or None. 0xFF bytes mean "no information".
    """
    for _area, pack in iter_packs(seq):
        if pack[0] != 0x13:
            continue
        if all(x == 0xFF for x in pack[1:]):
            continue
        f = bcd(pack[1] & 0x3F)
        s = bcd(pack[2] & 0x7F)
        m = bcd(pack[3] & 0x7F)
        h = bcd(pack[4] & 0x3F)
        if f < 60 and s < 60 and m < 60 and h < 24:
            return (h, m, s, f)
    return None


def tc_to_frames(tc, fps):
    h, m, s, f = tc
    return ((h * 60 + m) * 60 + s) * fps + f


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--duration-seconds", type=float, default=None)
    ap.add_argument("--max-report", type=int, default=10)
    ap.add_argument("--dump-packs", action="store_true",
                    help="list which metadata pack IDs the camera actually "
                         "emits, to see what is available in this mode")
    args = ap.parse_args()

    data = open(args.path, "rb").read()
    if len(data) % SEQ_BYTES:
        print(f"FAIL  file is {len(data)} bytes, not a whole number of "
              f"{SEQ_BYTES}-byte DIF sequences "
              f"(remainder {len(data) % SEQ_BYTES})")
        return 1

    nseq = len(data) // SEQ_BYTES
    # Determine the system from the highest Dseq present in the first frames.
    max_dseq = 0
    for i in range(min(nseq, 24)):
        off = i * SEQ_BYTES
        if data[off] == 0x1F and (data[off + 1] & 0x0F) == 0x07:
            max_dseq = max(max_dseq, data[off + 1] >> 4)
    seq_per_frame = 12 if max_dseq >= 10 else 10
    system = "PAL" if seq_per_frame == 12 else "NTSC"
    fps = 25 if seq_per_frame == 12 else 30

    print(f"file          {args.path}")
    print(f"size          {len(data)} bytes, {nseq} DIF sequences")
    print(f"system        {system} ({seq_per_frame} sequences/frame, "
          f"{nseq / seq_per_frame:.2f} frames)")

    if nseq % seq_per_frame:
        print(f"WARN  trailing partial frame ({nseq % seq_per_frame} "
              f"sequences over)")

    zero_seqs, bad_dseq, bad_sct, bad_dbn = [], [], [], []
    for i in range(nseq):
        seq = data[i * SEQ_BYTES:(i + 1) * SEQ_BYTES]
        if seq[0] == 0 and not any(seq[:BLOCK]):
            zero_seqs.append(i)
            continue
        want_dseq = i % seq_per_frame
        got_dseq = seq[1] >> 4
        if seq[0] != 0x1F or (seq[1] & 0x0F) != 0x07 or got_dseq != want_dseq:
            bad_dseq.append((i, want_dseq, seq[0], seq[1]))
            continue
        for b in range(150):
            id0, id2 = seq[b * BLOCK], seq[b * BLOCK + 2]
            sct = SCT_FROM_ID0.get(id0 >> 4)
            if sct is None or sct != EXPECTED_SCT[b]:
                bad_sct.append((i, b, id0))
            elif EXPECTED_SCT[b] == SCT_VIDEO or EXPECTED_SCT[b] == SCT_AUDIO:
                # DBN counts within its own section type across the sequence.
                n = (b - 6) // 16
                want = n if EXPECTED_SCT[b] == SCT_AUDIO else (b - 6) - n - 1
                if id2 != want:
                    bad_dbn.append((i, b, id2, want))

    print()
    print("--- structure ---")
    print(f"zero-filled (dropped) sequences   {len(zero_seqs)}")
    print(f"bad sequence headers / Dseq       {len(bad_dseq)}")
    print(f"bad block section types           {len(bad_sct)}")
    print(f"bad block DBNs                    {len(bad_dbn)}")
    for label, lst in (("zero seq", zero_seqs), ("bad dseq", bad_dseq),
                       ("bad sct", bad_sct), ("bad dbn", bad_dbn)):
        for item in lst[:args.max_report]:
            print(f"  {label}: {item}")
        if len(lst) > args.max_report:
            print(f"  {label}: ... and {len(lst) - args.max_report} more")

    if args.dump_packs:
        print()
        print("--- packs present (first 200 sequences) ---")
        seen = {}
        for i in range(min(nseq, 200)):
            seq = data[i * SEQ_BYTES:(i + 1) * SEQ_BYTES]
            for area, pack in iter_packs(seq):
                if pack[0] == 0xFF:
                    continue
                key = (area, pack[0])
                seen.setdefault(key, [0, set()])
                seen[key][0] += 1
                if len(seen[key][1]) < 4:
                    seen[key][1].add(pack[1:].hex())
        for (area, pid), (count, samples) in sorted(seen.items()):
            print(f"  {area:8s} pack 0x{pid:02x}  x{count:<6d} "
                  f"samples: {', '.join(sorted(samples))}")

    print()
    print("--- timecode ---")
    frames = nseq // seq_per_frame
    tcs = []
    for fi in range(frames):
        seq0 = data[fi * seq_per_frame * SEQ_BYTES:
                    (fi * seq_per_frame + 1) * SEQ_BYTES]
        tcs.append(timecode_from_sequence(seq0))

    present = [t for t in tcs if t is not None]
    if not present:
        print("no SMPTE timecode pack (0x13) found — the camera is not "
              "emitting one in this mode")
    elif len(set(present)) == 1:
        print(f"timecode present but frozen at "
              f"{present[0][0]:02d}:{present[0][1]:02d}:"
              f"{present[0][2]:02d}:{present[0][3]:02d} "
              f"({len(present)}/{frames} frames) — typical for live/E-E "
              f"output; not usable to detect drops")
    else:
        jumps, missing = [], 0
        prev = None
        for fi, t in enumerate(tcs):
            if t is None:
                missing += 1
                prev = None
                continue
            if prev is not None:
                d = tc_to_frames(t, fps) - tc_to_frames(prev, fps)
                if d != 1:
                    jumps.append((fi, prev, t, d))
            prev = t
        first = next(t for t in tcs if t)
        last = next(t for t in reversed(tcs) if t)
        print(f"range   {first[0]:02d}:{first[1]:02d}:{first[2]:02d}:"
              f"{first[3]:02d} -> {last[0]:02d}:{last[1]:02d}:"
              f"{last[2]:02d}:{last[3]:02d}")
        print(f"frames without a timecode  {missing}")
        print(f"non-consecutive steps      {len(jumps)}")
        for j in jumps[:args.max_report]:
            print(f"  frame {j[0]}: {j[1]} -> {j[2]} (delta {j[3]})")
        if len(jumps) > args.max_report:
            print(f"  ... and {len(jumps) - args.max_report} more")

    if args.duration_seconds:
        print()
        print("--- rate ---")
        expected = args.duration_seconds * (30000 / 1001 if fps == 30 else 25)
        print(f"captured {frames} frames in {args.duration_seconds:.1f}s; "
              f"expected ~{expected:.0f} "
              f"({100.0 * frames / expected:.2f}%)")

    bad = len(zero_seqs) + len(bad_dseq) + len(bad_sct) + len(bad_dbn)
    print()
    print("RESULT:", "clean" if bad == 0 else f"{bad} structural problems")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
