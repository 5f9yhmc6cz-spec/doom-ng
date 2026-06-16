#!/usr/bin/env python3
"""Extract the DOOM sound effects DOOM-NG uses from a user-supplied IWAD.
DMX format: u16 fmt, u16 rate, u32 nsamples, 16 pad bytes, then unsigned 8-bit PCM.
Writes WAVs next to the Neo Geo build; the ngdevkit %.adpcma rule converts them.
Usage: wadsfx.py doom1.wad neogeo/"""
import sys, struct, wave

WANT = {  # lump -> output (matches neogeo/Makefile's V-ROM recipe)
    "DSPISTOL": "snd_pistol.wav",
    "DSPOPAIN": "snd_impdeath.wav",
    "DSBAREXP": "snd_boom.wav",
    "DSITEMUP": "snd_itemup.wav",
    "DSDOROPN": "snd_dooropn.wav",
}

wadp, outdir = sys.argv[1], sys.argv[2].rstrip("/")
w = open(wadp, "rb").read()
nl, doff = struct.unpack("<ii", w[4:12])
for i in range(nl):
    off, sz = struct.unpack("<ii", w[doff+i*16:doff+i*16+8])
    name = w[doff+i*16+8:doff+i*16+16].rstrip(b"\0").decode()
    if name in WANT:
        fmt, rate = struct.unpack("<HH", w[off:off+4])
        ns, = struct.unpack("<I", w[off+4:off+8])
        pcm = w[off+24:off+24+ns-32]          # skip 16-byte lead pad; drop the 16-byte tail pad
        out = f"{outdir}/{WANT[name]}"
        wv = wave.open(out, "wb"); wv.setnchannels(1); wv.setsampwidth(1); wv.setframerate(rate)
        wv.writeframes(pcm); wv.close()
        print(f"{name} -> {out} ({rate}Hz, {len(pcm)} samples)")
