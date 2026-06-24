#!/usr/bin/env python3
"""Generate the doomsnd.s _sfxtab from the BUILT .adpcma sizes.
The SFX live sequentially in V-ROM after the 10 music tracks, starting at byte 0x500000 (= 256-byte block
0x5000). Each ADPCM-A sample is 256-byte block aligned. When the SFX are resampled (wadsfx.py) their sizes
change, so this regenerates the start/stop block table. Paste the output over the _sfxtab .db block.
Usage: gen_sfxtab.py <dir-with-.adpcma>   (default: neogeo)"""
import sys, os
DIR = sys.argv[1] if len(sys.argv) > 1 else "neogeo"
# (.adpcma basename, label) in the V-ROM cat order -- MUST match neogeo/Makefile's $(VROM1) recipe.
SFX = [("snd_pistol", "pistol"), ("snd_impdeath", "scream"), ("snd_boom", "boom"),
       ("snd_itemup", "itemup"), ("snd_dooropn", "door"), ("snd_impdth", "impdth"), ("snd_posdth", "posdth"),
       ("snd_dorcls", "dorcls"), ("snd_pstart", "pstart"), ("snd_pstop", "pstop")]
# music tracks before the SFX in V-ROM cat order: E1M1-9 + intro (10) + E2M1-9 + E3M1-9 (18) = 28.
NTRACKS = int(os.environ.get("NTRACKS", "28"))
BASE = NTRACKS * 0x800   # SFX base block = (NTRACKS * 512KB music) / 256  (10->0x5000, 28->0xE000)
blk = BASE
print("_sfxtab:")
for f, label in SFX:
    sz = os.path.getsize(os.path.join(DIR, f + ".adpcma"))
    n = (sz + 255) // 256
    start, stop = blk, blk + n - 1
    print("        .db     0x%02x, 0x%02x, 0x%02x, 0x%02x   ; %s  0x%04x-0x%04x (%d blk, %s)" % (
        start & 0xff, (start >> 8) & 0xff, stop & 0xff, (stop >> 8) & 0xff, label, start, stop, n, f))
    blk += n
