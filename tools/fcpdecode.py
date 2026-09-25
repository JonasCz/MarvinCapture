#!/usr/bin/env python3
# Pinnacle Studio 500-USB open driver
# Copyright (C) 2026 Jonas Cz.
#
# This program is free software: you can redistribute it and/or modify it under
# the terms of the GNU Affero General Public License as published by the Free
# Software Foundation, either version 3 of the License, or (at your option) any
# later version. This program is distributed WITHOUT ANY WARRANTY; see
# <https://www.gnu.org/licenses/> for the full licence.
"""Decode 1394 async traffic (incl. FCP / AV/C) from a usbmon trace of the
Pinnacle 500-USB, or from `ep84`/`ep02` hex lines on stdin.

    tshark -r trace.pcapng -Y 'usb.device_address==2 && usb.capdata' \
        -T fields -e frame.time_relative -e usb.endpoint_address -e usb.capdata \
        | python3 tools/fcpdecode.py

Input lines: <time> <endpoint> <hex payload>. EP 0x02 carries our AT
descriptors (type-8 writes into device RAM 0x11c0..0x1237); EP 0x84 carries
AR packets as type-9 messages (RAM 0x3000.. = AR request buffer,
0x5000.. = AR response buffer). See docs/deck-control.md.
"""
import struct, sys

TCODE = {0: 'wrq', 1: 'wrb', 2: 'wrresp', 4: 'rdq', 5: 'rdb', 6: 'rdqresp',
         7: 'rdbresp', 9: 'lock', 0xb: 'lockresp', 0xe: 'phy/bus-reset'}
EVT = {0x11: 'ack_complete', 0x12: 'ack_pending', 0x14: 'ack_busy_X',
       0x15: 'ack_busy_A', 0x16: 'ack_busy_B', 0x1b: 'ack_tardy',
       0x1d: 'ack_data_error', 0x1e: 'ack_type_error', 0x0a: 'evt_timeout',
       0x03: 'evt_missing_ack', 0x0e: 'evt_flushed', 0x10: 'evt_bus_reset'}

def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]

def avc(data):
    return data.hex(' ')

def hdr1394(q, data):
    tc = (q[0] >> 4) & 0xf
    tl = (q[0] >> 10) & 0x3f
    s = f"{TCODE.get(tc, 'tc%x' % tc)} tl={tl}"
    if tc in (0, 1, 4, 5, 9):
        off = ((q[1] & 0xffff) << 32) | q[2]
        s += f" {q[1] >> 16:04x}->{q[0] >> 16:04x} off={off:012x}" if len(q) > 1 else ''
    elif tc in (2, 6, 7, 0xb):
        s += f" {q[1] >> 16:04x}->{q[0] >> 16:04x} rcode={(q[1] >> 12) & 0xf}"
    return s, tc

def dec_out(p):
    out, o = [], 0
    while o + 4 <= len(p):
        h = u32(p, o); t = h >> 28
        if t in (2, 3, 4):
            v = u32(p, o + 4) if o + 8 <= len(p) else 0
            out.append(f"T{t} {h & 0xfffff:05x}={v:08x}"); o += 8
        elif t == 8:
            n = (h >> 16) & 0x7ff; a = h & 0xffff; d = p[o + 4:o + 4 + n]
            q = [u32(d, k) for k in range(0, len(d) - 3, 4)]
            if a in range(0x11c0, 0x1240) and n >= 32 and (q[0] >> 28) in (0, 1):
                s, tc = hdr1394(q[4:8], None)
                if (q[0] >> 24) == 0x12 and tc == 0:        # OUTPUT_LAST-Imm quadlet write
                    s += ' data=' + d[28:32].hex(' ')
                elif (q[0] >> 24) == 0x02 and len(d) > 48:  # MORE-Imm + LAST
                    s += f" len={q[7] >> 16} data=" + d[48:48 + (q[7] >> 16)].hex(' ')
                out.append(f"AT@{a:04x} {s}")
            else:
                out.append(f"W{a:04x}[{n}]")
            o += 4 + n
        else:
            out.append('?' + p[o:].hex()); break
    return out

def dec_in(p):
    out, o = [], 0
    while o + 4 <= len(p):
        h = u32(p, o); t = h >> 28
        if t == 9:
            n = (h >> 16) & 0x7ff; a = h & 0xffff; d = p[o + 4:o + 4 + n]; o += 4 + n
            if (0x3000 <= a < 0x5000 or 0x5000 <= a < 0x7000) and n >= 16:
                q = [u32(d, k) for k in range(0, 16, 4)]
                s, tc = hdr1394(q, d)
                tr = u32(d, n - 4)
                ctx = 'ARreq' if a < 0x5000 else 'ARrsp'
                pay = ''
                if tc == 1:
                    ln = q[3] >> 16; pay = ' data=' + avc(d[16:16 + ln])
                elif tc in (0, 6):
                    pay = ' data=' + d[12:16].hex(' ')
                out.append(f"{ctx}@{a:04x} {s}{pay} [{EVT.get((tr >> 16) & 0x1f, hex((tr >> 16) & 0x1f))}]")
            elif a in (0x11cc, 0x11ec, 0x120c, 0x122c) or 0x11c0 <= a < 0x1240:
                v = u32(d, 0) if n >= 4 else 0
                out.append(f"ATst@{a:04x} {EVT.get((v >> 16) & 0x1f, hex((v >> 16) & 0x1f))}")
            elif a in (0x110c, 0x111c, 0x114c, 0x115c):
                out.append(f"ARdesc@{a:04x} res={u32(d, 0) & 0xffff:04x}")
            else:
                out.append(f"R{a:04x}[{n}]")
        elif t == 3:
            out.append(f"reg {h & 0xfffff:05x}={u32(p, o + 4):08x}"); o += 8
        elif t == 0xa:
            out.append(f"IntEvent={u32(p, o + 4):08x}"); o += 8
        else:
            out.append('?' + p[o:].hex()); break
    return out

for line in sys.stdin:
    f = line.split()
    if len(f) < 3:
        continue
    t, ep, hx = f[0], f[1], f[2]
    try:
        p = bytes.fromhex(hx)
    except ValueError:
        continue
    if ep in ('0x02', 'ep02', 'out'):
        r = dec_out(p)
    elif ep in ('0x84', 'ep84', 'in'):
        r = dec_in(p)
    else:
        continue
    r = [x for x in r if not x.startswith(('ATst', 'ARdesc'))] if '-q' in sys.argv else r
    if r:
        print(f"{t:>12} {ep} " + ' | '.join(r))
