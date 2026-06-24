#!/usr/bin/env python3
# Ground-truth reference capture: headless ViZDoom (real ZDoom + the real WAD) renders
# E1M1 at exact (x,y,angle). Sets the camera by PATCHING the player-1 start in a WAD copy
# (ViZDoom's turn-action was a no-op), so the player spawns exactly at the desired position/angle.
# Modes:
#   refcap.py <wad> <MAP> <x> <y> <angles_csv_deg> <out_prefix>   # sweep at one spot
#   refcap.py <wad> <MAP> --list <nodefile> <outdir>             # one frame per path node
#     nodefile lines: "<i> <x> <y> <deg>"  (deg ZDoom convention, 0=east CCW)
import os, sys, os, struct
import vizdoom as vzd
from PIL import Image

def patch_start(wad_in, wad_out, mapn, x, y, ang):
    data = bytearray(open(wad_in, "rb").read())
    _m, nl, dofs = struct.unpack_from("<4sii", data, 0)
    midx = next((i for i in range(nl)
                 if struct.unpack_from("<ii8s", data, dofs+i*16)[2].split(b"\0")[0].decode("latin1") == mapn), None)
    if midx is None: raise SystemExit(f"map {mapn} not found")
    fp, sz, _n = struct.unpack_from("<ii8s", data, dofs + (midx+1)*16)     # THINGS = first lump after marker
    for t in range(sz // 10):
        if struct.unpack_from("<hhhhh", data, fp+t*10)[3] == 1:            # player-1 start type
            struct.pack_into("<hhh", data, fp+t*10, int(x), int(y), int(ang) % 360); break
    else: raise SystemExit("no player-1 start")
    open(wad_out, "wb").write(data)

def capture(wad, mapn, x, y, ang, outpath):
    patch_start(wad, "/tmp/_ref.wad", mapn, x, y, ang)
    g = vzd.DoomGame()
    g.set_doom_game_path("/tmp/_ref.wad"); g.set_doom_map(mapn)
    g.set_screen_resolution(vzd.ScreenResolution.RES_320X240)
    g.set_screen_format(vzd.ScreenFormat.RGB24)
    g.set_render_hud(bool(os.environ.get("REFCAP_HUD"))); g.set_render_weapon(False); g.set_render_crosshair(False)   # REFCAP_HUD=1: keep the status bar (DEBT #3 reference)
    g.set_render_decals(False); g.set_render_particles(False)
    g.set_render_messages(False); g.set_render_corpses(False)
    g.set_window_visible(False); g.set_available_buttons([])
    g.set_mode(vzd.Mode.PLAYER); g.init(); g.new_episode()
    g.make_action([], 3)
    Image.fromarray(g.get_state().screen_buffer).save(outpath)
    g.close()

wad, mapn = sys.argv[1], sys.argv[2]
if sys.argv[3] == "--list":
    nodefile, outdir = sys.argv[4], sys.argv[5]
    os.makedirs(outdir, exist_ok=True)
    rows = [ln.split() for ln in open(nodefile) if ln.strip() and not ln.startswith("#")]
    for n, (i, x, y, deg) in enumerate(rows):
        capture(wad, mapn, float(x), float(y), int(deg), f"{outdir}/node_{int(i):03d}.png")
        if n % 20 == 0: print(f"  {n+1}/{len(rows)} ...", flush=True)
    print(f"done: {len(rows)} frames -> {outdir}")
else:
    x, y = float(sys.argv[3]), float(sys.argv[4]); outpre = sys.argv[6]
    for a in [int(v) for v in sys.argv[5].split(",")]:
        capture(wad, mapn, x, y, a, f"{outpre}_a{a:03d}.png")
        print(f"angle {a:3d} -> {outpre}_a{a:03d}.png")
    print("done")
