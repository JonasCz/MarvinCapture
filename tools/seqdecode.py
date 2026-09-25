#!/usr/bin/env python3
# Pinnacle Studio 500-USB open driver
# Copyright (C) 2026 Jonas Cz.
#
# This program is free software: you can redistribute it and/or modify it under
# the terms of the GNU Affero General Public License as published by the Free
# Software Foundation, either version 3 of the License, or (at your option) any
# later version. This program is distributed WITHOUT ANY WARRANTY; see
# <https://www.gnu.org/licenses/> for the full licence.
"""Decode EP 0x02 command-channel traffic of the Pinnacle 500-USB into OHCI
register writes / reads, FPGA register writes, device-RAM block writes,
AT/AR/IR descriptors and 1394 packet headers, one message per line.

    tshark -r trace.pcapng -Y 'usb.endpoint_address==0x02 && usb.capdata' \\
        -T fields -e frame.number -e usb.endpoint_address -e usb.capdata \\
        | python3 tools/seqdecode.py

Input lines: <label> <endpoint> <hex payload>; only EP 0x02 lines are
decoded (a bare hex payload per line works too). This is how the old
replayed start sequence was taken apart; docs/startup.md has the result.
Message format: docs/command-channel-findings.md.
"""
import struct, sys

REG = {
    0x000: 'Version', 0x004: 'GUID_ROM', 0x008: 'ATRetries', 0x00c: 'CSRData',
    0x010: 'CSRCompareData', 0x014: 'CSRControl', 0x018: 'ConfigROMhdr',
    0x01c: 'BusID', 0x020: 'BusOptions', 0x024: 'GUIDHi', 0x028: 'GUIDLo',
    0x034: 'ConfigROMmap', 0x038: 'PostedWriteAddressLo',
    0x03c: 'PostedWriteAddressHi', 0x040: 'VendorID', 0x050: 'HCControlSet',
    0x054: 'HCControlClear', 0x064: 'SelfIDBuffer', 0x068: 'SelfIDCount',
    0x070: 'IRMultiChanMaskHiSet', 0x074: 'IRMultiChanMaskHiClear',
    0x078: 'IRMultiChanMaskLoSet', 0x07c: 'IRMultiChanMaskLoClear',
    0x080: 'IntEventSet', 0x084: 'IntEventClear', 0x088: 'IntMaskSet',
    0x08c: 'IntMaskClear', 0x090: 'IsoXmitIntEventSet',
    0x094: 'IsoXmitIntEventClear', 0x098: 'IsoXmitIntMaskSet',
    0x09c: 'IsoXmitIntMaskClear', 0x0a0: 'IsoRecvIntEventSet',
    0x0a4: 'IsoRecvIntEventClear', 0x0a8: 'IsoRecvIntMaskSet',
    0x0ac: 'IsoRecvIntMaskClear', 0x0b0: 'InitialBandwidthAvailable',
    0x0b4: 'InitialChannelsAvailableHi', 0x0b8: 'InitialChannelsAvailableLo',
    0x0dc: 'FairnessControl', 0x0e0: 'LinkControlSet',
    0x0e4: 'LinkControlClear', 0x0e8: 'NodeID', 0x0ec: 'PhyControl',
    0x0f0: 'IsochronousCycleTimer', 0x100: 'AsyncReqFilterHiSet',
    0x104: 'AsyncReqFilterHiClear', 0x108: 'AsyncReqFilterLoSet',
    0x10c: 'AsyncReqFilterLoClear', 0x110: 'PhysReqFilterHiSet',
    0x114: 'PhysReqFilterHiClear', 0x118: 'PhysReqFilterLoSet',
    0x11c: 'PhysReqFilterLoClear', 0x120: 'PhysicalUpperBound',
}
for base, name in ((0x180, 'ATreq'), (0x1a0, 'ATrsp'), (0x1c0, 'ARreq'),
                   (0x1e0, 'ARrsp')):
    REG[base] = name + '.CtlSet'; REG[base + 4] = name + '.CtlClear'
    REG[base + 0xc] = name + '.CmdPtr'
for n in range(32):
    REG[0x200 + 16 * n] = f'IT{n}.CtlSet'; REG[0x204 + 16 * n] = f'IT{n}.CtlClear'
    REG[0x20c + 16 * n] = f'IT{n}.CmdPtr'
    REG[0x400 + 32 * n] = f'IR{n}.CtlSet'; REG[0x404 + 32 * n] = f'IR{n}.CtlClear'
    REG[0x40c + 32 * n] = f'IR{n}.CmdPtr'; REG[0x410 + 32 * n] = f'IR{n}.Match'

TCODE = {0: 'wrq', 1: 'wrb', 2: 'wrresp', 4: 'rdq', 5: 'rdb', 6: 'rdqresp',
         7: 'rdbresp', 9: 'lock', 0xb: 'lockresp', 0xe: 'phy'}
