#!/usr/bin/env python3
"""Single source of truth for the all-Episode-1 C-ROM ordering.

Both wad2c.py (which builds the C-ROM tiles) and vs_extract.py (which bakes per-seg texture/flat
IDs into the geometry) MUST agree on the order of wall textures and flats, or maps E1M2..E1M9 will
index the wrong tiles in the shared all-E1 C-ROM (E1M1 would still look perfect -- the subtle bug).
This module returns the canonical sorted union used across E1M1..E1M9, so both tools import it.

  from vs_flats import e1_assets
  walls, flats = e1_assets("doom1.wad")     # both sorted; walls then flats then sky = the tile order
CLI: python3 tools/vs_flats.py [doom1.wad]  -> prints the ordering for verification.
"""
import struct, sys

def e1_assets(wadpath, episode=1):
    wad = open(wadpath, "rb").read()
    _, n, diro = struct.unpack_from("<4sii", wad, 0)
    D = [struct.unpack_from("<ii8s", wad, diro + i * 16) for i in range(n)]
    names = [d[2].rstrip(b"\0").decode("latin1") for d in D]
    walls, flats = set(), set()
    nmaps = 0
    for m in range(1, 10):
        mn = f"E{episode}M{m}"
        if mn not in names:
            continue
        nmaps += 1
        mi = names.index(mn)
        def lump(w):
            for j in range(mi + 1, mi + 12):
                if names[j] == w:
                    return wad[D[j][0]:D[j][0] + D[j][1]]
            return b""
        SD, SE = lump("SIDEDEFS"), lump("SECTORS")
        for k in range(len(SD) // 30):
            b = SD[k * 30:k * 30 + 30]
            for o in (4, 12, 20):                       # upper, lower, middle texture names
                t = b[o:o + 8].rstrip(b"\0").decode("latin1").upper()   # DOOM texture names are case-insensitive (uppercase-canonical); normalize so STONE/stone don't double-bake
                if t and t != "-":
                    walls.add(t)
        for k in range(len(SE) // 26):
            b = SE[k * 26:k * 26 + 26]
            for o in (4, 12):                           # floorpic, ceilingpic
                f = b[o:o + 8].rstrip(b"\0").decode("latin1").upper()    # flats are uppercase-canonical too
                if f:
                    flats.add(f)
    return sorted(walls), sorted(flats), nmaps

def e1_flat_order(wadpath, episode=1):
    """The ONE canonical, WAD-present flat order shared by wad2c (C-ROM flat tiles), vsflatlut
    (per-flat LUT bank slots) and vs_extract (per-seg slot ids). A flat earns a slot iff its lump
    exists AND is a full 64x64 flat (>=4096B) -- wad2c's exact gate. The shared SLOT (0..NFLAT-1)
    is the index into this list -> all three consumers agree, so maps 2..9 can't misindex.
    Returns (ordered_names, {NAME:slot})."""
    wad = open(wadpath, "rb").read()
    _, n, diro = struct.unpack_from("<4sii", wad, 0)
    D = [struct.unpack_from("<ii8s", wad, diro + i * 16) for i in range(n)]
    names = [d[2].rstrip(b"\0").decode("latin1") for d in D]
    size = {}
    for i, nm in enumerate(names):
        size.setdefault(nm.upper(), D[i][1])   # first occurrence wins (matches wad2c's gdir)
    _, flats, _ = e1_assets(wadpath, episode)  # already sorted + uppercased
    ordered = [f for f in flats if size.get(f.upper(), -1) >= 4096]
    return ordered, {f: i for i, f in enumerate(ordered)}

def flat_slot(name, wadpath="doom1.wad", _cache={}):
    """flat NAME -> shared slot (0..NFLAT-1); -1 if not a baked flat. (F_SKY1 IS a >=4096 lump so it
    gets a slot; callers map F_SKY1 ceilings to their own sky sentinel.)"""
    if wadpath not in _cache:
        _cache[wadpath] = e1_flat_order(wadpath)[1]
    return _cache[wadpath].get(name.upper(), -1)

if __name__ == "__main__":
    wp = sys.argv[1] if len(sys.argv) > 1 else "doom1.wad"
    w, f, nm = e1_assets(wp)
    print(f"E1 canonical ordering across {nm} maps: {len(w)} wall textures, {len(f)} flats")
    print("walls:", " ".join(w))
    print("flats:", " ".join(f))
