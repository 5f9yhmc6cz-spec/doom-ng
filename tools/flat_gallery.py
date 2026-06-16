#!/usr/bin/env python3
"""Render each flat a map uses to a PNG + an editable per-flat LUT-spec table, for MANUAL
floor/ceiling LUT categorisation. Output -> repo/flat_gallery/:
  <name>.png   the flat (64x64, NEAREST-upscaled 4x)
  tiers.csv    one row per flat -- EDIT fold / views / phases:

    fold   = rotational symmetry (how far you turn before it looks the same), sets the baked arc:
             HEX = 60deg (6-fold)   SQ = 90deg (4-fold)   FULL = 360deg (no symmetry / directional)
             ANY = looks identical at any angle (speckle/noise) -- a couple of views is plenty
    views  = baked angle snapshots ACROSS that fold arc. more views = smoother spin.
             e.g. speckle ANY/2 ; a hex floor HEX/12-21 ; a busy directional FULL/24+
    phases = floor-scroll steps (the flow as you walk).  LUT tiles per flat = 20 * 7 * views * phases.

Re-running PRESERVES your edits (old UNIFORM/GRID/DIRECTIONAL buckets map to fold/views/phases).
Usage: python3 tools/flat_gallery.py [doom1.wad] [E1M1]
"""
import struct, sys, os, collections, csv
from PIL import Image

WAD = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("DOOM_WAD", "doom1.wad")
MAP = sys.argv[2] if len(sys.argv) > 2 else "E1M1"
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "flat_gallery")
os.makedirs(OUT, exist_ok=True)
CSVP = os.path.join(OUT, "tiers.csv")

wad = open(WAD, "rb").read()
_, nl, diro = struct.unpack_from("<4sii", wad, 0)
D = [struct.unpack_from("<ii8s", wad, diro + i * 16) for i in range(nl)]
names = [d[2].rstrip(b"\0").decode("latin1") for d in D]
def lump(nm): i = names.index(nm); return wad[D[i][0]:D[i][0] + D[i][1]]
pp = lump("PLAYPAL"); pal = [(pp[i * 3], pp[i * 3 + 1], pp[i * 3 + 2]) for i in range(256)]
mi = names.index(MAP)
sec = next(wad[D[j][0]:D[j][0] + D[j][1]] for j in range(mi + 1, mi + 12) if names[j] == "SECTORS")
fu, cu = collections.Counter(), collections.Counter()
for k in range(len(sec) // 26):
    b = sec[k * 26:k * 26 + 26]
    fu[b[4:12].rstrip(b"\0").decode("latin1")] += 1
    cu[b[12:20].rstrip(b"\0").decode("latin1")] += 1
flats = sorted(set(fu) | set(cu))

TIER2 = {"UNIFORM": ("ANY", "4", "2"), "GRID": ("SQ", "8", "4"),
         "DIRECTIONAL": ("HEX", "21", "8"), "SKY": ("SKY", "0", "0")}
prior = {}
if os.path.exists(CSVP):
    for row in csv.reader(open(CSVP)):
        if row and row[0] != "flat" and not row[0].startswith("#"):
            prior[row[0]] = [c.strip() for c in row]
def spec(nm):
    r = prior.get(nm)
    if r:
        if len(r) >= 6 and r[4].isdigit():          # already new format -> keep your edits
            return r[3], r[4], r[5]
        if len(r) >= 4 and r[3].upper() in TIER2:   # old bucket -> map
            return TIER2[r[3].upper()]
    return ("SKY", "0", "0") if nm == "F_SKY1" else ("SQ", "8", "4")

rows = []
for nm in flats:
    fold, views, phases = spec(nm)
    try:
        i = names.index(nm); off, sz = D[i][0], D[i][1]
        if sz == 4096:
            img = Image.new("RGB", (64, 64)); img.putdata([pal[b] for b in wad[off:off + 4096]])
            img.resize((256, 256), Image.NEAREST).save(os.path.join(OUT, nm + ".png"))
    except ValueError:
        pass
    rows.append((nm, fu.get(nm, 0), cu.get(nm, 0), fold, views, phases))

with open(CSVP, "w") as f:
    f.write("flat,floor_sectors,ceil_sectors,fold,views,phases   # fold HEX=60 SQ=90 FULL=360 ANY=invariant ; tiles=20*7*views*phases\n")
    for nm, fn, cn, fold, v, p in rows:
        f.write(f"{nm},{fn},{cn},{fold},{v},{p}\n")
tot = sum(20 * 7 * int(v) * int(p) for *_, v, p in rows if v.isdigit() and v != "0")
print(f"{MAP}: {len(rows)} flats -> {OUT}/   current spec totals ~{tot:,} tiles of 1,048,576")
