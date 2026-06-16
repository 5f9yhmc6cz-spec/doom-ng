#!/usr/bin/env python3
# fix_tile0.py -- repair the C-ROM tile-chain offsets after a sprite re-bake.
#
# WHY THIS EXISTS:
#   The C-ROM is a CONCATENATION chain (neogeo/Makefile CROM1 deps, in order):
#     base-crom-logo, logo, textiles, floorlut, SPRITES, ceillut, ramps, vsfloor, vsceil, vsflat
#   Each downstream component's tile base (CEILLUT_TILE0, RAMP_TILE0, VSFLOOR_TILE0,
#   VSCEIL_TILE0, VSFLAT_TILE0, and TITLE_TILE0 which lives inside ramps.c1) is BAKED
#   into its .h at generation time = the sum of all .c1 chain lengths before it.
#   sprites.c1 sits MID-CHAIN, so re-running tools/wad2c.py to add/remove a sprite frame
#   grows/shrinks sprites.c1 and shifts every downstream component -- but the build does
#   NOT auto-regenerate ceillut/ramps/vsfloor/vsceil/vsflat (their generators need the
#   /tmp/*.raw host LUT bakes, which aren't always present). Result: stale TILE0s ->
#   the renderer addresses every floor/ceiling/flat/ramp/title tile 14-ish tiles early
#   -> the ENTIRE cart renders as garbage.
#
#   This tool recomputes each downstream TILE0 from the ACTUAL current .c1 sizes and
#   rewrites the .h #defines. The tile CONTENT is untouched (it's already correct) -- only
#   the base offsets are corrected. Idempotent: re-running sets the same absolute values.
#
# RUN IT after `python3 tools/wad2c.py` (the sprite bake) whenever the sprite set changed.
# (Neo Geo C-ROM tile = 16x16x4bpp = 128 bytes, split 64 in .c1 + 64 in .c2 -> tiles = c1_size/64.)
import os, re, glob, sys

NG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "neogeo")

def tiles(rel):
    g = glob.glob(os.path.join(NG, rel))
    if not g: sys.exit("fix_tile0: missing %s (run the bake first)" % rel)
    return os.path.getsize(g[0]) // 64

def get(hf, name):
    p = os.path.join(NG, hf)
    m = re.search(r'#define\s+%s\s+(\d+)' % name, open(p).read())
    return int(m.group(1)) if m else None

def setdef(hf, name, val):
    p = os.path.join(NG, hf); s = open(p).read(); old = get(hf, name)
    open(p, "w").write(re.sub(r'(#define\s+%s\s+)\d+' % name, r'\g<1>%d' % val, s, count=1))
    tag = "" if old != val else "  (already correct)"
    print("  %-12s %-14s %s -> %s%s" % (hf, name, old, val, tag))

# chain offsets (each = sum of all .c1 tiles before it)
off_cl  = (tiles("build/assets/base-crom-logo.c1") + tiles("build/assets/logo.c1")
           + tiles("textiles.c1") + tiles("floorlut.c1") + tiles("sprites.c1"))
off_rp  = off_cl  + tiles("ceillut.c1")
off_vf  = off_rp  + tiles("ramps.c1")
off_vc  = off_vf  + tiles("vsfloor.c1")
off_vfl = off_vc  + tiles("vsceil.c1")

# AVG / TITLEPIC / MENU tiles all live INSIDE ramps.c1 (wad2c --ramps appends, in order:
# ramp-edge tiles, AVG_TILE0, TITLE_TILE0, MENU_TILE0), so all three shift by the same amount
# RAMP_TILE0 does. We can't derive their offsets from .c1 file boundaries, so reconstruct each
# one's offset-WITHIN-ramps from the CURRENT headers (RAMP and the embedded base must be read in
# the SAME state) and re-anchor to the corrected RAMP base off_rp. Capture BEFORE rewriting ramps.h.
cur_rp = get("ramps.h", "RAMP_TILE0")
EMBEDDED = [("ramps.h", "AVG_TILE0"), ("titlepic.h", "TITLE_TILE0"), ("menu.h", "MENU_TILE0")]
emb = [(hf, name, get(hf, name)) for hf, name in EMBEDDED]

print("fix_tile0: rewriting downstream C-ROM tile bases from the live chain:")
setdef("ceillut.h", "CEILLUT_TILE0", off_cl)
setdef("ramps.h",   "RAMP_TILE0",    off_rp)
setdef("vsfloor.h", "VSFLOOR_TILE0", off_vf)
setdef("vsceil.h",  "VSCEIL_TILE0",  off_vc)
setdef("vsflat.h",  "VSFLAT_TILE0",  off_vfl)
for hf, name, old in emb:                       # embedded-in-ramps banks: re-anchor offset-within-ramps to off_rp
    if old is not None and cur_rp is not None:
        setdef(hf, name, off_rp + (old - cur_rp))
print("fix_tile0: done -- rebuild the cart (gmake cart) to pick up the corrected bases.")
