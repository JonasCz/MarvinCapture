#!/usr/bin/env python3
# Pinnacle Studio 500-USB open driver
# Copyright (C) 2026 Jonas Cz.
#
# This program is free software: you can redistribute it and/or modify it
# under the terms of the GNU Affero General Public License as published by the
# Free Software Foundation, either version 3 of the License, or (at your
# option) any later version. This program is distributed WITHOUT ANY WARRANTY;
# see <https://www.gnu.org/licenses/> for the full licence.
"""Analyse a PINNACLE_RAW_DUMP of an HDV capture: layer-1 address continuity,
layer-2 record integrity, TS CC errors -- each mapped back to a USB offset and
(if an ep88 log is given) an arrival time."""
# usage: python3 tools/hdvraw.py raw.bin [ep88.log]
#   raw.bin  from PINNACLE_RAW_DUMP=raw.bin, log from PINNACLE_DEBUG_EP88=1 (stderr)
import struct, sys, collections, re, bisect

raw = open(sys.argv[1], 'rb').read()
log = sys.argv[2] if len(sys.argv) > 2 else None

# completions -> cumulative offset / time
cum, times, sizes = [0], [], []
if log:
    off = 0
    for line in open(log, errors='replace'):
        m = re.match(r'ep88 t=([\d.]+)ms size=(\d+)', line)
        if m:
            times.append(float(m.group(1))); sizes.append(int(m.group(2)))
            off += int(m.group(2)); cum.append(off)

def where(usb_off):
    if not times:
        return f"usb@{usb_off}"
    k = bisect.bisect_right(cum, usb_off) - 1
    k = min(max(k, 0), len(times) - 1)
    return f"usb@{usb_off} (xfer#{k} t={times[k]:.1f}ms size={sizes[k]})"

# ---- layer 1
i = 0
ring = bytearray()
ring_usb = []          # (ring_off_start, usb_off_start, addr)
resync1 = 0
msgs = 0
lens = collections.Counter()
addr_events = []
exp_addr = None
last_addr_end = None
while i + 4 <= len(raw):
    w = struct.unpack_from('<I', raw, i)[0]
    if (w >> 28) == 9:
        n = (w >> 16) & 0x7FF
        addr = w & 0xFFFF
        if 0x7000 <= addr < 0x10000:
            if exp_addr is not None and addr != exp_addr:
                addr_events.append((i, addr, exp_addr, len(ring)))
            ring_usb.append((len(ring), i + 4, addr))
            ring += raw[i + 4:i + 4 + n]
            exp_addr = addr + n
            if exp_addr >= 0x10000:
                exp_addr = 0x7000 + (exp_addr - 0x10000)
            lens[n] += 1
        msgs += 1
        i += 4 + n
    else:
        i += 1
        resync1 += 1

print(f"raw {len(raw)} B, {msgs} msgs, layer1 resync bytes {resync1}, ring {len(ring)} B")
print("ring msg len histogram (top 8):", lens.most_common(8))
print(f"ring address discontinuities: {len(addr_events)}")
for (u, a, e, ro) in addr_events[:15]:
    print(f"   at {where(u)} ring_off={ro}: addr {a:#06x} expected {e:#06x} (skip {(a - e) & 0xFFFF})")

def ring_to_usb(ro):
    ks = [r[0] for r in ring_usb]
    k = bisect.bisect_right(ks, ro) - 1
    r0, u0, a0 = ring_usb[k]
    return u0 + (ro - r0)

# ---- layer 2
r = bytes(ring)
i = 0
resync2 = []
recs = 0
dbc_prev = None
dbc_events = []
sp_total = 0
ts = bytearray()
ts_origin = []   # ring offset of each ts packet
sizes_c = collections.Counter()
while i + 12 <= len(r):
    w = struct.unpack_from('<I', r, i)[0]
    dlen = w >> 16
    tc = (w >> 4) & 0xF
    ok = False
    if tc == 0xA and 8 < dlen <= 8 + 192 * 5 and (dlen - 8) % 192 == 0 and i + 8 + dlen + 4 <= len(r):
        if r[i + 5] == 6 and r[i + 6] == 0xC4 and (r[i + 8] & 0x3F) == 0x20:
            ok = True
    if not ok:
        if (w >> 16) == 0 and tc == 0xA:
            pass
        resync2.append(i)
        i += 4
        continue
    recs += 1
    dbc = r[i + 7]
    nsp = (dlen - 8) // 192
    sizes_c[nsp] += 1
    if dbc_prev is not None and dbc != dbc_prev:
        dbc_events.append((i, dbc_prev, dbc))
    dbc_prev = (dbc + 8 * nsp) & 0xFF
    for k in range(nsp):
        p = i + 4 + 8 + k * 192 + 4
        ts += r[p:p + 188]
        ts_origin.append(p)
    i += 4 + ((dlen + 3) & ~3) + 4

print(f"records {recs}, sp/record histogram {dict(sizes_c)}, layer2 resynced quadlets {len(resync2)}")
# group resyncs
groups = []
for q in resync2:
    if groups and q - groups[-1][1] <= 4:
        groups[-1][1] = q
    else:
        groups.append([q, q])
print(f"layer2 resync groups: {len(groups)}")
for a, b in groups[:15]:
    print(f"   ring_off={a}..{b + 4} ({b + 4 - a} B) {where(ring_to_usb(a))}")
print(f"DBC discontinuities: {len(dbc_events)}")
for (i, e, g) in dbc_events[:15]:
    print(f"   ring_off={i} expected {e} got {g} {where(ring_to_usb(i))}")

# TS CC
cc = {}
errs = []
n = len(ts) // 188
bad_sync = 0
for k in range(n):
    p = ts[k * 188:(k + 1) * 188]
    if p[0] != 0x47:
        bad_sync += 1
        errs.append((k, 'sync', 0)); continue
    pid = ((p[1] & 0x1F) << 8) | p[2]
    afc = (p[3] >> 4) & 3
    c = p[3] & 15
    if pid == 0x1FFF or not afc & 1:
        continue
    disc = afc & 2 and p[4] > 0 and p[5] & 0x80
    last = cc.get(pid)
    if last is not None and not disc and c != last and c != (last + 1) & 15:
        errs.append((k, pid, (c - last - 1) & 15))
    cc[pid] = c
print(f"{n} TS packets, bad sync {bad_sync}, CC errors {len(errs)}")
for (k, pid, miss) in errs[:20]:
    po = ts_origin[k]
    print(f"   ts#{k} pid={pid if pid == 'sync' else hex(pid)} missing~{miss} ring_off={po} {where(ring_to_usb(po))}")
