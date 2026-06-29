#!/usr/bin/env python3
# wad2c.py -- extract one map (geometry + prebuilt BSP + wall textures) from a
# Doom WAD. Emits src/map.c (geometry, sidedef texture ids, seg offsets) plus a
# binary e1m1.crom (Doom palette + composited wall textures) the host loads.
import struct, sys, os
import shutil as _sh, os as _os
_TILETOOL = _sh.which("tiletool.py") or "/opt/homebrew/bin/tiletool.py"  # PATH-resolved (ngdevkit), brew fallback
_BREWPY = ("/opt/homebrew/opt/python3/bin:") if _os.path.isdir("/opt/homebrew/opt/python3/bin") else ""

WAD = sys.argv[1] if len(sys.argv) > 1 else "doom1.wad"
MAP = sys.argv[2] if len(sys.argv) > 2 else "E1M1"
OUT = sys.argv[3] if len(sys.argv) > 3 else "src/map.c"

data = open(WAD, "rb").read()
magic, nlumps, dirofs = struct.unpack_from("<4sII", data, 0)
assert magic in (b"IWAD", b"PWAD"), "not a WAD: %r" % magic


def nm(b):
    return b.split(b"\0")[0].decode("ascii", "replace").upper()


lumps = []
for i in range(nlumps):
    pos, size = struct.unpack_from("<ii", data, dirofs + i * 16)
    lumps.append((nm(data[dirofs + i * 16 + 8: dirofs + i * 16 + 16]), pos, size))

gdir = {}
for n, pos, size in lumps:
    gdir[n] = (pos, size)                      # last wins, Doom-style


def glump(name):
    pos, size = gdir[name]
    return data[pos: pos + size]


mi = next(i for i, (n, _, _) in enumerate(lumps) if n == MAP)
sub = {n: (pos, size) for n, pos, size in lumps[mi + 1: mi + 11]}


def lump(name):
    pos, size = sub[name]
    return data[pos: pos + size]


VERT, LINE, SIDE, SECT, THIN, SEG, SSE, NOD = (
    lump(x) for x in ("VERTEXES", "LINEDEFS", "SIDEDEFS", "SECTORS",
                      "THINGS", "SEGS", "SSECTORS", "NODES"))

verts = [struct.unpack_from("<hh", VERT, o) for o in range(0, len(VERT), 4)]
sectors = [struct.unpack_from("<hh8s8shhh", SECT, o) for o in range(0, len(SECT), 26)]
sectors = [(fl, ce, max(0, min(255, li)), nm(ff), nm(cf)) for (fl, ce, ff, cf, li, sp, tg) in sectors]
sider = []
for o in range(0, len(SIDE), 30):
    xo, yo, up, lo, mid, sec = struct.unpack_from("<hh8s8s8sh", SIDE, o)
    sider.append((sec, xo, yo, nm(up), nm(lo), nm(mid)))
lines = []
for o in range(0, len(LINE), 14):
    v1, v2, fl, sp, tg, r, l = struct.unpack_from("<HHhhhHH", LINE, o)
    lines.append((v1, v2, fl, sp, -1 if r == 0xFFFF else r, -1 if l == 0xFFFF else l))
segs = [struct.unpack_from("<HHhHhh", SEG, o) for o in range(0, len(SEG), 12)]
segs = [(v1, v2, line, side, off) for (v1, v2, ang, line, side, off) in segs]
ssectors = [struct.unpack_from("<HH", SSE, o) for o in range(0, len(SSE), 4)]
ssectors = [(first, num) for (num, first) in ssectors]


def child(c):
    return -((c & 0x7FFF) + 1) if (c & 0x8000) else c


nodes = []
for o in range(0, len(NOD), 28):
    x, y, dx, dy = struct.unpack_from("<hhhh", NOD, o)
    rc, lc = struct.unpack_from("<HH", NOD, o + 24)
    nodes.append((x, y, dx, dy, child(rc), child(lc)))
things = [struct.unpack_from("<hhhh", THIN, o) for o in range(0, len(THIN), 10)]


def phash(name):
    h = 0
    for c in name.encode("ascii", "replace"):
        h = (h * 131 + c) & 0xff
    return h or 1


# ---- composite wall textures (PNAMES + TEXTURE1 + patches) ----
pn = glump("PNAMES"); npn = struct.unpack_from("<I", pn, 0)[0]
pnames = [nm(pn[4 + i * 8: 12 + i * 8]) for i in range(npn)]
t1 = glump("TEXTURE1"); ntx = struct.unpack_from("<I", t1, 0)[0]
toffs = struct.unpack_from("<%di" % ntx, t1, 4)
tex1_off = {nm(t1[toffs[i]: toffs[i] + 8]): (t1, toffs[i]) for i in range(ntx)}   # name -> (lump, offset)
# TEXTURE2 (registered/Ultimate DOOM: E2-E4's marble/skin/wood/gstone textures live HERE). Without it
# every TEXTURE2 wall is dropped from the bake AND every texid past the first one shifts vs vs_extract's
# full 245-wall enumerate -> mangled texture palettes even on E1M1. TEXTURE1 wins on dup names (DOOM rule).
try:
    t2 = glump("TEXTURE2"); ntx2 = struct.unpack_from("<I", t2, 0)[0]
    toffs2 = struct.unpack_from("<%di" % ntx2, t2, 4)
    for i in range(ntx2):
        tex1_off.setdefault(nm(t2[toffs2[i]: toffs2[i] + 8]), (t2, toffs2[i]))
except Exception:
    pass   # shareware doom1.wad has no TEXTURE2 -- E1-only build, unchanged


def parse_patch(b):
    w, h, lo, to = struct.unpack_from("<hhhh", b, 0)
    colofs = struct.unpack_from("<%dI" % w, b, 8)
    grid = [[-1] * w for _ in range(h)]
    for x in range(w):
        o = colofs[x]
        while True:
            td = b[o]
            if td == 255:
                break
            ln = b[o + 1]; o += 3
            for i in range(ln):
                y = td + i
                if 0 <= y < h:
                    grid[y][x] = b[o + i]
            o += ln + 1
    return w, h, lo, to, grid


def build_texture(lump, o):
    masked, w, h, coldir, pc = struct.unpack_from("<ihhiH", lump, o + 8)
    grid = [[-1] * w for _ in range(h)]
    po = o + 22
    for p in range(pc):
        ox, oy, pat, sd, cm = struct.unpack_from("<hhhhh", lump, po); po += 10
        pw, ph, plo, pto, pg = parse_patch(glump(pnames[pat]))
        for cx in range(pw):
            tx = ox + cx
            if 0 <= tx < w:
                for cy in range(ph):
                    ty = oy + cy
                    if 0 <= ty < h and pg[cy][cx] >= 0:
                        grid[ty][tx] = pg[cy][cx]
    return w, h, grid


# ALL-E1 ordering (shared with vs_extract via tools/vs_flats): every map indexes the SAME C-ROM
# tiles. Uppercase-canonical (DOOM textures are case-insensitive).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vs_flats import e1_assets, e1_flat_order
WALLS_E1, FLATS_E1, _ = e1_assets(WAD)
texlist = []; texid = {}
for name in WALLS_E1:
    if name in tex1_off:
        w, h, grid = build_texture(*tex1_off[name])
        texid[name] = len(texlist); texlist.append((w, h, grid, 0))


def tid(name):
    return -1 if name == "-" else texid.get(name.upper(), -1)


sides_out = [(sec, xo, yo, tid(up), tid(lo), tid(mid)) for (sec, xo, yo, up, lo, mid) in sider]

# flats (raw 64x64 palette indices) -> appended to the texture table. SHARED ORDER via
# e1_flat_order so the C-ROM flat index == flat_slot() == vsflatlut bank slot == vs_extract's
# per-seg slot id (one source of truth -> maps 2..9 index the right flat tiles).
flatid = {}
allflats, _flatslot = e1_flat_order(WAD)   # WAD-present, >=4096-gated, sorted (== flat_slot order)
for name in allflats:
    b = glump(name)
    grid = [[b[y * 64 + x] for x in range(64)] for y in range(64)]
    flatid[name] = len(texlist); texlist.append((64, 64, grid, 0))


def fid(name):
    return flatid.get(name.upper(), -1)


# Sky: the real sky graphic is the SKY1 *texture* (not a flat). F_SKY1 ceilings are
# flagged with ceiltex = -2 so the backend scrolls the sky instead of floor-casting.
SKY_TEX = -1
if "SKY1" in tex1_off:
    skw, skh, skg = build_texture(*tex1_off["SKY1"])
    SKY_TEX = len(texlist); texlist.append((skw, skh, skg, 0))

sec_out = [(fl, ce, li, fid(ff), -2 if cf == "F_SKY1" else fid(cf))
           for (fl, ce, li, ff, cf) in sectors]

# spawn sector via BSP descent
def pside(px, py, n):
    return 0 if n[2] * (py - n[1]) - n[3] * (px - n[0]) < 0 else 1


def find_ss(px, py):
    if not nodes:
        return 0
    idx = len(nodes) - 1
    while idx >= 0:
        idx = nodes[idx][4 + pside(px, py, nodes[idx])]
        if idx < 0:
            return -idx - 1
    return idx


# thing type (DoomEd number) -> (sprite name, mono?)  mono=1: single rotation (items/decor)
THINGSPR = {
    3004:("POSS",0), 9:("SPOS",0), 65:("CPOS",0), 3001:("TROO",0), 3002:("SARG",0),
    3005:("HEAD",0), 3006:("SKUL",0), 3003:("BOSS",0),
    2035:("BAR1",1),
    2001:("SHOT",1), 2002:("MGUN",1), 2003:("LAUN",1), 2004:("PLAS",1), 2005:("CSAW",1), 2006:("BFUG",1),
    2007:("CLIP",1), 2008:("SHEL",1), 2010:("ROCK",1), 2047:("CELL",1), 17:("CELP",1),
    2048:("AMMO",1), 2049:("SBOX",1), 2046:("BROK",1),
    2011:("STIM",1), 2012:("MEDI",1), 2013:("SOUL",1), 2014:("BON1",1), 2015:("BON2",1),
    2018:("ARM1",1), 2019:("ARM2",1), 2022:("PINV",1), 2023:("PSTR",1), 2024:("PINS",1),
    2025:("SUIT",1), 2026:("PMAP",1), 2045:("PVIS",1), 8:("BPAK",1),
    5:("BKEY",1), 6:("YKEY",1), 13:("RKEY",1), 40:("BSKU",1), 39:("YSKU",1), 38:("RSKU",1),
    2028:("COLU",1), 34:("CAND",1), 35:("CBRA",1),
    44:("TGRN",1), 45:("TBLU",1), 46:("TRED",1), 47:("SMIT",1), 48:("ELEC",1),
    30:("COL1",1), 31:("COL2",1), 32:("COL3",1), 33:("COL4",1),
}
spr_cache = {}


def get_sprite(lumpname):
    if lumpname in spr_cache:
        return spr_cache[lumpname]
    w, h, lo, to, grid = parse_patch(glump(lumpname))
    r = (len(texlist), w, h); texlist.append((w, h, grid, 1)); spr_cache[lumpname] = r
    return r


def sprite_for(t):
    if t not in THINGSPR:
        return None
    name, mono = THINGSPR[t]
    for c in [name + "A0", name + "A1", name + "A2A8"]:
        if c in gdir:
            return c
    return None


