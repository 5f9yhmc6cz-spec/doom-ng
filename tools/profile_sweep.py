#!/usr/bin/env python3
"""DOOM-NG headless 68k-CYCLE profiler sweep.

Runs the patched gngeo (tools/gngeo-shot -> neogeo/tools/gngeo-prof) with the cart in profile mode
($DOOMNG_PROFILE). The cart auto-skips the title/menu, drives a canned camera path through the REAL
movement code (deterministic + config-independent), resets to spawn between a cart-side config sweep,
and brackets each vs_render with cycle markers. gngeo (generator68k) timestamps 68k cycles at each
marker and dumps per-frame totals to a CSV. We parse it -> avg / 1%-low / 0.1%-low / frv / max per
config, ranked by 1%-low (the metric that predicts felt smoothness). See memory doomng-headless-profiler.

Why this and not the in-cart bnch: the cart's only clock is vblank-granular and bnch reports a 16x-burst
AVERAGE, which structurally cannot yield percentiles. The truth (per-frame cycle distribution) lives in
the emulator. NG fps is quantized 60/30/20, so percentiles on fps quantize the judder away; on cycles
they don't.

Usage:  tools/profile_sweep.py                # build cart? no -- run the sweep + report
        tools/profile_sweep.py --parse CSV    # just re-report an existing CSV (no run)
        tools/profile_sweep.py --timeout 300  # cap the gngeo wall-clock (default 300s)
"""
import os, sys, subprocess, csv, statistics, argparse

ROOT  = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NG    = os.path.join(ROOT, "neogeo")
GNGEO = os.path.join(NG, "tools", "gngeo-prof")
ELF   = os.path.join(NG, "build", "rom.elf")
DATA  = os.path.join(NG, "gngeo_data_doomng.zip")
OUT   = "/tmp/doomng_prof.csv"
NM_CANDIDATES = ["m68k-neogeo-elf-nm", "m68k-elf-nm",
                 "/opt/homebrew/bin/m68k-neogeo-elf-nm"]

# Mirror of the cart's prof_apply_cfg() switch (neogeo/main.c). Keep in sync when you edit the sweep.
# CANONICAL: component breakdown at the new defaults (0=full,1=-emit,2=-emit-flats,3=-emit-flats-actors).
CFG_LABELS = {0: "full", 1: "-emit", 2: "-emit-flats", 3: "-emit-flats-actors"}
# (component-breakdown labels: 0=full,1=-emit,2=-emit-flats,3=-emit-flats-actors; the breakdown
#  differencing below auto-fires only when label[1] contains 'emit'.)

def which(cands):
    for c in cands:
        if os.path.isabs(c) and os.path.exists(c): return c
        p = subprocess.run(["which", c], capture_output=True, text=True)
        if p.returncode == 0: return p.stdout.strip()
    return None

def prof_port_addr():
    nm = which(NM_CANDIDATES)
    if not nm: sys.exit("no m68k nm found (need ngdevkit toolchain)")
    if not os.path.exists(ELF): sys.exit(f"no ELF at {ELF} -- build the cart first (make -C neogeo cart)")
    for line in subprocess.check_output([nm, ELF]).decode().splitlines():
        p = line.split()
        if len(p) >= 3 and p[2] == "g_prof_port":
            return int(p[0], 16) & 0xffff
    sys.exit("g_prof_port not found in ELF (is profile mode compiled in?)")