CMD = {0: 'OUTPUT_MORE', 1: 'OUTPUT_LAST', 2: 'INPUT_MORE', 3: 'INPUT_LAST'}


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def desc(q):
    """One 16-byte OHCI descriptor (4 LE quadlets)."""
    c = q[0]
    cmd, key = c >> 28, (c >> 24) & 7
    s = f"{CMD.get(cmd, 'cmd%x' % cmd)}{'-Imm' if key == 2 else ''}"
    s += f" s={(c >> 27) & 1} i={(c >> 20) & 3} b={(c >> 18) & 3} req={c & 0xffff}"
    if key != 2:
        s += f" addr={q[1]:x}"
    s += f" branch={q[2]:x}"
    return s


def hdr1394(q):
    tc = (q[0] >> 4) & 0xf
    s = f"{TCODE.get(tc, 'tc%x' % tc)} tl={(q[0] >> 10) & 0x3f} rt={(q[0] >> 8) & 3} spd={q[0] & 7}"
    if tc in (0, 1, 4, 5, 9):
        s += f" dest={q[1] >> 16:04x} off={((q[1] & 0xffff) << 32) | q[2]:012x}"
        if tc in (1, 5, 9) and len(q) > 3:
            s += f" len={q[3] >> 16} xt={q[3] & 0xffff}"
        elif tc == 0 and len(q) > 3:
            s += f" data={struct.pack('<I', q[3]).hex(' ')}"
    elif tc in (2, 6, 7, 0xb):
        s += f" dest={q[1] >> 16:04x} rcode={(q[1] >> 12) & 0xf}"
    return s


def ram(a, d):
    """Interpret a type-8 block write into device RAM."""
    q = [u32(d, k) for k in range(0, len(d) - 3, 4)]
    out = []
    # AT descriptor blocks: OUTPUT_*-Immediate followed by the 1394 header
    if len(q) >= 8 and (q[0] >> 24) in (0x02, 0x12):
        hl = (q[0] & 0xffff) // 4
        out.append(desc(q[0:4]))
        out.append('  hdr ' + hdr1394(q[4:4 + hl]))
        if (q[0] >> 24) == 0x02 and len(q) >= 12:     # MORE-Imm + LAST + payload
            out.append('  ' + desc(q[8:12]))
            n = q[8] & 0xffff
            out.append('  payload ' + d[48:48 + n].hex(' '))
        return out
    # IR / AR descriptors: INPUT_MORE/LAST
    if q and (q[0] >> 28) in (2, 3) and len(q) % 4 == 0:
        for k in range(0, len(q), 4):
            out.append(desc(q[k:k + 4]))
        return out
    return [d.hex(' ')]


def decode(p):
    o, out = 0, []
    while o + 4 <= len(p):
        h = u32(p, o); t = h >> 28
        rep = f" reply#{(h >> 20) & 0x7f}" if h & (1 << 27) else ''
        if t in (2, 3):
            v = u32(p, o + 4) if o + 8 <= len(p) else 0
            a = h & 0xfffff
            name = REG.get(a - 0x10000, f'reg{a:05x}') if a >= 0x10000 else f'addr{a:05x}'
            tag = (h >> 20) & 0x7f
            if t == 2:
                out.append(f"W {name} = {v:08x}" + (f" [tag {tag:02x}]" if tag and not rep else '') + rep)
            else:
                out.append(f"R {name}{rep}")
            o += 8
        elif t in (4, 5, 6, 7):
            v = u32(p, o + 4) if o + 8 <= len(p) else 0
            what = {4: 'FPGA-USB reg', 5: 'FPGA-USB read', 6: 'FPGA-ext',
                    7: 'type7'}[t]
            a = h & 0xfffff
            idx = f"{a - 0x10000:03x}" if t == 6 else f"{a}"
            out.append(f"{what}[{idx}] = {v:08x}{rep}" if t != 5 else f"{what}[{idx}]{rep}")
            o += 8
        elif t == 8:
            n = (h >> 16) & 0x7ff; a = h & 0xffff; d = p[o + 4:o + 4 + n]
            lines = ram(a, d)
            out.append(f"RAM[{a:04x}..+{n}] " + lines[0])
            out += ['        ' + x for x in lines[1:]]
            o += 4 + n
        else:
            out.append('? ' + p[o:].hex(' ')); break
    return out


if __name__ == '__main__':
    for n, line in enumerate(sys.stdin):
        f = line.split()
        if not f:
            continue
        if len(f) >= 3:
            label, ep, hx = f[0], f[1], f[2]
            if ep not in ('0x02', '2', 'ep02', 'out'):
                continue
        else:
            label, hx = str(n), f[-1]
        try:
            p = bytes.fromhex(hx.replace(':', ''))
        except ValueError:
            continue
        for j, text in enumerate(decode(p)):
            print(f"{label:>8}" if j == 0 else ' ' * 8, text)
