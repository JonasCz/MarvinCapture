#!/usr/bin/env python3
"""Check an analog capture's raw USB dump (pinanalog --raw FILE).

Record format: ep (1 byte), length (u32 LE), arrival time in us (u64 LE), data.

EP 0x82 video: each frame is a 12-byte header followed by the picture,
    ff 00 <frame# u16 LE> <device time u32 LE> 00 00 00 00
and ends with a short USB packet. EP 0x86 audio: one packet per transfer,
same header layout (with its own counter), then 16-bit LE stereo samples.

Reports truncated frames, frame/packet counter gaps, and how the device
clock, the video frame rate and the audio sample rate relate to each other.
"""

import argparse
import struct
import sys


def records(path):
    with open(path, "rb") as f:
        data = f.read()
    off = 0
    while off + 13 <= len(data):
        ep = data[off]
        n, us = struct.unpack_from("<IQ", data, off + 1)
        off += 13
        yield ep, us, data[off:off + n]
        off += n


def fit(xs, ys):
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    k = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx
    resid = max(abs(y - (my + k * (x - mx))) for x, y in zip(xs, ys))
    return k, resid


def unwrap(values, bits):
    out, base, prev = [], 0, None
    for v in values:
        if prev is not None and v < prev:
            base += 1 << bits
        out.append(base + v)
        prev = v
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("raw")
    ap.add_argument("--frame-bytes", type=int, default=720 * 576 * 2)
    args = ap.parse_args()

    frames = []      # (arrival us, frame#, device ts, payload bytes)
    audio = []       # (arrival us, packet#, device ts, sample count)
    cur = None
    bad = 0
    for ep, us, b in records(args.raw):
        if ep == 0x82:
            if cur is None or (len(b) >= 12 and b[0] == 0xFF and b[1] == 0 and cur[3] >= args.frame_bytes):
                if cur is not None:
                    frames.append(cur)
                if len(b) < 12 or b[0] != 0xFF or b[1] != 0:
                    bad += 1
                    cur = None
                    continue
                seq, ts = struct.unpack_from("<HI", b, 2)
                cur = [us, seq, ts, len(b) - 12]
            else:
                cur[3] += len(b)
            if len(b) % 512:
                frames.append(cur)
                cur = None
        elif ep == 0x86 and len(b) >= 12:
            seq, ts = struct.unpack_from("<HI", b, 2)
            audio.append((us, seq, ts, (len(b) - 12) // 4))

    ok = sum(1 for f in frames if f[3] == args.frame_bytes)
    print(f"video: {len(frames)} frames, {ok} complete, {len(frames) - ok} short/long, "
          f"{bad} transfers without a header where one was expected")
    short = [f for f in frames if f[3] != args.frame_bytes]
    for f in short[:10]:
        print(f"  frame {f[1]}: {f[3]} bytes")
    if len(frames) > 1:
        seqs = unwrap([f[1] for f in frames], 16)
        gaps = [(a, b) for a, b in zip(seqs, seqs[1:]) if b != a + 1]
        print(f"  frame counter gaps: {len(gaps)}" + (f" e.g. {gaps[:5]}" if gaps else ""))
    if audio:
        seqs = unwrap([a[1] for a in audio], 16)
        gaps = [(a, b) for a, b in zip(seqs, seqs[1:]) if b != a + 1]
        sizes = sorted(set(a[3] for a in audio))
        print(f"audio: {len(audio)} packets, samples/packet {sizes}, counter gaps: {len(gaps)}")

    if len(frames) > 10 and len(audio) > 10:
        fx = unwrap([f[1] for f in frames], 16)
        fts = unwrap([f[2] for f in frames], 32)
        kv, rv = fit(fx, fts)
        ax = unwrap([a[1] for a in audio], 16)
        ats = unwrap([a[2] for a in audio], 32)
        ka, ra = fit(ax, ats)
        # device clock against the host clock (arrival times; noisy but long)
        kh, _ = fit([f[0] for f in frames], fts)
        print(f"device ticks per video frame  {kv:.3f} (max residual {rv:.1f})")
        print(f"device ticks per audio packet {ka:.3f} (max residual {ra:.1f})")
        spp = audio[0][3]
        print(f"audio samples per video frame {spp * kv / ka:.4f}")
        dev_hz = kh * 1e6
        print(f"device clock vs host clock    {dev_hz:.1f} Hz  ->  "
              f"video {dev_hz / kv:.5f} fps, audio {dev_hz / ka * spp:.1f} Hz (host time)")
    return 0 if frames and ok == len(frames) and bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
