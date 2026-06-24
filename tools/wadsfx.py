#!/usr/bin/env python3
"""Extract the DOOM sound effects DOOM-NG uses from a user-supplied IWAD.
DMX format: u16 fmt, u16 rate, u32 nsamples, 16 pad bytes, then unsigned 8-bit PCM.
Writes WAVs next to the Neo Geo build; the ngdevkit %.adpcma rule converts them.
Usage: wadsfx.py doom1.wad neogeo/"""
import sys, struct, wave

WANT = {  # lump -> output (matches neogeo/Makefile's V-ROM recipe)
    "DSPISTOL": "snd_pistol.wav",
    "DSPOPAIN": "snd_impdeath.wav",   # (legacy name) the shared enemy scream -- kept; per-class deaths below override it
    "DSBAREXP": "snd_boom.wav",
    "DSITEMUP": "snd_itemup.wav",
    "DSDOROPN": "snd_dooropn.wav",
    "DSBGDTH1": "snd_impdth.wav",     # IMP death (per-class scream)
    "DSPODTH1": "snd_posdth.wav",     # POSSESSED (zombieman/sergeant) death (per-class scream)
    "DSDORCLS": "snd_dorcls.wav",     # door CLOSE (was: close reused the open sound DSDOROPN)
    "DSPSTART": "snd_pstart.wav",     # lift/platform START moving (was: lifts reused the door sound)
    "DSPSTOP":  "snd_pstop.wav",      # lift/platform reach STOP (was: silent)
}
GAIN = {  # the WAD records some SFX very quietly; scale 8-bit PCM about the 128 centre (clamped) so they're audible on the Neo Geo mix
    "DSITEMUP": 6,                    # pickup blip is peak~19/128 in the WAD -> ~6x to match the others (114<128, no clip)
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
        g = GAIN.get(name, 1)
        if g != 1:                            # amplify about the 128 centre, clamped
            pcm = bytes(max(0, min(255, (b - 128) * g + 128)) for b in pcm)
        # RESAMPLE to the YM2610 ADPCM-A FIXED playback rate so SFX play at the correct PITCH. ADPCM-A has
        # no rate register: every sample is clocked out at ~18518Hz (8MHz/432). DOOM SFX are 11025Hz, so
        # encoded as-is they play ~1.68x too fast (pitched up). Upsampling makes the .adpcma carry 1.68x
        # more samples -> correct pitch + duration. This CHANGES .adpcma sizes, so the doomsnd.s _sfxtab MUST
        # be regenerated afterwards (tools/gen_sfxtab.py reads the built .adpcma sizes).
        TARGET = 18518
        if rate != TARGET:
            import numpy as np
            src = np.frombuffer(pcm, dtype=np.uint8).astype(np.float32)
            nout = max(1, round(len(src) * TARGET / rate))
            res = np.interp(np.linspace(0.0, len(src) - 1, nout), np.arange(len(src)), src)
            pcm = np.clip(np.round(res), 0, 255).astype(np.uint8).tobytes()
            rate = TARGET
        out = f"{outdir}/{WANT[name]}"
        wv = wave.open(out, "wb"); wv.setnchannels(1); wv.setsampwidth(1); wv.setframerate(rate)
        wv.writeframes(pcm); wv.close()
        print(f"{name} -> {out} ({rate}Hz, {len(pcm)} samples)")