def run(timeout):
    if not os.path.exists(GNGEO): sys.exit(f"no gngeo-prof at {GNGEO} -- build it (see memory doomng-headless-profiler)")
    addr = prof_port_addr()
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
               DOOMNG_PROFILE="1", DOOMNG_PROF_PORT=hex(addr), DOOMNG_PROF_OUT=OUT)
    if os.path.exists(OUT): os.remove(OUT)
    print(f"[sweep] gngeo-prof  g_prof_port=0x{addr:04x}  out={OUT}  (timeout {timeout}s)")
    cmd = [GNGEO, "-d", DATA, "--screen320", "--no-pal", "--no-vsync",
           "-b", "soft", "-e", "none", "-i", os.path.join(NG, "build", "rom"), "puzzledp"]
    try:
        subprocess.run(cmd, cwd=NG, env=env, timeout=timeout,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        print("[sweep] gngeo exited (DONE reached)")
    except subprocess.TimeoutExpired:
        print(f"[sweep] WARNING: no DONE within {timeout}s -- parsing partial CSV")
        subprocess.run(["pkill", "-9", "-f", "gngeo-prof"], capture_output=True)

def metrics(costs):
    s = sorted(costs, reverse=True)        # highest cost (worst) first
    low = lambda p: sum(s[:max(1, int(len(s) * p))]) / max(1, int(len(s) * p))
    return dict(n=len(costs), avg=sum(costs) / len(costs),
                lo1=low(0.01), lo01=low(0.001), frv=statistics.pstdev(costs), mx=max(costs))

def report(csv_path):
    rows, bad = {}, 0
    with open(csv_path) as f:
        for r in csv.DictReader(f):
            try: rows.setdefault(int(r["cfg"]), []).append(int(r["total"]))
            except (ValueError, TypeError): bad += 1
    if not rows: sys.exit("no usable rows in CSV")
    res = {c: metrics(v) for c, v in rows.items() if v}
    base = res.get(0, {}).get("lo1")
    print(f"\n{'config':<28}{'n':>6}{'avg':>11}{'1%low':>11}{'0.1%low':>11}{'frv':>10}{'max':>11}  vs base")
    print("-" * 100)
    for c in sorted(res, key=lambda c: res[c]["lo1"]):
        m = res[c]; lab = CFG_LABELS.get(c, f"cfg{c}")
        d = f"{100*(m['lo1']-base)/base:+6.1f}%" if base else ""
        print(f"{lab:<28}{m['n']:>6}{m['avg']:>11.0f}{m['lo1']:>11.0f}{m['lo01']:>11.0f}{m['frv']:>10.0f}{m['mx']:>11.0f}  {d}")
    nfull = max((m["n"] for m in res.values()), default=0)
    print(f"\nranked by 1%-low (lower=smoother). cycles/frame; 200000 = one 60fps vblank budget.")
    print(f"configs {sorted(res)}; ~{nfull} frames/config; {bad} malformed rows skipped.")
    if nfull < 1000: print("NOTE: <1000 frames/config -> 0.1%-low is thin (lengthen PROF_PATH for a solid 0.1%).")
    # COMPONENT BREAKDOWN: only for the cumulative-disable set (label[1] mentions 'emit'), difference it.
    if all(c in res for c in (0,1,2,3)) and 'emit' in CFG_LABELS.get(1,''):
        for metric,key in (("AVG","avg"),("WORST-FRAME (1%-low)","lo1")):
            v = {c: res[c][key] for c in (0,1,2,3)}; full = v[0]
            comps = [("emit (SCB write)", v[0]-v[1]), ("flats (floor/ceil LUT)", v[1]-v[2]),
                     ("actors (billboards)", v[2]-v[3]), ("walk+proj (BSP)", v[3])]
            print(f"\n=== COMPONENT BREAKDOWN -- {metric} (cycles/frame, cumulative-disable differencing) ===")
            print(f"{'stage':<28}{'cycles':>10}{'% of full':>11}{'vblanks':>9}")
            print("-"*58)
            for name,c in sorted(comps, key=lambda x:-x[1]):
                print(f"{name:<28}{c:>10.0f}{100*c/full:>10.1f}%{c/200000:>9.2f}")
            print("-"*58); print(f"{'FULL':<28}{full:>10.0f}{100.0:>10.1f}%{full/200000:>9.2f}")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--parse", metavar="CSV", help="re-report an existing CSV without running")
    ap.add_argument("--timeout", type=int, default=300, help="gngeo wall-clock cap (s)")
    a = ap.parse_args()
    if a.parse: report(a.parse); return
    run(a.timeout)
    if not os.path.exists(OUT): sys.exit("no CSV produced -- did the cart enter profile mode? (check g_prof_port)")
    report(OUT)

if __name__ == "__main__":
    main()
