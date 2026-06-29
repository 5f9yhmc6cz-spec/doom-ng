#!/usr/bin/env python3
# Bake EVERY canonical E1 flat's perspective LUT into ONE C-ROM bank (neogeo/vsflat.c1/c2), appended
# AFTER vsceil (the new chain end). Per-flat tier (views=NA, phases=NPH) from flat_gallery/tiers.csv
# (default 8/4); ALL flats use the 90deg SQUARE fold now (the floor was unified to it), so FLNAL=4*NA
# spans the 90deg arc with NA samples and the cart folds set=((ang&63)*NA)>>6. The ceiling reuses each
# flat's floor bake VFLIP'd at draw. Indexed by the SHARED flat slot (tools/vs_flats.e1_flat_order) so
# vs_extract's per-seg VEFFL/VECFL index VSFLAT_BASE[] directly. Mirrors tools/vsfloorlut.py (1 flat).
import struct, subprocess, os, sys, csv
import shutil as _sh
from PIL import Image
_TILETOOL = _sh.which("tiletool.py") or "/opt/homebrew/bin/tiletool.py"
_BREWPY = ("/opt/homebrew/opt/python3/bin:") if os.path.isdir("/opt/homebrew/opt/python3/bin") else ""
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.join(HERE, "..")
NG = os.path.join(ROOT, "neogeo"); WAD = os.environ.get("WAD", os.path.join(ROOT, "doom1.wad"))
HOST = os.path.join(ROOT, "doomng-host")
sys.path.insert(0, HERE)
from vs_flats import e1_assets, e1_flat_order

# --- canonical flats (shared slot order) + the host FLAT texid (= walls-present-in-TEXTURE1 + slot) ---
WALLS, _F, _ = e1_assets(WAD)
ordered, _slotof = e1_flat_order(WAD)            # ordered[slot] = NAME; slot == flat_slot == VEFFL value
data = open(WAD, "rb").read()
_, n, diro = struct.unpack_from("<4sii", data, 0)
D = [struct.unpack_from("<ii8s", data, diro + i * 16) for i in range(n)]
nm = [d[2].rstrip(b"\0").decode("latin1") for d in D]
# WALLCOUNT = the flat-block start in the host's e1m1.crom -- the texid --bakefloor ACTUALLY indexes.
# MUST come from the CROM the host loads, not a re-derived TEXTURE1 count: those drifted (TEXTURE1-only
# 103 vs the CROM's real 245-wall layout) -> every flat slot baked the wrong texture (2026-06-23 flats-
# garbage bug, confirmed on MAME). The appended flats are the one long contiguous 64x64 run.
def _crom_wallcount(path):
    b = open(path, "rb").read(); q = 768; nt = struct.unpack_from("<I", b, q)[0]; q += 4
    dm = [struct.unpack_from("<hhI", b, q + i * 8)[:2] for i in range(nt)]
    best = (0, 0); i = 0
    while i < nt:
        if dm[i] == (64, 64):
            j = i
            while j < nt and dm[j] == (64, 64): j += 1
            if j - i > best[1]: best = (i, j - i)
            i = j
        else: i += 1
    return best   # (start, length)
_flat_start, _flat_len = _crom_wallcount(os.path.join(ROOT, "e1m1.crom"))
if _flat_len != len(ordered):
    raise SystemExit("vsflatlut: e1m1.crom flat-block is %d flats but e1_flat_order gives %d -- "
                     "DOOMNG_EPISODES mismatch between wad2c (e1m1.crom) and vsflatlut. "
                     "Rebuild e1m1.crom with the SAME episode set (this is the flats-garbage bug)."
                     % (_flat_len, len(ordered)))
WALLCOUNT = _flat_start

# --- per-flat tier (views=NA, phases=NPH); default 8/4. (fold column ignored: all 90deg now.) ---
DEFAULT = (8, 4)
tiers = {}
csvp = os.path.join(ROOT, "flat_gallery", "tiers.csv")
if os.path.exists(csvp):
    for row in csv.reader(open(csvp)):
        if not row or not row[0].strip() or row[0].lstrip().startswith("#"): continue
        f = row[0].split("#")[0].strip()
        if f == "flat": continue
        try: tiers[f] = (int(row[4]), int(row[5]))
        except (IndexError, ValueError): pass
def tier(name): return tiers.get(name, DEFAULT)

def ng16(r, g, b):
    r5, g5, b5 = r >> 3, g >> 3, b >> 3
    return ((r5 & 1) << 14) | ((g5 & 1) << 13) | ((b5 & 1) << 12) | ((r5 >> 1) << 8) | ((g5 >> 1) << 4) | (b5 >> 1)

