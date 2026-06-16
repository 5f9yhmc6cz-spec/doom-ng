#!/usr/bin/env python3
"""Extract one named lump from a DOOM WAD. Usage: wadlump.py file.wad LUMPNAME out.bin"""
import sys, struct
w = open(sys.argv[1], "rb").read()
nl, doff = struct.unpack("<ii", w[4:12])
want = sys.argv[2]
for i in range(nl):
    off, sz = struct.unpack("<ii", w[doff+i*16:doff+i*16+8])
    nm = w[doff+i*16+8:doff+i*16+16].rstrip(b"\0").decode()
    if nm == want:
        open(sys.argv[3], "wb").write(w[off:off+sz])
        print(f"{want}: {sz}B -> {sys.argv[3]}")
        break
else:
    sys.exit(f"{want} NOT FOUND in {sys.argv[1]}")
