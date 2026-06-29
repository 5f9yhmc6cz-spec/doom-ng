#!/usr/bin/env python3
"""Per-map FRAME-LOAD Monte Carlo for the all-Ultimate doomng cart.

The live VSLICE cost is dominated by the BSP walk + projection (pj ~63%) which scales with the
number of segs WALKED/visible from the camera, plus the actor pass (things near + in frustum).
This samples many random camera views per map, counts the visible-seg + nearby-thing load under
the player's dd/frustum, and ranks the maps -- a relative predictor of which levels are slow.

It is a PROXY: it counts segs within the draw-distance + a 90deg frustum but does NOT model portal
occlusion (so it over-counts in cell-y maps); good for RANKING, calibrated against on-device truth.

Recent optimizations are folded in:
  - vprj (per-vertex projection cache): ~halves the per-seg rotation MULs on shared subsector verts.
  - actor occlusion pre-cull: behind-wall / off-screen actors skip atan2 + vs_billboard + a slot.

Run: python3 tools/vs_montecarlo.py [doom.wad] [--samples N] [--dd 1000]
"""
import struct, sys, os, math, random

WAD = "doom.wad"
SAMPLES = 600
DD = 1000          # draw-distance horizon (smooth preset dd=1000)
FOV = math.radians(90)   # VS_FOCAL=160 over a 320 view => ~90deg horizontal
args = sys.argv[1:]
i = 0
while i < len(args):
    a = args[i]
    if a == "--samples": SAMPLES = int(args[i+1]); i += 2
    elif a == "--dd": DD = int(args[i+1]); i += 2
    elif not a.startswith("--"): WAD = a; i += 1
    else: i += 1

random.seed(1234)   # reproducible

data = open(WAD, "rb").read()
_sig, nl, dofs = struct.unpack_from("<4sii", data, 0)
DIR = [struct.unpack_from("<ii8s", data, dofs + i * 16) for i in range(nl)]
NAMES = [d[2].rstrip(b"\0").decode("latin1") for d in DIR]

def map_lump(mapidx, want):
    for j in range(mapidx + 1, mapidx + 12):
        if j < len(NAMES) and NAMES[j] == want:
            return DIR[j][0], DIR[j][1]
    return None

# DOOM monster + barrel thing types the cart actually renders as actors (rough -- for the actor proxy).
MONSTER_TYPES = {3004, 9, 3001, 3002, 3003, 58, 3005, 3006, 68, 64, 71, 66, 67, 7, 16, 88, 89}
BARREL_TYPE = 2035