COLS, ROWS = 20, 7
NFLAT = len(ordered); SKY = "F_SKY1"
base = [-1] * NFLAT; na_a = [0] * NFLAT; nph_a = [0] * NFLAT; pal_a = [[0] * 16 for _ in range(NFLAT)]
parts = []; off = 0                                  # parts = (c1path,c2path) per baked flat, in slot order
ttenv = dict(os.environ); ttenv["PATH"] = _BREWPY + ttenv.get("PATH", "")
NOLUT = os.environ.get("VSFLAT_NOLUT", "") not in ("", "0")   # uber-36: skip the REAL-flat LUT bake -> base[]=-1 so nflat=0 at runtime + 0-byte vsflat.c1/c2. Frees the per-map flat palette slots for the fog bands (g_generic=1 already draws the SYNTHETIC vsfloor/vsceil LUT, never these). Self-check above still runs (order derivation). Re-enable: drop VSFLAT_NOLUT + set gen=0 (param 5).
for slot, name in enumerate(ordered):
    if name == SKY:                                  # sky: no floor LUT (cart leaves backdrop), base stays -1
        continue
    if NOLUT: continue                               # uber-36 (VSFLAT_NOLUT): base stays -1, no tiletool, no tiles emitted for this flat
    na, nph = tier(name)
    env = dict(os.environ)
    env.update(FLAT=str(WALLCOUNT + slot), FLNA=str(na), FLNPHASE=str(nph), FLNAL=str(4 * na),
               FLOUT="/tmp/vsflat_%d.raw" % slot, FLPAL="/tmp/vsflat_%d.pal" % slot)
    subprocess.run([HOST, "--bakefloor"], check=True, env=env)
    raw = open(env["FLOUT"], "rb").read(); w, h, rna, rnph = struct.unpack("iiii", raw[:16])
    assert (w, rna, rnph) == (320, na, nph), "vsflat %s header mismatch %s" % (name, (w, rna, rnph))
    px = raw[16:16 + w * h]
    pal = [tuple(map(int, l.split())) for l in open(env["FLPAL"]) if l.strip()]
    img = Image.frombytes("P", (w, h), bytes(px))
    fp = [0, 0, 0]
    for (r, g, b) in pal: fp += [r, g, b]
    fp += [0, 0, 0] * (256 - 1 - len(pal)); img.putpalette(fp)
    # tiletool THIS flat alone -> raw 64B tiles (one GIF per flat sidesteps the 65535px GIF limit);
    # concatenated in slot order below, so VSFLAT_BASE[slot] = cumulative tile count.
    gp = "/tmp/vsflat_%d.gif" % slot; img.save(gp, optimize=False)
    c1 = "/tmp/vsflat_%d.c1" % slot; c2 = "/tmp/vsflat_%d.c2" % slot
    subprocess.run([_TILETOOL, "--sprite", "-c", gp, "-o", c1, c2], check=True, env=ttenv)
    parts.append((c1, c2))
    base[slot] = off; na_a[slot] = na; nph_a[slot] = nph
    p16 = [0] + [ng16(*c) for c in pal]; p16 += [0] * (16 - len(p16)); pal_a[slot] = p16[:16]
    off += COLS * ROWS * na * nph                    # tiles this flat contributes to the bank

# --- concatenate per-flat banks (slot order) into ONE vsflat.c1/c2 (raw tiles, no header) ---
with open(os.path.join(NG, "vsflat.c1"), "wb") as o1, open(os.path.join(NG, "vsflat.c2"), "wb") as o2:
    for c1, c2 in parts:
        o1.write(open(c1, "rb").read()); o2.write(open(c2, "rb").read())

# --- TILE0 = full chain before vsflat (logo..vsceil); rebase BASE to absolute tile index ---
def tiles(fn):
    p = os.path.join(NG, fn); return os.path.getsize(p) // 64 if os.path.exists(p) else 0
TILE0 = (tiles("build/assets/base-crom-logo.c1") + tiles("build/assets/logo.c1")
         + tiles("textiles.c1") + tiles("floorlut.c1") + tiles("sprites.c1")
         + tiles("ceillut.c1") + tiles("ramps.c1") + tiles("vsfloor.c1") + tiles("vsceil.c1"))
base = [(b + TILE0) if b >= 0 else -1 for b in base]

with open(os.path.join(NG, "vsflat.h"), "w") as f:
    f.write("/* generated by tools/vsflatlut.py -- per-flat perspective LUT bank (%d flats, 90deg square fold), chain-end after vsceil */\n" % NFLAT)
    f.write("#define VSFLAT_TILE0 %d\n#define VSFLAT_COLS %d\n#define VSFLAT_ROWS %d\n#define VSFLAT_NFLAT %d\n" % (TILE0, COLS, ROWS, NFLAT))
    f.write("static const int VSFLAT_BASE[VSFLAT_NFLAT]={%s};\n" % ",".join(str(b) for b in base))   # absolute tile; -1 = sky/unbaked -> synthetic fallback
    f.write("static const unsigned char VSFLAT_NA[VSFLAT_NFLAT]={%s};\n" % ",".join(str(x) for x in na_a))
    f.write("static const unsigned char VSFLAT_NPH[VSFLAT_NFLAT]={%s};\n" % ",".join(str(x) for x in nph_a))
    f.write("static const unsigned short VSFLAT_PAL16[VSFLAT_NFLAT][16]={\n")
    for p in pal_a: f.write("{%s},\n" % ",".join(str(x) for x in p))
    f.write("};\n")
bt = tiles("vsflat.c1")
print("vsflat: TILE0=%d NFLAT=%d, %d baked, %d bank tiles -> chain end %d/1048576 (%.1f%%)"
      % (TILE0, NFLAT, len(parts), bt, TILE0 + bt, 100.0 * (TILE0 + bt) / 1048576))
