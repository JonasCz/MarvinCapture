#!/usr/bin/env python3
# Pinnacle Studio 500-USB open driver
# Copyright (C) 2026 Jonas Cz.
#
# This program is free software: you can redistribute it and/or modify it under
# the terms of the GNU Affero General Public License as published by the Free
# Software Foundation, either version 3 of the License, or (at your option) any
# later version. This program is distributed WITHOUT ANY WARRANTY; see
# <https://www.gnu.org/licenses/> for the full licence.
"""Extract the three FPGA bitstreams (and the FX2 firmware image) embedded in your own copy of the vendor
driver MarvinAVS64.sys (from Pinnacle_Video_Driver_64bit.msi ->
Data1.cab -> marvinavs64.cab). Never redistribute the output.

    python3 scripts/extract-bitstreams.py MarvinAVS64.sys [outdir]

MarvinAVS64.sys (FUN_0002c280) loads one of three 78,422-byte bitstreams
depending on what the device is to do:

    ohci     1394 / DV / HDV capture  (the one pincli uploads)
    render   playback to the analog outputs
    capture  analog capture

The offsets below are for the driver version dated 2007-05-09 (the one in
the vendor MSI); each blob is checked against a known MD5 so a
different version fails loudly instead of producing garbage. See
docs/analog.md.
"""
import hashlib, os, struct, sys

SIZE = 0x13256
BLOBS = [   # name, virtual address, md5
    ("ohci",    0x4aa70, "3888c23c9bcc81c88964c45d981a0b68"),
    ("render",  0x5dcd0, "cacaa36da76a4499fb27f727aad1cf96"),
    ("capture", 0x70f30, "280bacc631c6a69b831734e3bc72969d"),
]
# The FX2 (USB controller) firmware the driver downloads to units that boot
# without one (MovieBox Deluxe): a raw 8051 image at VA 0x49720, which runs up
# to the start of the OHCI bitstream (0x1350 bytes). Written as fx2-marvin.bin.
FX2_VA, FX2_SIZE, FX2_MD5 = 0x49720, 0x1350, "22f873f4c47d52de4a4423ef19e6e29b"


def va_to_offset(pe, va):
    """Map a virtual address to a file offset using the PE section table."""
    e_lfanew = struct.unpack_from("<I", pe, 0x3c)[0]
    nsec = struct.unpack_from("<H", pe, e_lfanew + 6)[0]
    opt_size = struct.unpack_from("<H", pe, e_lfanew + 20)[0]
    image_base = struct.unpack_from("<Q", pe, e_lfanew + 24 + 24)[0]
    rva = va - image_base
    sec = e_lfanew + 24 + opt_size
    for i in range(nsec):
        vsize, vaddr, _, raw = struct.unpack_from("<IIII", pe, sec + 40 * i + 8)
        if vaddr <= rva < vaddr + max(vsize, 1):
            return raw + (rva - vaddr)
    raise ValueError("address 0x%x not in any section" % va)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    pe = open(sys.argv[1], "rb").read()
    out = sys.argv[2] if len(sys.argv) > 2 else "."
    os.makedirs(out, exist_ok=True)
    ok = True
    for name, va, md5 in BLOBS:
        off = va_to_offset(pe, va)
        blob = pe[off:off + SIZE]
        got = hashlib.md5(blob).hexdigest()
        if got != md5:
            print("%-8s md5 %s does not match %s -- different driver version?" % (name, got, md5))
            ok = False
            continue
        path = os.path.join(out, "fpga-%s.bin" % name)
        open(path, "wb").write(blob)
        print("%-8s -> %s" % (name, path))
    off = va_to_offset(pe, FX2_VA)
    blob = pe[off:off + FX2_SIZE]
    got = hashlib.md5(blob).hexdigest()
    if got != FX2_MD5:
        print("fx2      md5 %s does not match %s -- different driver version?" % (got, FX2_MD5))
        ok = False
    else:
        path = os.path.join(out, "fx2-marvin.bin")
        open(path, "wb").write(blob)
        print("fx2      -> %s" % path)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
