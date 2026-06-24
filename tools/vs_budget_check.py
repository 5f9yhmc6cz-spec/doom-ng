#!/usr/bin/env python3
"""Per-map BUDGET verification for the whole-E1 single-cart plan.

Imports vs_flats.e1_assets for the CANONICAL wall ordering (so a wall's texid == its
index in the sorted walls list -- exactly what wad2c.py / vs_extract.py use), then
parses doom1.wad directly per map to compute palette + geometry budgets.

Run: python3 tools/vs_budget_check.py [doom1.wad]
"""
import struct, sys, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vs_flats import e1_assets

WAD = sys.argv[1] if len(sys.argv) > 1 else "doom1.wad"

# Canonical all-E1 ordering. A wall's texid = index in this sorted list.
walls, flats, nmaps = e1_assets(WAD)
WALLSET = set(walls)   # for "resolves to a canonical wall texid" membership test

data = open(WAD, "rb").read()
_sig, nl, dofs = struct.unpack_from("<4sii", data, 0)
DIR = [struct.unpack_from("<ii8s", data, dofs + i * 16) for i in range(nl)]
NAMES = [d[2].rstrip(b"\0").decode("latin1") for d in DIR]

def map_lump(mapidx, want):
    """Return (offset, size) of a lump belonging to the map at NAMES index mapidx."""
    for j in range(mapidx + 1, mapidx + 12):
        if NAMES[j] == want:
            return DIR[j][0], DIR[j][1]
    return None

PAL_LIMIT = 243   # wall palette range = HW slots 16..243 (sprites 244-253, backdrop 255)

rows = []
total_geom = 0
for m in range(1, 10):
    mn = f"E1M{m}"
    if mn not in NAMES:
        continue
    mi = NAMES.index(mn)

    SD = map_lump(mi, "SIDEDEFS")
    SE = map_lump(mi, "SECTORS")
    SG = map_lump(mi, "SEGS")
    SS = map_lump(mi, "SSECTORS")
    ND = map_lump(mi, "NODES")

    # (a) distinct wall textures actually used, that RESOLVE to a canonical wall texid
    used_walls = set()
    sd_off, sd_sz = SD
    for k in range(sd_sz // 30):
        b = data[sd_off + k * 30: sd_off + k * 30 + 30]
        for o in (4, 12, 20):   # upper, lower, middle
            t = b[o:o + 8].rstrip(b"\0").decode("latin1").upper()
            if t and t != "-" and t in WALLSET:
                used_walls.add(t)
    distinct_walls = len(used_walls)

    # (b) has_sky = any sector ceiling flat == F_SKY1; also collect distinct flats (f)
    has_sky = False
    used_flats = set()
    se_off, se_sz = SE
    for k in range(se_sz // 26):
        b = data[se_off + k * 26: se_off + k * 26 + 26]
        fl = b[4:12].rstrip(b"\0").decode("latin1").upper()    # floorpic
        ce = b[12:20].rstrip(b"\0").decode("latin1").upper()   # ceilingpic
        if fl:
            used_flats.add(fl)
        if ce:
            used_flats.add(ce)
        if ce == "F_SKY1":
            has_sky = True
    distinct_flats = len(used_flats)

    # (c) palette slots (walls + sky only, the CURRENT compaction)
    pal_slots = 16 + 3 * (distinct_walls + (1 if has_sky else 0))
    fits_palette = pal_slots <= PAL_LIMIT

    # (d) geometry counts
    segs = SG[1] // 12
    ssectors = SS[1] // 4
    nodes = ND[1] // 28

    # (e) geometry byte estimate
    geom_bytes = 24 * segs + 4 * ssectors + 28 * nodes
    total_geom += geom_bytes

    # (f) FUTURE: palette slots once flats are also wired into the compaction.
    # NOTE: F_SKY1 is itself one of the distinct_flats; has_sky already pays a slot
    # for sky, and the literal F_SKY1 flat is never drawn as a real flat. So subtract
    # 1 from distinct_flats when sky is present to avoid double-counting that slot.
    eff_flats = distinct_flats - (1 if has_sky else 0)
    pal_slots_with_flats = 16 + 3 * (distinct_walls + eff_flats + (1 if has_sky else 0))
    fits_with_flats = pal_slots_with_flats <= PAL_LIMIT

    rows.append(dict(
        map=mn, distinct_walls=distinct_walls, has_sky=has_sky,
        pal_slots=pal_slots, fits_palette=fits_palette,
        segs=segs, ssectors=ssectors, nodes=nodes, geom_bytes=geom_bytes,
        distinct_flats=distinct_flats, pal_slots_with_flats=pal_slots_with_flats,
        fits_with_flats=fits_with_flats,
    ))

# ---- print the full per-map table ----
print(f"Canonical all-E1 ordering: {len(walls)} wall textures, {len(flats)} flats, {nmaps} maps")
print(f"Palette wall range = HW slots 16..{PAL_LIMIT}  (228 = 3*76 max wall+sky bands)\n")
hdr = f"{'MAP':6} {'walls':>5} {'sky':>3} {'palslots':>8} {'fit?':>4} | {'segs':>5} {'ssec':>5} {'nodes':>5} {'geomB':>7} | {'flats':>5} {'pal+flats':>9} {'fit?':>4}"
print(hdr)
print("-" * len(hdr))
for r in rows:
    print(f"{r['map']:6} {r['distinct_walls']:5d} {('Y' if r['has_sky'] else 'n'):>3} "
          f"{r['pal_slots']:8d} {('OK' if r['fits_palette'] else 'OVER'):>4} | "
          f"{r['segs']:5d} {r['ssectors']:5d} {r['nodes']:5d} {r['geom_bytes']:7d} | "
          f"{r['distinct_flats']:5d} {r['pal_slots_with_flats']:9d} "
          f"{('OK' if r['fits_with_flats'] else 'OVER'):>4}")
print("-" * len(hdr))

all_fit = all(r["fits_palette"] for r in rows)
all_fit_flats = all(r["fits_with_flats"] for r in rows)
worst_pal = max(rows, key=lambda r: r["pal_slots"])
worst_palflats = max(rows, key=lambda r: r["pal_slots_with_flats"])
worst_geom = max(rows, key=lambda r: r["geom_bytes"])

print(f"\nTOTAL geometry across {len(rows)} maps: {total_geom} bytes "
      f"({total_geom/1024:.1f} KB)  vs ~880 KB P1 rodata free")
print(f"ALL fit palette now (walls+sky <= {PAL_LIMIT})?  {all_fit}")
print(f"  worst (walls+sky): {worst_pal['map']} @ {worst_pal['pal_slots']} slots "
      f"({worst_pal['distinct_walls']} walls + sky={worst_pal['has_sky']})")
print(f"ALL still fit WITH flats wired in?             {all_fit_flats}")
print(f"  worst (walls+flats+sky): {worst_palflats['map']} @ {worst_palflats['pal_slots_with_flats']} slots")
print(f"Worst geometry map: {worst_geom['map']} @ {worst_geom['geom_bytes']} bytes "
      f"({worst_geom['geom_bytes']/1024:.1f} KB)")
print(f"Total geom < 880KB?  {total_geom < 880*1024}")