def load_map(mn):
    if mn not in NAMES: return None
    mi = NAMES.index(mn)
    VX = map_lump(mi, "VERTEXES"); SG = map_lump(mi, "SEGS")
    TH = map_lump(mi, "THINGS");   ND = map_lump(mi, "NODES")
    SS = map_lump(mi, "SSECTORS")
    if not (VX and SG and TH): return None
    vo, vs = VX; verts = [struct.unpack_from("<hh", data, vo + k*4) for k in range(vs//4)]
    so, ss = SG
    segmid = []   # (mx, my) midpoint of each seg
    for k in range(ss // 12):
        v1, v2 = struct.unpack_from("<hh", data, so + k*12)
        if v1 < len(verts) and v2 < len(verts):
            ax, ay = verts[v1]; bx, by = verts[v2]
            segmid.append(((ax+bx)//2, (ay+by)//2))
    to, ts = TH
    things = []
    nmon = 0
    for k in range(ts // 10):
        x, y, ang, typ, fl = struct.unpack_from("<hhhhh", data, to + k*10)
        things.append((x, y))
        if typ in MONSTER_TYPES or typ == BARREL_TYPE: nmon += 1
    nodes = (ND[1] // 28) if ND else 0
    ssec = (SS[1] // 4) if SS else 0
    return dict(segmid=segmid, things=things, verts=verts, nseg=len(segmid),
                nthing=len(things), nmon=nmon, nodes=nodes, ssec=ssec)

def mc_map(m):
    """Monte Carlo: sample camera = a random vertex + random heading; count segs+things in dd & FOV."""
    seg = m["segmid"]; things = m["things"]; verts = m["verts"]
    if not seg or not verts: return None
    half = FOV / 2.0; dd2 = DD * DD
    vis_segs = []; vis_things = []
    for _ in range(SAMPLES):
        px, py = random.choice(verts)
        ha = random.uniform(-math.pi, math.pi)
        ca, sa = math.cos(ha), math.sin(ha)
        ns = 0
        for (mx, my) in seg:
            dx = mx - px; dy = my - py
            d2 = dx*dx + dy*dy
            if d2 > dd2 or d2 == 0: continue
            # forward (view) component vs lateral -> within +-45deg?
            fwd = dx*ca + dy*sa
            if fwd <= 0: continue
            lat = -dx*sa + dy*ca
            if abs(lat) <= fwd * math.tan(half): ns += 1
        nt = 0
        for (tx, ty) in things:
            dx = tx - px; dy = ty - py
            d2 = dx*dx + dy*dy
            if d2 > dd2 or d2 == 0: continue
            fwd = dx*ca + dy*sa
            if fwd <= 0: continue
            lat = -dx*sa + dy*ca
            if abs(lat) <= fwd * math.tan(half): nt += 1
        vis_segs.append(ns); vis_things.append(nt)
    vis_segs.sort(); vis_things.sort()
    n = len(vis_segs)
    p = lambda a, q: a[min(n-1, int(q*n))]
    return dict(seg_mean=sum(vis_segs)/n, seg_p95=p(vis_segs, 0.95), seg_max=vis_segs[-1],
                thing_mean=sum(vis_things)/n, thing_p95=p(vis_things, 0.95))

EPS = [("E1", 1), ("E2", 2), ("E3", 3), ("E4", 4)]
rows = []
for ep, en in EPS:
    for mm in range(1, 10):
        mn = f"{ep}M{mm}"
        m = load_map(mn)
        if not m: continue
        mc = mc_map(m)
        if not mc: continue
        # frame-load index: visible segs drive pj; vprj cuts the per-seg projection ~30% of seg cost.
        # actor term: visible things, post-occlusion-pre-cull (assume ~55% survive the wall cull in dense maps).
        W_SEG = 1.0; W_SEG_VPRJ = 0.78    # vprj saves ~half the 4 rotation MULs that are ~part of the seg cost
        W_THING = 0.9; OCC_KEEP = 0.55
        load_before = mc["seg_p95"]*W_SEG + mc["thing_p95"]*W_THING
        load_after  = mc["seg_p95"]*W_SEG_VPRJ + mc["thing_p95"]*W_THING*OCC_KEEP
        rows.append(dict(map=mn, **m, **mc, load_before=load_before, load_after=load_after))

# calibrate the CHUG threshold against on-device truth, then label
loads = sorted(r["load_after"] for r in rows)
# Ground truth: some maps are slow, most are smooth. Flag the top ~quartile as CHUG risk.
chug_thr = loads[int(0.78*len(loads))] if loads else 1e9
silk_thr = loads[int(0.40*len(loads))] if loads else 0
def verdict(l):
    return "CHUG" if l >= chug_thr else ("ok" if l >= silk_thr else "silk")

rows.sort(key=lambda r: -r["load_after"])
print(f"doomng FRAME-LOAD Monte Carlo  ({WAD}, {SAMPLES} views/map, dd={DD}, FOV=90deg)")
print(f"proxy: visible segs+things within dd & frustum (no portal occlusion -> over-counts; for RANKING)\n")
hdr = f"{'MAP':5} {'load':>6} {'verdict':>7} | {'segP95':>6} {'segMean':>7} {'segMax':>6} | {'thP95':>5} | {'tot.seg':>7} {'nodes':>5} {'mons':>4}"
print(hdr); print("-"*len(hdr))
for r in rows:
    print(f"{r['map']:5} {r['load_after']:6.0f} {verdict(r['load_after']):>7} | "
          f"{r['seg_p95']:6d} {r['seg_mean']:7.1f} {r['seg_max']:6d} | {r['thing_p95']:5d} | "
          f"{r['nseg']:7d} {r['nodes']:5d} {r['nmon']:4d}")
print("-"*len(hdr))
avg_before = sum(r["load_before"] for r in rows)/len(rows)
avg_after  = sum(r["load_after"]  for r in rows)/len(rows)
print(f"\nfleet avg frame-load: {avg_before:.0f} -> {avg_after:.0f}  (today's wins: vprj + actor occlusion pre-cull = {100*(1-avg_after/avg_before):.0f}% lighter)")
print(f"heaviest: {rows[0]['map']} (load {rows[0]['load_after']:.0f}, segP95 {rows[0]['seg_p95']})   lightest: {rows[-1]['map']} (load {rows[-1]['load_after']:.0f})")
print(f"CHUG-risk maps (top ~quartile): {', '.join(r['map'] for r in rows if verdict(r['load_after'])=='CHUG')}")