SKIP = {1, 2, 3, 4, 11}
start = None; th_out = []
for x, y, a, t in things:
    if t == 1:
        start = (x, y, a)
    if t in SKIP:
        continue
    spr = sprite_for(t)
    if not spr:
        continue
    tex, w, h = get_sprite(spr)
    th_out.append((x, y, (a * 256 // 360) & 0xff, t, tex, w, h))
sx, sy, sa = start if start else (0, 0, 0)
ssx = find_ss(sx, sy); fseg, _ = ssectors[ssx]; ld = lines[segs[fseg][2]]
fsd = ld[4] if segs[fseg][3] == 0 else ld[5]
startsec = sides_out[fsd][0] if fsd >= 0 else 0
startz = sectors[startsec][0] + 41

J = lambda fmt, rows: ",".join(fmt % r for r in rows)
out = [
    '/* GENERATED by tools/wad2c.py from %s : %s. Do not edit by hand. */' % (WAD, MAP),
    '#include "dng.h"',
    'static const vec2 V[]={' + J("{%d,%d}", verts) + '};',
    'static const sector_t SE[]={' + J("{%d,%d,%d,%d,%d}", sec_out) + '};',
    'static const sidedef_t SD[]={' + J("{%d,%d,%d,%d,%d,%d}", sides_out) + '};',
    'static const linedef_t L[]={' + J("{%d,%d,%d,%d,%d,%d}", lines) + '};',
    'static const seg_t G[]={' + J("{%d,%d,%d,%d,%d}", segs) + '};',
    'static const subsector_t SS[]={' + J("{%d,%d}", ssectors) + '};',
    'static const node_t N[]={' + ",".join("{%d,%d,%d,%d,{%d,%d}}" % n for n in nodes) + '};',
    'static const thing_t T[]={' + J("{{%d,%d},%d,%d,%d,%d,%d}", th_out) + '};',
    'static const level_t LV={ V,%d, SE,%d, SD,%d, L,%d, G,%d, SS,%d, N,%d, T,%d };' % (
        len(verts), len(sec_out), len(sides_out), len(lines),
        len(segs), len(ssectors), len(nodes), len(th_out)),
    'const level_t *map_load(void){ return &LV; }',
    'camera_t MAP_START={ {%d,%d}, %d, %d.0f, %d, 0 };' % (sx, sy, (sa * 256 // 360) & 0xff, startz, startsec),
    'const int SKY_TEX=%d;' % SKY_TEX,
]
# Rotation LUT: each vertex's rotated coords for NA=128 camera angles (int16). Makes
# to_view a lookup+subtract instead of 4 multiplies -- the "finite angles" precompute.
import math
NA = 128
rotd = []; rots = []
for a in range(NA):
    th = a * 2 * math.pi / NA; c = math.cos(th); s = math.sin(th)
    for (vx, vy) in verts:
        rotd.append(int(round(vx * c + vy * s)))
        rots.append(int(round(vx * s - vy * c)))
out.append('const int ROT_NV=%d, ROT_NA=%d;' % (len(verts), NA))
out.append('const short ROTD[]={' + ",".join("%d" % x for x in rotd) + '};')
out.append('const short ROTS[]={' + ",".join("%d" % x for x in rots) + '};')
open(OUT, "w").write("\n".join(out) + "\n")

# Quantize each texture to <=16 colours -- the Neo Geo's per-sprite palette limit.
# Doom textures each use a limited palette, so they survive this. THIS is how the gritty
# textured look fits the hardware: every texture keeps its own 16-colour palette.
from collections import Counter
playpal = glump("PLAYPAL")
PALRGB = [(playpal[i*3], playpal[i*3+1], playpal[i*3+2]) for i in range(256)]
def quantize16(grid):
    cnt = Counter(p for row in grid for p in row if p >= 0)
    if len(cnt) <= 15:
        return
    # MEDIAN-CUT: spread 15 representative colours across the texture's actual colour RANGE.
    # The old most_common(15) kept only the most frequent colours and snapped everything else to
    # the nearest -- so smooth gradients (STARTAN shading) collapsed into hard bands (the vertical
    # stripes), and small bright accents (posters, status lights) got crushed to a dull neighbour.
    # Median-cut keeps the range, so gradients stay smooth and bright accents survive.
    def axrange(b):
        return [max(p[a] for p in b) - min(p[a] for p in b) for a in range(3)]
    pts = [[PALRGB[i][0], PALRGB[i][1], PALRGB[i][2], cnt[i], i] for i in cnt]
    boxes = [pts]
    while len(boxes) < 15:
        si, best = -1, -1
        for k, b in enumerate(boxes):
            if len(b) < 2: continue
            v = max(axrange(b)) * sum(p[3] for p in b)   # widest * heaviest box splits first
            if v > best: best, si = v, k
        if si < 0: break
        b = boxes.pop(si); ax = axrange(b).index(max(axrange(b)))
        b.sort(key=lambda p: p[ax])
        tot = sum(p[3] for p in b); acc = 0; cut = 1
        for k in range(len(b)):
            acc += b[k][3]
            if acc * 2 >= tot:
                cut = max(1, min(len(b) - 1, k + 1)); break
        boxes.append(b[:cut]); boxes.append(b[cut:])
    rep = {}
    for b in boxes:
        tw = sum(p[3] for p in b) or 1
        cr = sum(p[0]*p[3] for p in b)/tw; cg = sum(p[1]*p[3] for p in b)/tw; cb = sum(p[2]*p[3] for p in b)/tw
        ridx = min(b, key=lambda p: (p[0]-cr)**2 + (p[1]-cg)**2 + (p[2]-cb)**2)[4]   # box rep = colour nearest its weighted centroid
        for p in b: rep[p[4]] = ridx
    for row in grid:
        for x in range(len(row)):
            if row[x] >= 0:
                row[x] = rep[row[x]]
reduced = 0
for (w, h, grid, masked) in texlist:
    before = len(set(p for row in grid for p in row if p >= 0))
    quantize16(grid)
    if before > 16:
        reduced += 1
print("quantized %d textures to <=16 colours (of %d)" % (reduced, len(texlist)))

# binary crom: palette + textures
blob = bytearray(); dr = []
for (w, h, grid, masked) in texlist:
    dr.append((w, h, len(blob)))
    _dom = (Counter(p for r in grid for p in r if p >= 0).most_common(1) or [(0,)])[0][0]   # dominant Doom colour for opaque hole-fill
    for row in grid:
        for v in row:
            if masked:
                blob.append(255 if v < 0 else (254 if v == 255 else v))   # 255 = transparent
            else:
                blob.append(v if v >= 0 else _dom)   # opaque fill (was 0=black hole) -> matches the cart's now-opaque walls
crom = bytearray(glump("PLAYPAL")[:768])
crom += struct.pack("<I", len(texlist))
for (w, h, ofs) in dr:
    crom += struct.pack("<hhI", w, h, ofs)
crom += blob
crom_path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(OUT))), "e1m1.crom")
open(crom_path, "wb").write(crom)

# Per-texture dominant colour -> Neo Geo RGB444, for the cartridge's flat-colour
# palettes (each wall/flat shows its texture's real Doom colour, not a colour-by-kind).
def dom444(grid):
    cnt = Counter(p for row in grid for p in row if p >= 0)
    if not cnt:
        return 0
    r, g, b = PALRGB[cnt.most_common(1)[0][0]]
    return ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)
texcols = [dom444(grid) for (w, h, grid, masked) in texlist]
texpal_path = os.path.join(os.path.dirname(crom_path), "neogeo", "texpal.h")
with open(texpal_path, "w") as f:
    f.write("/* generated by wad2c.py -- per-texture dominant colour (Neo Geo RGB444) */\n")
    f.write("#define NTEX %d\n" % len(texcols))
    f.write("static const unsigned short TEXCOL[NTEX] = {\n")
    for i in range(0, len(texcols), 12):
        f.write("  " + ",".join("0x%04X" % c for c in texcols[i:i+12]) + ",\n")
    f.write("};\n")
print("wrote %s (%d texture colours)" % (texpal_path, len(texcols)))

# ---- Bake each texture's real pixels into Neo Geo C-ROM 16x16 tiles (via tiletool),
#      with its own 16-colour palette. This is the per-texel detail (real Doom textures). ----
import subprocess
from PIL import Image
ngdir = os.path.join(os.path.dirname(crom_path), "neogeo")
tmpd = "/tmp/dngtex"; os.makedirs(tmpd, exist_ok=True)
tiles_c1 = bytearray(); tiles_c2 = bytearray()
tbase = []; twt = []; tht = []; tpal16 = []; tilebase = 0; fogsrc = []
NSHIFT = 16  # horizontal sub-tile phases per texture: 1-TEXEL granularity (was 8/2-texel). Textiles+ramps x2 in C-ROM -- afforded by the symmetry/mirror canonicalization savings. Visibly sharper wall crops.
_ttenv = dict(os.environ); _ttenv["PATH"] = _BREWPY + _ttenv.get("PATH", "")
def remap15(used):
    """opaque Doom-palette index -> nearest KEPT colour's local index (1..15); never index 0,
       so opaque pixels can't punch transparent (black) holes the way local.get(v,0) did."""
    if not used: used = [0]
    loc = {idx: i + 1 for i, idx in enumerate(used)}
    rm = [0] * 256
    for c in range(256):
        cr, cg, cb = PALRGB[c]; best = used[0]; bd = 1 << 30
        for u in used:
            ur, ug, ub = PALRGB[u]; d = (cr-ur)**2 + (cg-ug)**2 + (cb-ub)**2
            if d < bd: bd = d; best = u
        rm[c] = loc[best]
    return loc, rm

def _texpx(t):
    """recompute the 16-colour remapped pixel buffer + palette for texture t (matches the textile loop)."""
    w, h, grid, masked = texlist[t]
    used = [idx for idx, _ in Counter(p for r in grid for p in r if p >= 0).most_common(15)]
    local, remap = remap15(used)
    pal16 = [0]*16
    for idx, i in local.items():
        r, g, b = PALRGB[idx]; r5, g5, b5 = r>>3, g>>3, b>>3
        pal16[i] = ((r5&1)<<14)|((g5&1)<<13)|((b5&1)<<12)|((r5>>1)<<8)|((g5>>1)<<4)|(b5>>1)
    palflat = sum([[((pal16[i]>>8)&0xF)*17, ((pal16[i]>>4)&0xF)*17, (pal16[i]&0xF)*17] for i in range(16)], [])
    wp = ((w+15)//16)*16; hp = ((h+15)//16)*16
    _fill = local.get(used[0], 1) if used else 1   # opaque fill for uncovered areas (walls must be opaque; see textile loop)
    px = bytearray([_fill]) * (wp*hp) if not masked else bytearray(wp*hp)
    for y in range(h):
        row = grid[y]
        for x in range(w):
            if row[x] >= 0: px[y*wp+x] = remap[row[x]]
    return px, palflat, wp, hp

PLAYPAL_RGB=[tuple(playpal[i*3:i*3+3]) for i in range(256)]
# === HUD FIX LAYER: the status bar (with static demo numbers) + 4 face expressions as 8x8
#     fix tiles. Composited offline pixel-identically to the old sprite HUD, each cell
#     quantized to the best of 4 fix palettes (slots 2-5). Drawn ONCE at init by the cart:
#     frees ~30 hardware sprites for world records. Pistol/flash stay sprites (they bob).
#     Runs in the MAIN pass only (not --ramps). ===
def _hudfix_run():
    bar_w, bar_h = 320, 32
    def glyph_cols(names):
        cols = {}
        for g in names:
            try: pw, ph, lo, to, pg = parse_patch(glump(g))
            except Exception: continue
            for row in pg:
                for v in row:
                    if v >= 0: cols[v] = cols.get(v, 0) + 1
        return cols
    TTNUM_COLS = glyph_cols(["STTNUM%d" % d for d in range(10)] + ["STTPRCNT"])   # the big red digit font's EXACT colours (the old r>1.3g chroma filter also passed STBAR browns and dragged innocent grey cells into the red palette)
    def compose(face_lump):
        img = [[0]*bar_w for _ in range(bar_h)]
        dmask = [[False]*40 for _ in range(4)]                     # 8x8 cells touched by BIG-DIGIT blits (positional, not chromatic)
        def blit(name, dx, dy, opaque=0, mark=0):
            try: pw, ph, lo, to, pg = parse_patch(glump(name))
            except Exception: return
            if mark:
                for cy in range(max(0,dy//8), min(4,(dy+ph+7)//8)):
                    for cx in range(max(0,dx//8), min(40,(dx+pw+7)//8)): dmask[cy][cx] = True
            for y in range(ph):
                for x in range(pw):
                    v = pg[y][x]
                    if v < 0:
                        if not opaque: continue
                        v = 0
                    if 0 <= dy+y < bar_h and 0 <= dx+x < bar_w: img[dy+y][dx+x] = v
        blit("STBAR", 0, 0, opaque=1)
        blit("STARMS", 104, 0, opaque=1)                           # single-player ARMS panel over the raw lump's FRAG area (vanilla ST_refreshBackground does the same)
        def num(xr, y, val, pct):                                  # big red numerals, right-justified, 12px advance
            x = xr - 14
            if pct: blit("STTPRCNT", x, y, mark=1); x -= 12
            if val <= 0: blit("STTNUM0", x, y, mark=1); return
            while val > 0:
                blit("STTNUM%d" % (val % 10), x, y, mark=1); val //= 10; x -= 12
        def ynum(xr, y, val):                                      # small yellow numerals, right-justified at xr, 4px advance (the ammo table -- the runtime draws these in vanilla, so STBAR alone never contains them)
            if val <= 0: blit("STYSNUM0", xr-4, y); return
            x = xr - 4
            while val > 0:
                blit("STYSNUM%d" % (val % 10), x, y); val //= 10; x -= 4
        num(46, 3, 50, 0); num(103, 3, 100, 1); num(233, 3, 0, 1)
        for i, (cur, mx) in enumerate(((50,200),(0,50),(0,50),(0,300))):   # BULL/SHEL/ROKT/CELL at pistol start
            ynum(288, 5+6*i, cur); ynum(314, 5+6*i, mx)
        blit("STYSNUM2", 111, 4)                                   # ARMS: weapon 2 owned (yellow), 3-7 unowned (grey)
        for i in range(1, 6):
            blit("STGNUM%d" % (i+2), 111+(i%3)*12, 4+(i//3)*10)
        if face_lump: blit(face_lump, 148, 0, opaque=0)   # MASK the face: its transparent pixels show STBAR's natural dark-grey (30,30,30) recessed slot, NOT opaque-black and NOT the wider slot-fill box that read as "cropped in too tight"
        return img, dmask
    composed = [compose(f) for f in ("STFST00", "STFST02", "STFST01", "STFEVL0")]
    bars = [c[0] for c in composed]
    globals()['_HUDBAR'] = bars[0]   # composed status bar (face frame 0) -> the sprite bake reads its right-edge 16px for the full-width HUD edge sprite (the cropped yellow ammo numerals)
    def cells_of(img):
        return [tuple(img[cy*8+y][cx*8+x] for y in range(8) for x in range(8))
                for cy in range(4) for cx in range(40)]
    cell_sets = [cells_of(b) for b in bars]
    masked_content = set()                                         # a content tuple is digit-bound if ANY instance sits under a big-digit blit
    for (img, dmask), cs in zip(composed, cell_sets):
        for cy in range(4):
            for cx in range(40):
                if dmask[cy][cx]: masked_content.add(cs[cy*40+cx])
    allcells = []
    for cs in cell_sets: allcells += cs
    from collections import Counter
    def cell_counter(cells):
        cc = Counter()
        for cell in cells:
            for v in cell:
                if v: cc[v] += 1
        return cc
    NPAL = 5                                                       # fix palettes 2..6: bucket 0 is the DIGIT palette
    uniq = sorted(set(allcells), key=lambda c: -len(set(x for x in c if x)))
    digit_pin = [c for c, _ in Counter(TTNUM_COLS).most_common(8)]   # pin fewer reds (8 covers the STTNUM shades) -> leaves more of the 14 slots for the STBAR recessed-slot GREYS so the digit cells' background matches the bar (was 12 -> flat/mismatched grey box behind the numerals)
    groups = [set() for _ in range(NPAL)]; assign = {}
    groups[0] = set(digit_pin)
    dig_cells = [c for c in uniq if c in masked_content]
    cc0 = cell_counter(dig_cells)                                   # grow the digit palette with the greys/browns that actually co-reside under digits (kills the red-speckle crush below the big numerals)
    for c, _ in cc0.most_common():
        if len(groups[0]) >= 14: break
        if c not in groups[0]: groups[0].add(c)
    for cell in dig_cells: assign[cell] = 0
    for cell in uniq:
        if cell in assign: continue
        cols = set(x for x in cell if x)
        best, bestkey = None, None
        for gi in range(1, NPAL):
            g = groups[gi]
            inter = len(g & cols); grow = len(cols - g)
            if len(g | cols) <= 14:
                key = (-inter, grow)
                if bestkey is None or key < bestkey: best, bestkey = gi, key
        if best is None:
            best = max(range(1, NPAL), key=lambda gi: len(groups[gi] & cols))
            room = 14 - len(groups[best])
            if room > 0:
                cc = Counter(x for x in cell if x and x not in groups[best])
                groups[best] |= set(c for c, _ in cc.most_common(room))
        else:
            groups[best] |= cols
        assign[cell] = best
    def cell_err(cell, pal):                                       # quantization error of a cell under a palette (sum of nearest-colour distances)
        e = 0
        for v in cell:
            if not v: continue
            pr, pgc, pb = PLAYPAL_RGB[v]
            bd = min(((PLAYPAL_RGB[c][0]-pr)**2 + (PLAYPAL_RGB[c][1]-pgc)**2 + (PLAYPAL_RGB[c][2]-pb)**2) for c in pal) if pal else 10**9
            e += bd
        return e
    SMALL_COLS = [c for c, _ in Counter(glyph_cols(["STYSNUM%d" % d for d in range(10)] + ["STGNUM%d" % d for d in range(10)])).most_common(8)]   # small digit font colours (ammo table + ARMS): too few pixels to survive frequency ranking, so pin them
    sb = max(range(1, NPAL), key=lambda gi: len(groups[gi] & set(SMALL_COLS)))
    for _ in range(2):                                             # refine: rebuild palettes from membership, reassign by min error (the one-shot greedy left label cells crushed -> dim/eroded text, tan ghost leakage)
        for gi in range(1, NPAL):
            members = [c for c in uniq if assign[c] == gi]
            keep = 14 - (len(SMALL_COLS) if gi == sb else 0)
            groups[gi] = set(c for c, _ in cell_counter(members).most_common(keep))
            if gi == sb: groups[gi] |= set(SMALL_COLS)
        members0 = [c for c in uniq if assign[c] == 0]
        g0 = set(digit_pin)
        for c, _ in cell_counter(members0).most_common():
            if len(g0) >= 14: break
            g0.add(c)
        groups[0] = g0
        for cell in uniq:
            if cell in masked_content: continue                    # digit cells stay pinned to the digit palette
            errs = [(cell_err(cell, sorted(groups[gi])), gi) for gi in range(1, NPAL)]
            assign[cell] = min(errs)[1]
    pal_list = [sorted(g) for g in groups]
    pal_idx = [{c: i+1 for i, c in enumerate(p)} for p in pal_list]
    def nearest(gi, c):
        p = pal_list[gi]
        if not p: return 1
        pr, pgc, pb = PLAYPAL_RGB[c]
        bi, bd = 1, 1e18
        for i, cc in enumerate(p):
            r, g, b = PLAYPAL_RGB[cc]
            d = (r-pr)**2 + (g-pgc)**2 + (b-pb)**2
            if d < bd: bd, bi = d, i+1
        return bi
    def enc_tile(cell, gi):
        px = [15 if c == 0 else (pal_idx[gi].get(c) or nearest(gi, c)) for c in cell]   # index 15 = opaque black: the bar must fully cover the world (index 0 would let it bleed through)
        out = bytearray(32); k = 0
        for xa, xb in ((4,5),(6,7),(0,1),(2,3)):
            for y in range(8):
                out[k] = (px[8*y+xa] & 0xF) | ((px[8*y+xb] & 0xF) << 4); k += 1
        return bytes(out)
    tiles = []; tmap = {}
    def tile_id(cell, gi):
        key = (cell, gi)
        if key not in tmap:
            tmap[key] = len(tiles); tiles.append(enc_tile(cell, gi))
        return tmap[key]
    FIXBASE = 1280
    maps = []
    for cs in cell_sets:
        maps.append([(((assign[c])+2) << 12) | (FIXBASE + tile_id(c, assign[c])) for c in cs])
    open(os.path.join(ngdir, "hudfix.fix"), "wb").write(b"".join(tiles))
    def ngc(c):
        r, g, b = PLAYPAL_RGB[c]
        r5, g5, b5 = r >> 3, g >> 3, b >> 3
        return ((r5 & 1) << 14) | ((g5 & 1) << 13) | ((b5 & 1) << 12) | ((r5 >> 1) << 8) | ((g5 >> 1) << 4) | (b5 >> 1)
    with open(os.path.join(ngdir, "hudfix.h"), "w") as f:
        f.write("/* generated by wad2c.py -- status bar + 4 face expressions as fix-layer tiles */\n")
        f.write("#define HUDFIX_BASE %d\n#define HUDFIX_NTILES %d\n" % (FIXBASE, len(tiles)))
        for pi, p in enumerate(pal_list):
            vals = [0x8000] + [ngc(c) for c in p] + [0]*(15-len(p))
            vals[15] = ngc(0)                                   # slot 15 = opaque DOOM black
            f.write("static const unsigned short HUDFIX_PAL%d[16]={%s};\n" % (pi+2, ",".join(str(v) for v in vals)))
        f.write("static const unsigned short HUDFIX_MAP[160]={%s};\n" % ",".join(str(w) for w in maps[0]))
        for fi in range(4):
            cells = [maps[fi][cy*40+cx] for cy in range(4) for cx in range(18, 22)]
            f.write("static const unsigned short HUDFIX_FACE%d[16]={%s};\n" % (fi, ",".join(str(w) for w in cells)))
    print("hudfix: %d tiles, palettes %s -> neogeo/hudfix.{fix,h}" % (len(tiles), [len(p) for p in pal_list]))
if "--ramps" not in sys.argv:
    _hudfix_run()

if "--ramps" in sys.argv:
    # === texture-baked smooth-wall edge tiles: a normal edge-row tile with pixels past a
    #     diagonal zeroed (index 0 = transparent), so the wall silhouette is pixel-smooth.
    #     Usage-limited by the host bake's manifest (ramps_used.txt). TOP edge only for now. ===
    def _tilecount(p):
        try: return os.path.getsize(os.path.join(ngdir, p)) // 64
        except OSError: return 0
    RAMP_DMIN, RAMP_DMAX = -32, 32; RAMP_NDROP = RAMP_DMAX - RAMP_DMIN + 1   # TUNED: drop capped +/-32 (up to 2-tile stacks) -> no tall jaggy caps, smaller C-ROM
    RAMP_TILE0 = 257 + _tilecount("textiles.c1") + _tilecount("floorlut.c1") + _tilecount("sprites.c1") + _tilecount("ceillut.c1")
    mpath = os.path.join(os.path.dirname(ngdir), "ramps_used.txt")
    combos = []
    for ln in open(mpath):
        ln = ln.strip()
        if not ln or ln[0] == "#": continue
        t, d, e = map(int, ln.split())
        combos.append((t, d, e))                   # e: 0=top edge, 1=bottom edge
    # MIRROR CANONICALIZATION: the +d and -d alpha masks are exact horizontal mirrors
    # (el(xi) == el_mirror(15-xi)), and SCB1 has a per-tile h-flip bit. Bake only the
    # NEGATIVE slope of each |d| and let the cart flip for d>0: silhouette pixel-exact,
    # texel content mirrored within the cap row (sub-perceptual at 16px strip scale).
    # Census measured: 1781 signed combos -> 944 canonical = ramps C-ROM nearly halved.
    # VFLIP MERGE: bottom(d) == hvflip(top(d)) EXACTLY (integer-verified per stack tile), so only
    # TOP stacks are baked. The cart derives all four cases from the per-tile flip bits:
    # top(-d)=00, top(+d)=H, bottom(+d)=V, bottom(-d)=HV. 1781 signed combos -> 490 top stacks.
    combos = sorted({(t, -abs(d), 0) for (t, d, e) in combos})
    from collections import defaultdict
    bytex = defaultdict(set)
    for (t, d, e) in combos: bytex[t].add((d, e))
    ramp_off = [[[-1, -1] for _ in range(RAMP_NDROP)] for _ in range(len(texlist))]
    ramp_c1 = b""; ramp_c2 = b""; base = 0
    for t in sorted(bytex):
        px, palflat, wp, hp = _texpx(t); wt = wp // 16; ht = max(1, hp // 16)
        items = sorted(bytex[t])
        nts = [max(1, (abs(d)+15)//16) for (d, e) in items]   # stack height per combo (1 tile per 16px of drop)
        total_rows = sum(NSHIFT*nt for nt in nts)             # sheet tile-rows: each combo = NSHIFT phases x nt stack tiles
        sheet = bytearray(wp * total_rows * 16)
        rowoff = 0
        for bi, (d, e) in enumerate(items):
            ad = abs(d); nt = nts[bi]
            for s in range(NSHIFT):
                sh = s * (16 // NSHIFT)
                for k in range(nt):                            # stack tile k (k=0 nearest the corner)
                    band = rowoff + s*nt + k
                    srow0 = ((k % ht) if e == 0 else ((ht-1-k) % ht)) * 16   # top stack walks rows 0.. ; bottom walks rows th-1..
                    for y in range(16):
                        dst = (band*16 + y) * wp
                        for x in range(wp):
                            v = px[(srow0 + y)*wp + ((x+sh) % wp)]
                            xi = x & 15
                            if e == 0:                         # top: transparent ABOVE the edge, minus this tile's offset
                                el = ((ad*xi)//15 if d >= 0 else (ad*(15-xi))//15) - k*16
                                trans = y < el
                            else:                              # bottom: transparent BELOW the edge
                                elb = (k+1)*16 - ((ad*(15-xi))//15 if d >= 0 else (ad*xi)//15)
                                trans = y >= elb
                            sheet[dst + x] = 0 if trans else v
            for _e in (0, 1):                                          # bottoms share the top stack (cart sets v-flip)
                ramp_off[t][d - RAMP_DMIN][_e] = base + rowoff * wt
                if -d <= RAMP_DMAX: ramp_off[t][-d - RAMP_DMIN][_e] = base + rowoff * wt
            rowoff += NSHIFT * nt
        img = Image.frombytes('P', (wp, total_rows*16), bytes(sheet)); img.putpalette(palflat)
        gifp = "%s/ramp_%d.gif" % (tmpd, t); img.save(gifp, optimize=False)
        c1p = "%s/ramp_%d.c1" % (tmpd, t); c2p = "%s/ramp_%d.c2" % (tmpd, t)
        subprocess.run([_TILETOOL, "--sprite", "-c", gifp, "-o", c1p, c2p], check=True, env=_ttenv)
        c1d = open(c1p, 'rb').read(); c2d = open(c2p, 'rb').read()
        ramp_c1 += c1d; ramp_c2 += c2d; base += len(c1d) // 64
    # === OPAQUE-CORNER cap OVERLAY wedges (NEW cap mode TB32o) ================================
    # The in-strip caps bevel a sloped edge by baking the corner PAST the diagonal as index 0
    # (HARDWARE-TRANSPARENT) -> the floor/ceiling LUT shows through, but the LUT is on a 16px
    # grid that can't follow the diagonal -> a stepped "picket fence" seam. The OPAQUE-CORNER
    # mode leaves the wall strip plain (no in-strip caps) and instead lays a SEPARATE overlay
    # sprite over the wall's top/bottom corner: the overlay is SOLID (index 1) ABOVE the
    # diagonal and TRANSPARENT (index 0) below it -- the exact INVERSE of the in-strip cap.
    # Drawn with a cap palette whose index 1 = the mean ceiling colour (top) / floor colour
    # (bottom), the solid wedge MERGES with the ceiling/floor so the wall silhouette reads as a
    # clean diagonal with nothing showing through.  PER-SLOPE only (16px-wide; the mask repeats
    # every 16px -> reused for every tcol; texture-INdependent; no NSHIFT phases -- the solid/
    # transparent mask does not depend on the wall's horizontal U-shift). Same -|d| canonical
    # baking + flip derivation as the in-strip caps (top(-d)=00, top(+d)=H, bottom(+d)=V,
    # bottom(-d)=HV), so the cart reuses rtfl/rbfl verbatim. Appended AFTER the ramps + before
    # the AVG mip -> the AVG/TITLE/MENU TILE0s (which sum 'base'/'abase') re-anchor on re-bake.
    RAMP_OVL_TILE0 = RAMP_TILE0 + base
    ramp_ovl_off = [-1]*RAMP_NDROP            # overlay tile-offset (from RAMP_OVL_TILE0) per quantized slope; -1 = none
    ovl_base = 0
    ovl_slopes = sorted({ -abs(d) for (t, d, e) in combos })   # canonical negative |d| (combos already canonicalized to (-|d|,0))
    ovl_c1 = b""; ovl_c2 = b""
    for d in ovl_slopes:                       # d<=0; bake the NEGATIVE-slope TOP wedge, cart flips for +d / bottom
        ad = abs(d); nt = max(1, (ad+15)//16)  # stack height: 1 tile per 16px of drop (matches the cap stack)
        sheet = bytearray(16 * nt * 16)        # nt stacked 16x16 tiles (k=0 nearest the corner)
        for k in range(nt):
            for y in range(16):
                for x in range(16):
                    xi = x & 15
                    el = (ad*(15-xi))//15 - k*16   # d<0 branch of the cap's top 'el'; SOLID where the cap is TRANSPARENT (y<el)
                    sheet[(k*16 + y)*16 + x] = 1 if y < el else 0
        ovlpal = [0,0,0, 255,255,255] + [0]*42  # grey index-1 placeholder (recoloured by the cart cap palette at draw)
        img = Image.frombytes('P', (16, nt*16), bytes(sheet)); img.putpalette(ovlpal)
        gifp = "%s/rampovl_%d.gif" % (tmpd, ad); img.save(gifp, optimize=False)
        c1p = "%s/rampovl_%d.c1" % (tmpd, ad); c2p = "%s/rampovl_%d.c2" % (tmpd, ad)
        subprocess.run([_TILETOOL, "--sprite", "-c", gifp, "-o", c1p, c2p], check=True, env=_ttenv)
        c1d = open(c1p, 'rb').read(); c2d = open(c2p, 'rb').read()
        ovl_c1 += c1d; ovl_c2 += c2d
        ramp_ovl_off[d - RAMP_DMIN] = ovl_base
        if -d <= RAMP_DMAX: ramp_ovl_off[-d - RAMP_DMIN] = ovl_base   # +d shares the wedge (cart h-flips)
        ovl_base += len(c1d) // 64
    ramp_c1 += ovl_c1; ramp_c2 += ovl_c2; base += ovl_base
    # === cap PALETTES: index 1 = mean CEILING flat colour (top caps) / mean FLOOR flat colour
    #     (bottom caps). Mean RGB over E1's distinct ceiling/floor flats' raw pixels -> NG RGB444.
    #     Index 0 stays transparent; indices 2..15 a dark ramp toward index 1 (unused by the wedge
    #     -- the wedge is index 0/1 only -- but kept sensible). Emitted as CAP_CEIL_PAL/CAP_FLOOR_PAL.
    _ceil_flats, _floor_flats = set(), set()
    for (fl, ce, li, ff, cf) in sectors:
        if ff: _floor_flats.add(ff.upper())
        if cf and cf != "F_SKY1": _ceil_flats.add(cf.upper())
    def _mean_flat_rgb(names):
        rs = gs = bs = n = 0
        for nm2 in names:
            fi = flatid.get(nm2, -1)
            if fi < 0: continue
            _w, _h, grid, _m = texlist[fi]
            for row in grid:
                for idx in row:
                    if idx < 0: continue
                    r, g, b = PALRGB[idx]; rs += r; gs += g; bs += b; n += 1
        if n == 0: return (96, 96, 96)
        return (rs//n, gs//n, bs//n)
    def _cap_pal16(rgb):                        # 16-colour NG palette: 0 transparent, 1 = flat colour, 2..15 dark ramp toward it
        r, g, b = rgb
        out = [0x8000]                          # index 0: transparent (bit15)
        def _ng(r2, g2, b2):
            r5, g5, b5 = r2 >> 3, g2 >> 3, b2 >> 3
            return ((r5 & 1) << 14) | ((g5 & 1) << 13) | ((b5 & 1) << 12) | ((r5 >> 1) << 8) | ((g5 >> 1) << 4) | (b5 >> 1)
        out.append(_ng(r, g, b))                # index 1: the representative flat colour (the ONLY one the wedge uses)
        for i in range(2, 16):                  # 2..15: a dark ramp (sensible filler; unused by the index-0/1 wedge)
            f = (15 - i) / 13.0
            out.append(_ng(int(r*f), int(g*f), int(b*f)))
        return out
    CAP_CEIL_PAL = _cap_pal16(_mean_flat_rgb(_ceil_flats))
    CAP_FLOOR_PAL = _cap_pal16(_mean_flat_rgb(_floor_flats))
    print("cap overlay: OVL_TILE0=%d, %d wedge tiles; ceil=%s floor=%s" % (
        RAMP_OVL_TILE0, ovl_base, _mean_flat_rgb(_ceil_flats), _mean_flat_rgb(_floor_flats)))
    # === LOD vertical-average mip ============================================================
    # A 1-tile-tall strip per texture where each texel COLUMN is the vertical AVERAGE of that
    # texture column (mean RGB -> nearest palette colour). Distant walls collapse to a single
    # tile (cot==1); the base path then samples srow=0 (the texture's TOP row), which aliases to
    # dark/garbage "boxes". This pre-averaged strip gives a representative colour with NO vertical
    # aliasing. Baked per-phase (wp x 16) so it shares the cart's `shift*wt + tcol` addressing.
    # Appended after the ramps -> no downstream base shift.
    AVG_TILE0 = RAMP_TILE0 + base
    avg_off = [0]*len(texlist); abase = 0
    for t in range(len(texlist)):
        px, palflat, wp, hp = _texpx(t); wt = wp // 16
        avgcol = bytearray(wp)
        for x in range(wp):                                   # vertical mean of column x -> nearest palette idx
            rs=gs=bs=n=0
            for y in range(hp):
                idx = px[y*wp+x]
                if idx == 0: continue                         # skip transparent (masked) texels
                rs+=palflat[idx*3]; gs+=palflat[idx*3+1]; bs+=palflat[idx*3+2]; n+=1
            if n == 0: avgcol[x]=0; continue
            rs//=n; gs//=n; bs//=n
            best=1; bestd=1<<30
            for i in range(1,16):
                dr=palflat[i*3]-rs; dg=palflat[i*3+1]-gs; db=palflat[i*3+2]-bs
                d=dr*dr+dg*dg+db*db
                if d<bestd: bestd=d; best=i
            avgcol[x]=best
        avg_off[t] = abase
        for s in range(NSHIFT):                               # NSHIFT phases, each a wp x 16 strip (matches base layout)
            sh = s * (16 // NSHIFT)
            strip = bytearray(wp*16)
            for y in range(16):
                row0 = y*wp
                for x in range(wp): strip[row0+x] = avgcol[(x+sh) % wp]
            img = Image.frombytes('P', (wp, 16), bytes(strip)); img.putpalette(palflat)
            gifp = "%s/avg_%d_%d.gif" % (tmpd, t, s); img.save(gifp, optimize=False)
            c1p = "%s/avg_%d_%d.c1" % (tmpd, t, s); c2p = "%s/avg_%d_%d.c2" % (tmpd, t, s)
            subprocess.run([_TILETOOL, "--sprite", "-c", gifp, "-o", c1p, c2p], check=True, env=_ttenv)
            c1d = open(c1p,'rb').read(); c2d = open(c2p,'rb').read()
            ramp_c1 += c1d; ramp_c2 += c2d; abase += len(c1d) // 64
    print("LOD avg-mip: TILE0=%d, %d tiles (%d KB)" % (AVG_TILE0, abase, len(ramp_c1)//1024 - len(ramp_c2)//1024))
    # === TITLEPIC: the DOOM title screen as a 20x13 (320x208) sprite tilemap. Each 16x16 tile gets its
    #     OWN 15-colour palette (NG per-tile palette = near-original quality); index 0 is left transparent
    #     so tiles are fully opaque (colours in 1..15). Tile DATA is palette-agnostic indices, so the whole
    #     image bakes as ONE tiletool pass; the cart applies TITLE_MAP[row][col] palettes at title time and
    #     restores the game palettes after. Appended after the avg-mip -> no downstream base shift. ===
    import numpy as _np
    TITLE_TILE0 = AVG_TILE0 + abase; TCOLS, TROWS = 20, 14; _tg = None   # TROWS=14 -> 224px active area; the title is baked NATIVE + vertically CENTRED (no stretch) -> letterbox top/bottom (see below)
    try: _tw,_th,_tlo,_tto,_tg = parse_patch(glump("TITLEPIC"))
    except Exception as _e: print("TITLEPIC: not found (%s) -> cart falls back to text title" % _e)
    def _ngc(r,g,b):
        R,G,B=r>>3,g>>3,b>>3; return ((R&1)<<14)|((G&1)<<13)|((B&1)<<12)|((R>>1)<<8)|((G>>1)<<4)|(B>>1)
    if _tg:
        _rgb=_np.zeros((TROWS*16,TCOLS*16,3),dtype=_np.uint8)
        _voff=(TROWS*16-_th)//2                          # NATIVE (no vertical stretch), vertically CENTRED -> black letterbox top+bottom (uncrop without stretching: the ~200px title sits 1:1 in the 224px area with ~12px letterbox each; also drops the palette count well under 248, so no bottom-row merge glitch).
        if _voff<0: _voff=0
        for _y in range(TROWS*16):
            _sy=_y-_voff
            if _sy<0 or _sy>=_th: continue               # letterbox rows: stay black (index 0)
            for _x in range(min(_tw,TCOLS*16)):
                _i=_tg[_sy][_x]
                if _i>=0: _rgb[_y,_x]=PALRGB[_i]
        _idx=_np.zeros((TROWS*16,TCOLS*16),dtype=_np.uint8); _pals=[]; _pmap={}; _tmap=[[0]*TCOLS for _ in range(TROWS)]
        for _ty in range(TROWS):
            for _tx in range(TCOLS):
                _q=Image.fromarray(_rgb[_ty*16:_ty*16+16,_tx*16:_tx*16+16]).quantize(colors=15,method=Image.MEDIANCUT)
                _qp=(_q.getpalette()+[0]*45)[:45]; _qd=list(_q.getdata())   # pad: solid/low-colour tiles return <45
                _cols=tuple((_qp[_c*3],_qp[_c*3+1],_qp[_c*3+2]) for _c in range(15))
                _key=tuple(_ngc(*_c) for _c in _cols)   # dedup on the Neo Geo RGB444 values (hardware-identical palettes merge LOSSLESSLY; indices align -- same order); keeps the count under the 248 upload limit after the 14-row stretch
                if _key not in _pmap: _pmap[_key]=len(_pals); _pals.append(_cols)
                _tmap[_ty][_tx]=_pmap[_key]
                for _p in range(256): _idx[_ty*16+_p//16,_tx*16+_p%16]=_qd[_p]+1   # 0..14 -> 1..15 (keep 0 transparent)
        MAXTPAL=248                                              # the cart uploads title palettes to PALBANK slots 8..255 -> 248 max. The 224px stretch re-tiles to ~1 palette/tile (275); cap by merging the least-used (single-tile) palettes into their nearest kept one -> lossless-looking, no overflow garbage.
        if len(_pals)>MAXTPAL:
            _use=[0]*len(_pals)
            for _ty in range(TROWS):
                for _tx in range(TCOLS): _use[_tmap[_ty][_tx]]+=1
            _ord=sorted(range(len(_pals)), key=lambda p:-_use[p]); _keep=set(_ord[:MAXTPAL])
            def _pd(a,b): return sum(min((a[i][0]-b[j][0])**2+(a[i][1]-b[j][1])**2+(a[i][2]-b[j][2])**2 for j in range(15)) for i in range(15))   # SET-based (order-insensitive): score by each colour's NEAREST in b, not slot-by-slot -> the id-logo tile merges into a colour-COMPATIBLE palette (fixes the green-square: slot-aligned scoring picked a palette that couldn't represent the logo's colours)
            def _cm(c,pal):
                _bi,_bd=0,1<<60
                for _i,_pc in enumerate(pal):
                    _d=(c[0]-_pc[0])**2+(c[1]-_pc[1])**2+(c[2]-_pc[2])**2
                    if _d<_bd: _bd,_bi=_d,_i
                return _bi
            _near={_d: min(_ord[:MAXTPAL], key=lambda k:_pd(_pals[_d],_pals[k])) for _d in _ord[MAXTPAL:]}
            for _ty in range(TROWS):
                for _tx in range(TCOLS):
                    _p=_tmap[_ty][_tx]
                    if _p in _keep: continue
                    _k=_near[_p]
                    for _yy in range(16):
                        for _xx in range(16):
                            _oi=int(_idx[_ty*16+_yy,_tx*16+_xx])
                            if _oi: _idx[_ty*16+_yy,_tx*16+_xx]=_cm(_pals[_p][_oi-1],_pals[_k])+1
                    _tmap[_ty][_tx]=_k
            _comp={_old:_new for _new,_old in enumerate(_ord[:MAXTPAL])}
            _pals=[_pals[_ord[_i]] for _i in range(MAXTPAL)]
            for _ty in range(TROWS):
                for _tx in range(TCOLS): _tmap[_ty][_tx]=_comp[_tmap[_ty][_tx]]
            print("TITLEPIC: capped palettes to %d (merged %d least-used into nearest)"%(MAXTPAL,len(_use)-MAXTPAL))
        _prev=_np.zeros((TROWS*16,TCOLS*16,3),dtype=_np.uint8)   # DEBUG preview: reconstruct the quantized colours
        for _ty in range(TROWS):
            for _tx in range(TCOLS):
                _sl=_tmap[_ty][_tx]
                for _p in range(256): _prev[_ty*16+_p//16,_tx*16+_p%16]=_pals[_sl][_idx[_ty*16+_p//16,_tx*16+_p%16]-1]
        Image.fromarray(_prev).save("/tmp/titlepic_preview.png")
        _gi=Image.fromarray(_idx,'P'); _gi.putpalette(sum([[_v,_v,_v] for _v in range(256)],[]))
        _gp="%s/titlepic.gif"%tmpd; _gi.save(_gp,optimize=False)
        _c1="%s/titlepic.c1"%tmpd; _c2="%s/titlepic.c2"%tmpd
        subprocess.run([_TILETOOL,"--sprite","-c",_gp,"-o",_c1,_c2],check=True,env=_ttenv)
        ramp_c1+=open(_c1,'rb').read(); ramp_c2+=open(_c2,'rb').read()
        with open(os.path.join(ngdir,"titlepic.h"),"w") as f:
            f.write("/* generated by wad2c.py --ramps: DOOM TITLEPIC, %dx%d sprite tilemap, per-tile palette */\n"%(TCOLS,TROWS))
            f.write("#define TITLE_HAVE 1\n#define TITLE_TILE0 %d\n#define TITLE_COLS %d\n#define TITLE_ROWS %d\n#define TITLE_NPAL %d\n"%(TITLE_TILE0,TCOLS,TROWS,len(_pals)))
            f.write("static const unsigned short TITLE_PAL16[%d][16]={\n"%len(_pals))
            for _pc in _pals: f.write("{0,"+",".join("0x%04X"%_ngc(*_c) for _c in _pc)+"},\n")
            f.write("};\nstatic const unsigned char TITLE_MAP[%d][%d]={\n"%(TROWS,TCOLS))
            for _ty in range(TROWS): f.write("{"+",".join(str(_tmap[_ty][_tx]) for _tx in range(TCOLS))+"},\n")
            f.write("};\n")
        print("TITLEPIC: TILE0=%d, %d tiles, %d palettes"%(TITLE_TILE0,TCOLS*TROWS,len(_pals)))
    else:
        with open(os.path.join(ngdir,"titlepic.h"),"w") as f:
            f.write("#define TITLE_HAVE 0\n#define TITLE_TILE0 0\n#define TITLE_COLS 20\n#define TITLE_ROWS 13\n#define TITLE_NPAL 1\n")
            f.write("static const unsigned short TITLE_PAL16[1][16]={{0}};\nstatic const unsigned char TITLE_MAP[13][20]={{0}};\n")
    # === MENU: the DOOM new-game flow graphics (episode + skill names + animated skull), baked as
    #     transparent sprite tilemaps, ONE 15-colour palette per lump (menu art is simple). Drawn over
    #     black after the title (the title's 242 palettes leave no room to overlay). Appended after
    #     TITLEPIC -> no downstream base shift. Transparent patch pixels (grid==-1) -> tile index 0. ===
    MENU_LUMPS=["M_EPISOD","M_EPI1","M_EPI2","M_EPI3","M_EPI4","M_NEWG","M_SKILL","M_JKILL","M_ROUGH","M_HURT","M_ULTRA","M_NMARE","M_SKULL1","M_SKULL2"]
    MENU_TILE0 = TITLE_TILE0 + TCOLS*TROWS
    m_off=[]; m_cols=[]; m_rows=[]; m_w=[]; m_h=[]; m_pal=[]; m_base=0; m_have=1
    for _nm in MENU_LUMPS:
        try: _mw,_mh,_mlo,_mto,_mg = parse_patch(glump(_nm))
        except Exception as _e:
            print("MENU: %s not found (%s) -> cart falls back to text menus"%(_nm,_e)); m_have=0; break
        _C=(_mw+15)//16; _R=(_mh+15)//16
        _rgb=_np.zeros((_R*16,_C*16,3),dtype=_np.uint8); _msk=_np.zeros((_R*16,_C*16),dtype=bool)
        for _y in range(_mh):
            for _x in range(_mw):
                _i=_mg[_y][_x]
                if _i>=0: _rgb[_y,_x]=PALRGB[_i]; _msk[_y,_x]=True
        _q=Image.fromarray(_rgb).quantize(colors=15,method=Image.MEDIANCUT)
        _qp=(_q.getpalette()+[0]*45)[:45]; _qd=_np.array(_q.getdata(),dtype=_np.uint8).reshape(_R*16,_C*16)
        _idx=_np.where(_msk,_qd+1,0).astype(_np.uint8)                       # opaque -> 1..15, transparent -> 0
        _pal16=[0]+[_ngc(_qp[_c*3],_qp[_c*3+1],_qp[_c*3+2]) for _c in range(15)]
        _gi=Image.fromarray(_idx,'P'); _gi.putpalette(sum([[_v,_v,_v] for _v in range(256)],[]))
        _gp="%s/menu_%s.gif"%(tmpd,_nm); _gi.save(_gp,optimize=False)
        _mc1="%s/menu_%s.c1"%(tmpd,_nm); _mc2="%s/menu_%s.c2"%(tmpd,_nm)
        subprocess.run([_TILETOOL,"--sprite","-c",_gp,"-o",_mc1,_mc2],check=True,env=_ttenv)
        _md1=open(_mc1,'rb').read(); ramp_c1+=_md1; ramp_c2+=open(_mc2,'rb').read()
        m_off.append(m_base); m_cols.append(_C); m_rows.append(_R); m_w.append(_mw); m_h.append(_mh); m_pal.append(_pal16)
        m_base += len(_md1)//64
    with open(os.path.join(ngdir,"menu.h"),"w") as f:
        if m_have:
            f.write("/* generated by wad2c.py --ramps: DOOM new-game menu graphics (episode/skill/skull) */\n")
            f.write("#define MENU_HAVE 1\n#define MENU_TILE0 %d\n#define MENU_NLUMP %d\n"%(MENU_TILE0,len(MENU_LUMPS)))
            for _i,_nm in enumerate(MENU_LUMPS): f.write("#define ML_%s %d\n"%(_nm[2:],_i))
            f.write("static const unsigned int   MENU_OFF[%d]={%s};\n"%(len(m_off),",".join(map(str,m_off))))
            f.write("static const unsigned char  MENU_COLS[%d]={%s};\n"%(len(m_cols),",".join(map(str,m_cols))))
            f.write("static const unsigned char  MENU_ROWS[%d]={%s};\n"%(len(m_rows),",".join(map(str,m_rows))))
            f.write("static const short          MENU_W[%d]={%s};\n"%(len(m_w),",".join(map(str,m_w))))
            f.write("static const short          MENU_H[%d]={%s};\n"%(len(m_h),",".join(map(str,m_h))))
            f.write("static const unsigned short MENU_PAL16[%d][16]={\n"%len(m_pal))
            for _p in m_pal: f.write("{"+",".join("0x%04X"%_v for _v in _p)+"},\n")
            f.write("};\n")
            print("MENU: TILE0=%d, %d lumps, %d tiles"%(MENU_TILE0,len(MENU_LUMPS),m_base))
        else:
            f.write("/* generated by wad2c.py --ramps: menu lumps absent */\n#define MENU_HAVE 0\n")
    # === INTERPIC (intermission): the DOOM level-complete background WIMAP0 (E1 area map), baked as a
    #     20x14 sprite tilemap EXACTLY like TITLEPIC (per-tile 15-colour palette, vertical stretch to fill
    #     the active area). Shown between levels. Appended LAST in the ramps block -> nothing in-block shifts
    #     (downstream vsfloor/vsceil/vsflat re-bake off ramps.c1 via the Makefile). ===
    INTER_TILE0 = MENU_TILE0 + m_base; ICOLS, IROWS = 20, 14; _ig = None
    try: _iw,_ih,_ilo,_ito,_ig = parse_patch(glump("WIMAP0"))
    except Exception as _e: print("WIMAP0: not found (%s) -> no intermission screen" % _e)
    if _ig:
        _rgb=_np.zeros((IROWS*16,ICOLS*16,3),dtype=_np.uint8)
        for _y in range(IROWS*16):
            _sy=_y*_ih//(IROWS*16)
            if _sy>=_ih: continue
            for _x in range(min(_iw,ICOLS*16)):
                _i=_ig[_sy][_x]
                if _i>=0: _rgb[_y,_x]=PALRGB[_i]
        _idx=_np.zeros((IROWS*16,ICOLS*16),dtype=_np.uint8); _pals=[]; _pmap={}; _tmap=[[0]*ICOLS for _ in range(IROWS)]
        for _ty in range(IROWS):
            for _tx in range(ICOLS):
                _q=Image.fromarray(_rgb[_ty*16:_ty*16+16,_tx*16:_tx*16+16]).quantize(colors=15,method=Image.MEDIANCUT)
                _qp=(_q.getpalette()+[0]*45)[:45]; _qd=list(_q.getdata())
                _cols=tuple((_qp[_c*3],_qp[_c*3+1],_qp[_c*3+2]) for _c in range(15))
                _key=tuple(_ngc(*_c) for _c in _cols)
                if _key not in _pmap: _pmap[_key]=len(_pals); _pals.append(_cols)
                _tmap[_ty][_tx]=_pmap[_key]
                for _p in range(256): _idx[_ty*16+_p//16,_tx*16+_p%16]=_qd[_p]+1
        MAXIPAL=248
        if len(_pals)>MAXIPAL:
            _use=[0]*len(_pals)
            for _ty in range(IROWS):
                for _tx in range(ICOLS): _use[_tmap[_ty][_tx]]+=1
            _ord=sorted(range(len(_pals)), key=lambda p:-_use[p]); _keep=set(_ord[:MAXIPAL])
            def _ipd(a,b): return sum(min((a[i][0]-b[j][0])**2+(a[i][1]-b[j][1])**2+(a[i][2]-b[j][2])**2 for j in range(15)) for i in range(15))
            def _icm(c,pal):
                _bi,_bd=0,1<<60
                for _i,_pc in enumerate(pal):
                    _d=(c[0]-_pc[0])**2+(c[1]-_pc[1])**2+(c[2]-_pc[2])**2
                    if _d<_bd: _bd,_bi=_d,_i
                return _bi
            _near={_d: min(_ord[:MAXIPAL], key=lambda k:_ipd(_pals[_d],_pals[k])) for _d in _ord[MAXIPAL:]}
            for _ty in range(IROWS):
                for _tx in range(ICOLS):
                    _p=_tmap[_ty][_tx]
                    if _p in _keep: continue
                    _k=_near[_p]
                    for _yy in range(16):
                        for _xx in range(16):
                            _oi=int(_idx[_ty*16+_yy,_tx*16+_xx])
                            if _oi: _idx[_ty*16+_yy,_tx*16+_xx]=_icm(_pals[_p][_oi-1],_pals[_k])+1
                    _tmap[_ty][_tx]=_k
            _comp={_old:_new for _new,_old in enumerate(_ord[:MAXIPAL])}
            _pals=[_pals[_ord[_i]] for _i in range(MAXIPAL)]
            for _ty in range(IROWS):
                for _tx in range(ICOLS): _tmap[_ty][_tx]=_comp[_tmap[_ty][_tx]]
            print("INTERPIC: capped palettes to %d (merged %d least-used)"%(MAXIPAL,len(_use)-MAXIPAL))
        _gi=Image.fromarray(_idx,'P'); _gi.putpalette(sum([[_v,_v,_v] for _v in range(256)],[]))
        _gp="%s/interpic.gif"%tmpd; _gi.save(_gp,optimize=False)
        _ic1="%s/interpic.c1"%tmpd; _ic2="%s/interpic.c2"%tmpd
        subprocess.run([_TILETOOL,"--sprite","-c",_gp,"-o",_ic1,_ic2],check=True,env=_ttenv)
        ramp_c1+=open(_ic1,'rb').read(); ramp_c2+=open(_ic2,'rb').read()
        with open(os.path.join(ngdir,"interpic.h"),"w") as f:
            f.write("/* generated by wad2c.py --ramps: DOOM WIMAP0 intermission, %dx%d sprite tilemap, per-tile palette */\n"%(ICOLS,IROWS))
            f.write("#define INTER_HAVE 1\n#define INTER_TILE0 %d\n#define INTER_COLS %d\n#define INTER_ROWS %d\n#define INTER_NPAL %d\n"%(INTER_TILE0,ICOLS,IROWS,len(_pals)))
            f.write("static const unsigned short INTER_PAL16[%d][16]={\n"%len(_pals))
            for _pc in _pals: f.write("{0,"+",".join("0x%04X"%_ngc(*_c) for _c in _pc)+"},\n")
            f.write("};\nstatic const unsigned char INTER_MAP[%d][%d]={\n"%(IROWS,ICOLS))
            for _ty in range(IROWS): f.write("{"+",".join(str(_tmap[_ty][_tx]) for _tx in range(ICOLS))+"},\n")
            f.write("};\n")
        print("INTERPIC: TILE0=%d, %d tiles, %d palettes"%(INTER_TILE0,ICOLS*IROWS,len(_pals)))
    else:
        with open(os.path.join(ngdir,"interpic.h"),"w") as f:
            f.write("#define INTER_HAVE 0\n#define INTER_TILE0 0\n#define INTER_COLS 20\n#define INTER_ROWS 14\n#define INTER_NPAL 1\n")
            f.write("static const unsigned short INTER_PAL16[1][16]={{0}};\nstatic const unsigned char INTER_MAP[14][20]={{0}};\n")
    open(os.path.join(ngdir, "ramps.c1"), "wb").write(ramp_c1)
    open(os.path.join(ngdir, "ramps.c2"), "wb").write(ramp_c2)
    with open(os.path.join(ngdir, "ramps.h"), "w") as f:
        f.write("/* generated by wad2c.py --ramps -- texture-baked diagonal-alpha smooth-wall edge tiles */\n")
        f.write("#define RAMP_TILE0 %d\n#define RAMP_DMIN (%d)\n#define RAMP_NDROP %d\n" % (RAMP_TILE0, RAMP_DMIN, RAMP_NDROP))
        f.write("static const int RAMP_OFF[%d][%d][2]={\n" % (len(texlist), RAMP_NDROP))
        for t in range(len(texlist)):
            f.write("{" + ",".join("{%d,%d}" % (ramp_off[t][d][0], ramp_off[t][d][1]) for d in range(RAMP_NDROP)) + "},\n")
        f.write("};\n")
        f.write("/* OPAQUE-CORNER cap OVERLAY (cap mode TB32o): per-slope solid-above/transparent-below wedge,\n"
                "   drawn over the wall corner with a cap palette (idx1 = mean ceiling/floor colour) so the bevel\n"
                "   merges with the ceiling/floor instead of revealing the gridded LUT. Tile = RAMP_OVL_TILE0 +\n"
                "   RAMP_OVL_OFF[slope-RAMP_DMIN] + k (stack tile, k=0 nearest the corner). -1 = no wedge. */\n")
        f.write("#define RAMP_OVL_TILE0 %d\n" % RAMP_OVL_TILE0)
        f.write("static const int RAMP_OVL_OFF[%d]={%s};\n" % (RAMP_NDROP, ",".join(str(v) for v in ramp_ovl_off)))
        f.write("static const unsigned short CAP_CEIL_PAL[16]={%s};\n" % ",".join("0x%04X" % v for v in CAP_CEIL_PAL))
        f.write("static const unsigned short CAP_FLOOR_PAL[16]={%s};\n" % ",".join("0x%04X" % v for v in CAP_FLOOR_PAL))
        f.write("/* LOD: collapsed far walls (cot==1) sample the vertical-average strip at AVG_TILE0+AVG_OFF[tex]+shift*wt+tcol */\n")
        f.write("#define AVG_TILE0 %d\n" % AVG_TILE0)
        f.write("static const int AVG_OFF[%d]={%s};\n" % (len(texlist), ",".join(str(avg_off[t]) for t in range(len(texlist)))))
    print("ramps: TILE0=%d, %d combos, %d tiles (%d KB) -> neogeo/ramps.{c1,c2,h}" % (RAMP_TILE0, len(combos), base, len(ramp_c1)//1024))
    sys.exit(0)

for ti, (w, h, grid, masked) in enumerate(texlist):
    used = [idx for idx, _ in Counter(p for r in grid for p in r if p >= 0).most_common(15)]
    local, remap = remap15(used)                           # opaque -> nearest kept colour (no black holes)
    pal16 = [0] * 16
    for idx, i in local.items():
        r, g, b = PALRGB[idx]; r5, g5, b5 = r >> 3, g >> 3, b >> 3   # 5-bit per channel
        pal16[i] = ((r5 & 1) << 14) | ((g5 & 1) << 13) | ((b5 & 1) << 12) \
                 | ((r5 >> 1) << 8) | ((g5 >> 1) << 4) | (b5 >> 1)   # NG: lsb(14-12)+hi4(11-0)
    tpal16.append(pal16)
    wp = ((w + 15) // 16) * 16; hp = ((h + 15) // 16) * 16
    _fill = local.get(used[0], 1) if used else 1   # dominant local colour: opaque fill for uncovered areas. Solid walls MUST be opaque -- index 0 is TRANSPARENT on the Neo Geo, so holes let the background bleed through = the speckled-square garbage. Masked (sprite) textures keep their transparency.
    px = bytearray([_fill]) * (wp * hp) if not masked else bytearray(wp * hp)
    for y in range(h):
        row = grid[y]
        for x in range(w):
            if row[x] >= 0: px[y * wp + x] = remap[row[x]]
    palflat = sum([[((pal16[i] >> 8) & 0xF) * 17, ((pal16[i] >> 4) & 0xF) * 17, (pal16[i] & 0xF) * 17] for i in range(16)], [])
    _dkd = min(used, key=lambda u: sum(PALRGB[u])) if used else 0       # darkest used Doom colour
    fogsrc.append((bytes(px), palflat, wp, hp, local.get(_dkd, 1)))     # (pixels, palette, w, h, darkest local idx) -> fog dither target
    tbase.append(tilebase); twt.append(wp // 16); tht.append(hp // 16)   # base = phase 0; phase s lives at base + s*(wt*ht)
    for s in range(NSHIFT):                                              # bake NSHIFT horizontal sub-tile phases
        sh = s * (16 // NSHIFT)                                          # texel roll: 0,2,4,...,14
        rolled = bytearray(wp * hp)
        for y in range(hp):
            yb = y * wp
            for x in range(wp):
                rolled[yb + x] = px[yb + ((x + sh) % wp)]               # roll left, wrap within the texture (Doom tiles horizontally)
        img = Image.frombytes('P', (wp, hp), bytes(rolled)); img.putpalette(palflat)   # keep index 0 (no black holes)
        gifp = "%s/t%d_%d.gif" % (tmpd, ti, s); img.save(gifp, optimize=False)
        c1p = "%s/t%d_%d.c1" % (tmpd, ti, s); c2p = "%s/t%d_%d.c2" % (tmpd, ti, s)
        subprocess.run([_TILETOOL, "--sprite", "-c", gifp, "-o", c1p, c2p], check=True, env=_ttenv)
        c1d = open(c1p, 'rb').read(); c2d = open(c2p, 'rb').read()
        tiles_c1 += c1d; tiles_c2 += c2d
        tilebase += len(c1d) // 64
# === per-pixel FOG-dither variants: FOG_NDENS copies of the whole base block, each Bayer-dithering
#     d% of pixels toward that texture's darkest colour -> authentic per-pixel light falloff. The
#     runtime reaches density d's tile via base_tile + d*TEX_TOTAL (the blocks are identical in layout). ===
TEX_TOTAL = tilebase                                      # base tile count; fog density d lives at base + d*TEX_TOTAL
FOG_NDENS = 0   # fog-dither variants RETIRED: the cart fogs via the 3-level real-palette ramp; the 47K dithered tile copies were 5.8MB of dormant C-ROM
_BAY = [[0,8,2,10],[12,4,14,6],[3,11,1,9],[15,7,13,5]]   # 4x4 ordered dither
_TH  = [5, 10, 15]                                        # densities ~31% / 62% / 94% dithered to dark
for _d in range(FOG_NDENS):
    _th = _TH[_d]
    for _ti, (_pxb, _pf, _wp, _hp, _dk) in enumerate(fogsrc):
        _px = bytearray(_pxb)
        for _y in range(_hp):
            _b = _BAY[_y & 3]; _yb = _y * _wp
            for _x in range(_wp):
                if _px[_yb + _x] and _b[_x & 3] < _th: _px[_yb + _x] = _dk   # darken (keep index 0 transparent)
        for _s in range(NSHIFT):
            _sh = _s * (16 // NSHIFT); _rl = bytearray(_wp * _hp)
            for _y in range(_hp):
                _yb = _y * _wp; _row = _px[_yb:_yb + _wp]
                _rl[_yb:_yb + _wp] = _row[_sh:] + _row[:_sh]                 # roll left (wrap), matches the base loop
            _img = Image.frombytes('P', (_wp, _hp), bytes(_rl)); _img.putpalette(_pf)
            _g = "%s/fog%d_%d_%d.gif" % (tmpd, _d, _ti, _s); _img.save(_g, optimize=False)
            _c1 = "%s/fog%d_%d_%d.c1" % (tmpd, _d, _ti, _s); _c2 = "%s/fog%d_%d_%d.c2" % (tmpd, _d, _ti, _s)
            subprocess.run([_TILETOOL, "--sprite", "-c", _g, "-o", _c1, _c2], check=True, env=_ttenv)
            tiles_c1 += open(_c1, 'rb').read(); tiles_c2 += open(_c2, 'rb').read()
print("baked %d fog tiles (%d densities x %d base)" % (FOG_NDENS * TEX_TOTAL, FOG_NDENS, TEX_TOTAL))
open(os.path.join(ngdir, "textiles.c1"), "wb").write(tiles_c1)
open(os.path.join(ngdir, "textiles.c2"), "wb").write(tiles_c2)
with open(os.path.join(ngdir, "textiles.h"), "w") as f:
    f.write("/* generated by wad2c.py -- per-texture tile offsets + 16-colour palettes */\n")
    f.write("#define NTEXTILE %d\n" % len(tbase))
    f.write("#define SKY_TEX_VAL %d\n" % SKY_TEX)   # SKY1 texture index (-1 if none); the live cart defines `const int SKY_TEX` from this (the on-rails map.c that used to provide SKY_TEX is delinked)
    f.write("#define TEX_NSHIFT %d\n" % NSHIFT)   # horizontal phases baked per texture; phase s at TEXTBASE+s*(TEXWT*TEXHT)
    f.write("#define TEX_TOTAL %d\n" % TEX_TOTAL)  # base tile count; per-pixel fog density d lives at base_tile + d*TEX_TOTAL
    f.write("#define FOG_NDENS %d\n" % FOG_NDENS)  # number of fog-dither density levels baked after the base block
    # 32-bit: the all-E1 C-ROM exceeds 65535 tiles, so per-texture tile offsets overflow uint16
    f.write("static const unsigned int TEXTBASE[NTEXTILE]={%s};\n" % ",".join(map(str, tbase)))
    f.write("static const unsigned char  TEXWT[NTEXTILE]={%s};\n" % ",".join(map(str, twt)))
    f.write("static const unsigned char  TEXHT[NTEXTILE]={%s};\n" % ",".join(map(str, tht)))
    f.write("static const unsigned short TEXPAL16[NTEXTILE][16]={\n")
    for p in tpal16:
        f.write(" {%s},\n" % ",".join("0x%04X" % c for c in p))
    f.write("};\n")
print("baked %d tiles into C-ROM (%d KB), %d textures" % (tilebase, len(tiles_c1) // 1024, len(tbase)))

# ---- Bake demo sprites (first-person pistol, imp billboard, status bar) into C-ROM,
#      placed AFTER the floor LUT. Each gets its own 16-colour palette (244..). ----
# (tag, lump, opaque): opaque=1 fills patch gaps with the dominant colour (the STBAR
# background is solid; its scattered post-gaps must NOT punch transparent holes).
# (tag, lump, opaque, palfrom): palfrom shares another tag's 16-colour palette (palette slots are
# scarce: 244..254). Animation frames share their base sprite's palette -- the faces are the same
# skin, the imp frames the same hide -- so 12 sprites use only 6 palette slots.
spr_want = [("PISTOL", "PISGA0", 0, None), ("IMP", "TROOA1", 0, None), ("STBAR", "STBAR", 1, None), ("FLASH", "PISFA0", 0, None),
            ("FACE", "STFST00", 0, None),  ("FACEL", "STFST02", 0, "FACE"), ("FACER", "STFST01", 0, "FACE"), ("FACEG", "STFEVL0", 0, "FACE"),
            ("IMPB", "TROOB1", 0, "IMP"), ("IMPDI", "TROOI0", 0, "IMP"), ("IMPDK", "TROOK0", 0, "IMP"), ("IMPDM", "TROOM0", 0, "IMP"),
            ("POSS", "POSSA1", 0, None), ("SPOS", "SPOSA1", 0, None),
            ("SARG", "SARGA1", 0, None), ("BOSS", "BOSSA1", 0, None),   # E1 monsters: Pinky/Demon + Baron of Hell (per-map palettes -- see MONSTER_PERMAP)
            ("POSSD", "POSSL0", 0, "POSS"), ("SPOSD", "SPOSL0", 0, "SPOS"),   # DIE5 resting-corpse frames -> zombieman/shotgunner leave bodies (imp already has IMPDM)
            ("SARGD", "SARGN0", 0, "SARG"), ("BOSSD", "BOSSO0", 0, "BOSS"),   # Pinky/Baron DIE-rest corpses
            ("HEAD", "HEADA1", 0, None), ("SPID", "SPIDA1D1", 0, None), ("CYBR", "CYBRA1", 0, None),   # E2-4: Cacodemon, Spider Mastermind, Cyberdemon (base front frames)
            ("SKUL", "SKULA1", 0, None), ("SKUL_B0", "SKULB1", 0, "SKUL"),   # Lost Soul: rotation-less (no A2A8), A/B walk frames
            ("HEADD", "HEADL0", 0, "HEAD"), ("SPIDD", "SPIDP0", 0, "SPID"), ("CYBRD", "CYBRP0", 0, "CYBR"), ("SKULD", "SKULF0", 0, "SKUL"),   # corpses (Lost Soul uses a death frame as its body)
            ("BAR", "BAR1A0", 0, None), ("BEXPC", "BEXPC0", 0, None), ("BEXPA", "BEXPB0", 0, "BEXPC"), ("BEXPE", "BEXPE0", 0, "BEXPC")]
# MONSTER 8-WAY ROTATIONS (frame A): R0 = the A1 front already baked above (IMP/POSS/SPOS). Bake R1..R4 =
# A2A8/A3A7/A4A6/A5; rotations 6/7/8 reuse R3/R2/R1 H-flipped at draw. Share the base monster's palette.
for _mon, _b in (("IMP", "TROO"), ("POSS", "POSS"), ("SPOS", "SPOS"), ("SARG", "SARG"), ("BOSS", "BOSS"), ("HEAD", "HEAD")):   # mirror-packed (5 A + 5 B). SPID is also mirror-packed but its rot1/rot5 use CROSS-FRAME lump names -> handled explicitly below.
    for _ri, _suf in ((1, "A2A8"), (2, "A3A7"), (3, "A4A6"), (4, "A5")):       # A-frame R1..R4 (R0 = the A1 above)
        spr_want.append(("%s_R%d" % (_mon, _ri), _b + _suf, 0, _mon))
    for _ri, _suf in ((0, "B1"), (1, "B2B8"), (2, "B3B7"), (3, "B4B6"), (4, "B5")):  # B-frame R0..R4 (idle/walk frame 2)
        spr_want.append(("%s_B%d" % (_mon, _ri), _b + _suf, 0, _mon))
for _ri in range(1, 8):   # CYBERDEMON: 8 INDIVIDUAL rotations (NOT mirror-packed). R1..R7 = CYBRA2..A8 (R0 = the CYBRA1 base above)
    spr_want.append(("CYBR_R%d" % _ri, "CYBRA%d" % (_ri + 1), 0, "CYBR"))
for _ri in range(8):      # CYBR B-frame B0..B7 = CYBRB1..B8
    spr_want.append(("CYBR_B%d" % _ri, "CYBRB%d" % (_ri + 1), 0, "CYBR"))
# SPIDER: mirror-packed (5 A + 5 B) but rotations 1 & 5 use CROSS-FRAME lump names (A1D1/A5D5, B1E1/B5E5), not the standard A5/B1/B5. Base R0 = SPIDA1D1 (in spr_want above).
for _ri, _l in ((1,"SPIDA2A8"),(2,"SPIDA3A7"),(3,"SPIDA4A6"),(4,"SPIDA5D5")):
    spr_want.append(("SPID_R%d" % _ri, _l, 0, "SPID"))
for _ri, _l in ((0,"SPIDB1E1"),(1,"SPIDB2B8"),(2,"SPIDB3B7"),(3,"SPIDB4B6"),(4,"SPIDB5E5")):
    spr_want.append(("SPID_B%d" % _ri, _l, 0, "SPID"))
spr_c1 = bytearray(); spr_c2 = bytearray(); sprbase = 0; sprmeta = []
spr_paldb = {}; spr_nextpal = 0          # tag -> (local, remap, pal16, palslot)
item_pal16 = []                          # per-map sprite palettes: E1 MONSTERS (SARG/BOSS) first, then items; uploaded after the flats via g_itembase
MONSTER_PERMAP = {"SARG", "BOSS", "HEAD", "SKUL", "CYBR", "SPID"}   # all added monster bases routed PER-MAP (global slots 244-254 are full); rotations/corpses share via palfrom
# BILLBOARD 2x: the Neo Geo shrinks sprites but CAN'T magnify them, so close/mid billboards rendered at
# source-pixel size = ~0.7 of true perspective height. Bake the world billboards at 2x source -> the cart's
# vs_billboard /512 size math (cap 512) reaches true size for depth>=80 (shrink-only). The HUD/gun sprites
# below draw 1:1 (they do NOT go through vs_billboard), so they stay native. (HUDNUM/HEDGER are baked in their
# own blocks further down and are also 1:1.) Keep this set in sync with vs_billboard's /512 in neogeo/main.c.
HUD1X = {"PISTOL", "STBAR", "FLASH", "FACE", "FACEL", "FACER", "FACEG"}
for tag, lname, opaque, palfrom in spr_want:
    try: pb = glump(lname)
    except Exception: print("  sprite %s (%s) not found, skipping" % (tag, lname)); continue
    w, h, lo, to, grid = parse_patch(pb)
    if tag not in HUD1X:                                     # world billboard -> 2x source (nearest-neighbour)
        w, h, lo, to = w * 2, h * 2, lo * 2, to * 2
        grid = [[grid[y >> 1][x >> 1] for x in range(w)] for y in range(h)]
    if palfrom and palfrom in spr_paldb:
        local, remap, pal16, palslot = spr_paldb[palfrom]
    else:
        used = [idx for idx, _ in Counter(p for r in grid for p in r if p >= 0).most_common(15)]
        local, remap = remap15(used)
        pal16 = [0] * 16
        for idx, i in local.items():
            r, g, b = PALRGB[idx]; r5, g5, b5 = r >> 3, g >> 3, b >> 3
            pal16[i] = ((r5 & 1) << 14) | ((g5 & 1) << 13) | ((b5 & 1) << 12) | ((r5 >> 1) << 8) | ((g5 >> 1) << 4) | (b5 >> 1)
        if tag in MONSTER_PERMAP:
            palslot = -1 - len(item_pal16); item_pal16.append(pal16)   # per-map negative marker -> ITEMPAL16 (like items); draw adds g_itembase
        else:
            palslot = spr_nextpal; spr_nextpal += 1
        spr_paldb[tag] = (local, remap, pal16, palslot)
    wp = ((w + 15) // 16) * 16; hp = ((h + 15) // 16) * 16
    px = bytearray(wp * hp)                                  # index 0 = transparent (around the sprite)
    for y in range(h):
        for x in range(w):
            v = grid[y][x]; px[y * wp + x] = remap[v] if v >= 0 else (1 if opaque else 0)
    img = Image.frombytes('P', (wp, hp), bytes(px))
    img.putpalette(sum([[((pal16[i] >> 8) & 0xF) * 17, ((pal16[i] >> 4) & 0xF) * 17, (pal16[i] & 0xF) * 17] for i in range(16)], []))
    gifp = "%s/spr_%s.gif" % (tmpd, tag); img.save(gifp, optimize=False)   # optimize=False: keep index 0 = transparent (PIL would drop it + shift)
    c1p = "%s/spr_%s.c1" % (tmpd, tag); c2p = "%s/spr_%s.c2" % (tmpd, tag)
    subprocess.run([_TILETOOL, "--sprite", "-c", gifp, "-o", c1p, c2p], check=True, env=_ttenv)
    c1d = open(c1p, 'rb').read(); c2d = open(c2p, 'rb').read()
    spr_c1 += c1d; spr_c2 += c2d
    sprmeta.append((tag, sprbase, wp // 16, hp // 16, w, h, lo, to, pal16, palslot))
    sprbase += len(c1d) // 64
# HUD number font: STTNUM0-9 + STTPRCNT (the % sign) composited into one strip,
# 1 tile (16px) per glyph, sharing ONE palette. Glyph g is tile HUDNUM_BASE+g.
GLYPHS = ["STTNUM%d" % i for i in range(10)] + ["STTPRCNT"]
GW, GH = 16, 16
sgrid = [[-1] * (GW * len(GLYPHS)) for _ in range(GH)]
for gi, lump in enumerate(GLYPHS):
    gw, gh, glo, gto, gg = parse_patch(glump(lump))
    for y in range(min(gh, GH)):
        for x in range(min(gw, GW)):
            v = gg[y][x]
            if v >= 0: sgrid[y][gi * GW + x] = v
used = [idx for idx, _ in Counter(p for r in sgrid for p in r if p >= 0).most_common(15)]
local, remap = remap15(used)
pal16 = [0] * 16
for idx, i in local.items():
    r, g, b = PALRGB[idx]; r5, g5, b5 = r >> 3, g >> 3, b >> 3
    pal16[i] = ((r5 & 1) << 14) | ((g5 & 1) << 13) | ((b5 & 1) << 12) | ((r5 >> 1) << 8) | ((g5 >> 1) << 4) | (b5 >> 1)
sw = GW * len(GLYPHS)
px = bytearray(sw * GH)
for y in range(GH):
    for x in range(sw):
        v = sgrid[y][x]; px[y * sw + x] = remap[v] if v >= 0 else 0
img = Image.frombytes('P', (sw, GH), bytes(px))
img.putpalette(sum([[((pal16[i] >> 8) & 0xF) * 17, ((pal16[i] >> 4) & 0xF) * 17, (pal16[i] & 0xF) * 17] for i in range(16)], []))
gifp = "%s/spr_HUDNUM.gif" % tmpd; img.save(gifp, optimize=False)
c1p = "%s/spr_HUDNUM.c1" % tmpd; c2p = "%s/spr_HUDNUM.c2" % tmpd
subprocess.run([_TILETOOL, "--sprite", "-c", gifp, "-o", c1p, c2p], check=True, env=_ttenv)
c1d = open(c1p, 'rb').read(); c2d = open(c2p, 'rb').read()
spr_c1 += c1d; spr_c2 += c2d
sprmeta.append(("HUDNUM", sprbase, sw // 16, GH // 16, sw, GH, 0, 0, pal16, spr_nextpal)); spr_nextpal += 1   # WT = glyph count
sprbase += len(c1d) // 64
# HUD RIGHT-EDGE sprite: the COMPOSED bar's right 16px (incl. the baked-in YELLOW ammo-table numerals + black)
# -> vs_hud_edges fills the right gap (fix col 39, in horizontal blanking) matching the cropped numbers. The
# raw STBAR sprite has only the grey frame there. Left edge stays on raw STBAR (no numerals at x0..15). Baked
# LAST so it takes the one free sprite-palette slot (254; 255 holds the backdrop murk).
_eg = [[_HUDBAR[_y][304 + _x] for _x in range(16)] for _y in range(32)]
_used = [idx for idx, _ in Counter(p for r in _eg for p in r).most_common(15)]
_local, _remap = remap15(_used)
_p16 = [0] * 16
for _ix, _i in _local.items():
    _r, _g, _b = PALRGB[_ix]; _r5, _g5, _b5 = _r >> 3, _g >> 3, _b >> 3
    _p16[_i] = ((_r5 & 1) << 14) | ((_g5 & 1) << 13) | ((_b5 & 1) << 12) | ((_r5 >> 1) << 8) | ((_g5 >> 1) << 4) | (_b5 >> 1)
_ps = spr_nextpal; spr_nextpal += 1
_px = bytearray(16 * 32)
for _y in range(32):
    for _x in range(16):
        _v = _remap[_eg[_y][_x]]; _px[_y * 16 + _x] = _v if _v else 1   # remap is a list[orig_idx]->slot; fall back to 1 (opaque) for any colour outside the top-15
_im = Image.frombytes('P', (16, 32), bytes(_px))
_im.putpalette(sum([[((_p16[i] >> 8) & 0xF) * 17, ((_p16[i] >> 4) & 0xF) * 17, (_p16[i] & 0xF) * 17] for i in range(16)], []))
_gp = "%s/spr_HEDGER.gif" % tmpd; _im.save(_gp, optimize=False)
_c1p = "%s/spr_HEDGER.c1" % tmpd; _c2p = "%s/spr_HEDGER.c2" % tmpd
subprocess.run([_TILETOOL, "--sprite", "-c", _gp, "-o", _c1p, _c2p], check=True, env=_ttenv)
_c1d = open(_c1p, 'rb').read(); _c2d = open(_c2p, 'rb').read(); spr_c1 += _c1d; spr_c2 += _c2d
sprmeta.append(("HEDGER", sprbase, 1, 2, 16, 32, 0, 0, _p16, _ps)); sprbase += len(_c1d) // 64
# ---- ITEM billboards (armour + ammo): each gets its OWN tiles (appended to the sprite chain tail) +
#      its OWN 16-colour palette, but the palette is NOT a global 244+ slot (those are full). Instead
#      palettes are emitted into ITEMPAL16[] and uploaded PER-MAP after the flats (slots <=243) by
#      vs_upload_tex_pals in main.c -- mirroring the per-map flat-palette path. palslot is stored as a
#      NEGATIVE marker (-1-itemidx) so the header emit routes them to ITEM_PAL/ITEMPAL16 not 244+. ----
item_want = [("ARM1","ARM1A0"),("ARM2","ARM2A0"),("BON2","BON2A0"),      # armour: green, blue, helmet bonus
             ("CLIP","CLIPA0"),("AMMO","AMMOA0"),("SHEL","SHELA0"),("SBOX","SBOXA0"),  # bullets, shells
             ("ROCK","ROCKA0"),("BROK","BROKA0")]          # rockets (CELL/CELP omitted: not in shareware doom1.wad -- no plasma/BFG ammo in E1)
# item_pal16 already defined above (holds the per-map MONSTER pals SARG/BOSS first); items append AFTER them so SPR_x_PAL indices stay contiguous (monsters 0..,  items continue)
for tag, lname in item_want:
    try: pb = glump(lname)
    except Exception: print("  item %s (%s) not found, skipping" % (tag, lname)); continue
    w, h, lo, to, grid = parse_patch(pb)
    w, h, lo, to = w * 2, h * 2, lo * 2, to * 2             # items are world billboards -> 2x source (see HUD1X note)
    grid = [[grid[y >> 1][x >> 1] for x in range(w)] for y in range(h)]
    used = [idx for idx, _ in Counter(p for r in grid for p in r if p >= 0).most_common(15)]
    local, remap = remap15(used)
    pal16 = [0] * 16
    for idx, i in local.items():
        r, g, b = PALRGB[idx]; r5, g5, b5 = r >> 3, g >> 3, b >> 3
        pal16[i] = ((r5 & 1) << 14) | ((g5 & 1) << 13) | ((b5 & 1) << 12) | ((r5 >> 1) << 8) | ((g5 >> 1) << 4) | (b5 >> 1)
    wp = ((w + 15) // 16) * 16; hp = ((h + 15) // 16) * 16
    px = bytearray(wp * hp)                                  # index 0 = transparent
    for y in range(h):
        for x in range(w):
            v = grid[y][x]; px[y * wp + x] = remap[v] if v >= 0 else 0
    img = Image.frombytes('P', (wp, hp), bytes(px))
    img.putpalette(sum([[((pal16[i] >> 8) & 0xF) * 17, ((pal16[i] >> 4) & 0xF) * 17, (pal16[i] & 0xF) * 17] for i in range(16)], []))
    gifp = "%s/spr_%s.gif" % (tmpd, tag); img.save(gifp, optimize=False)
    c1p = "%s/spr_%s.c1" % (tmpd, tag); c2p = "%s/spr_%s.c2" % (tmpd, tag)
    subprocess.run([_TILETOOL, "--sprite", "-c", gifp, "-o", c1p, c2p], check=True, env=_ttenv)
    c1d = open(c1p, 'rb').read(); c2d = open(c2p, 'rb').read()
    spr_c1 += c1d; spr_c2 += c2d
    sprmeta.append((tag, sprbase, wp // 16, hp // 16, w, h, lo, to, pal16, -1 - len(item_pal16)))   # palslot<0 = item idx marker
    item_pal16.append(pal16)
    sprbase += len(c1d) // 64
open(os.path.join(ngdir, "sprites.c1"), "wb").write(spr_c1)
open(os.path.join(ngdir, "sprites.c2"), "wb").write(spr_c2)
with open(os.path.join(ngdir, "sprites.h"), "w") as f:
    f.write("/* generated by wad2c.py -- demo sprites; absolute tile = SPR_TILE0 + SPR_x_BASE + r*SPR_x_WT + col */\n")
    f.write("#define SPR_COUNT %d\n" % len(sprmeta))
    for i, (tag, base, wt, ht, w, h, lo, to, pal16, palslot) in enumerate(sprmeta):
        # palslot>=0 -> global sprite palette slot (244+); palslot<0 -> ITEM: SPR_x_PAL = item index
        # (the runtime per-map base g_itembase is ADDED in the draw path), palette comes from ITEMPAL16[].
        palval = (244 + palslot) if palslot >= 0 else (-1 - palslot)
        f.write("#define SPR_%s_BASE %d\n#define SPR_%s_WT %d\n#define SPR_%s_HT %d\n#define SPR_%s_W %d\n#define SPR_%s_H %d\n#define SPR_%s_LO %d\n#define SPR_%s_TO %d\n#define SPR_%s_PAL %d\n"
                % (tag, base, tag, wt, tag, ht, tag, w, tag, h, tag, lo, tag, to, tag, palval))
        f.write("static const unsigned short SPR_%s_PAL16[16]={%s};\n" % (tag, ",".join("0x%04X" % c for c in pal16)))
    # per-map ITEM palettes (armour/ammo): uploaded after the flats by vs_upload_tex_pals; index = SPR_x_PAL.
    f.write("#define NITEMPAL %d\n" % len(item_pal16))
    f.write("static const unsigned short ITEMPAL16[%d][16]={\n" % max(1, len(item_pal16)))
    for p in (item_pal16 or [[0] * 16]):
        f.write(" {%s},\n" % ",".join("0x%04X" % c for c in p))
    f.write("};\n")
print("baked %d sprites into C-ROM (%d tiles): %s" % (len(sprmeta), sprbase, ",".join(m[0] for m in sprmeta)))

print("%s: %d verts %d sectors %d sides %d lines %d segs %d nodes %d things | %d textures, crom %d KB"
      % (MAP, len(verts), len(sec_out), len(sides_out), len(lines), len(segs),
         len(nodes), len(th_out), len(texlist), len(crom) // 1024))
print("spawn=(%d,%d) sector=%d z=%d  crom->%s" % (sx, sy, startsec, startz, crom_path))

sky_secs = [i for i, s in enumerate(sectors) if s[4] == "F_SKY1"]
sky_vant = None
if sky_secs:
    s0 = sky_secs[0]; pts = []
    for (v1, v2, fl, sp, r, l) in lines:
        if (r >= 0 and sider[r][0] == s0) or (l >= 0 and sider[l][0] == s0):
            pts += [verts[v1], verts[v2]]
    if pts:
        sky_vant = (sum(p[0] for p in pts) // len(pts), sum(p[1] for p in pts) // len(pts))
print("sky: SKY_TEX=%d, %d sky sectors, vantage=%s" % (SKY_TEX, len(sky_secs), sky_vant))

DOORSP = {1, 26, 27, 28, 31, 32, 33, 34, 117, 118}
door_lines = [((verts[v1][0] + verts[v2][0]) // 2, (verts[v1][1] + verts[v2][1]) // 2)
              for (v1, v2, fl, sp, r, l) in lines if sp in DOORSP]
print("doors: %d door linedefs, first midpoint=%s" % (len(door_lines), door_lines[0] if door_lines else None))
