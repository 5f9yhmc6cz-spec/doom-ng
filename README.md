# DOOM-NG — free-roam DOOM on a stock Neo Geo

A live, free-roam renderer of **DOOM Episode 1** (all nine maps, E1M1–E1M9) running on an
unmodified Neo Geo (Motorola 68000 @ 12 MHz, no expansion hardware). You walk and turn
freely; each frame the 68000 walks DOOM's original **BSP tree** to find what's visible and
projects it, and the Neo Geo's **hardware sprite scaler** draws the textured wall columns.
The CPU does visibility; the sprite hardware does the pixels. No per-pixel software
rasterisation on the CPU at all.

The live renderer is called **VSLICE** (a BSP walk that emits the scene as vertical hardware
sprite strips). Build it with `make rom-vs`. An older on-rails / pre-rendered-frame engine
("FF") shares some of the codebase and is being retired.

---

## ⚠️ This repo contains NO id Software / DOOM assets

Nothing here is derived from DOOM until **you** supply an IWAD. Every texture, sprite,
palette, map, sound and lookup table is **generated from your own `doom1.wad` at build time**
and is git-ignored. You must bring your own WAD.

- **DOOM shareware `doom1.wad` works** and is freely and legally downloadable (id's official
  shareware release, archive.org, or your Steam/GOG DOOM install's IWAD). The full `doom.wad`
  works too.
- The generated, DOOM-derived files (textures, sprites, maps, geometry, sounds, the C-ROM
  tile blobs) are all in `.gitignore` — **never commit them**; doing so would redistribute
  id Software content.
- This is an unofficial, non-commercial homebrew renderer and is **not affiliated with or
  endorsed by id Software / ZeniMax / Bethesda**.

---

## 1. Prerequisites

| Need | What for | Where |
|------|----------|-------|
| **ngdevkit** | Neo Geo toolchain: `m68k-neogeo-elf-gcc`, the Z80 sound tools, `tiletool.py` etc., the open BIOS, and the `ngdevkit-gngeo` emulator | https://github.com/dciabrin/ngdevkit |
| **Python 3.8+** with **Pillow + numpy** | the asset pipeline (`tools/*.py`); `pip install -r requirements.txt` (or `apt install python3-pil python3-numpy`) | your OS / pip |
| **C compiler + make + SDL2** | builds `doomng-host`, the offline renderer/baker the pipeline drives | your OS (`gcc`/`clang`, `libsdl2-dev` / `sdl2`) |
| **ImageMagick** (`magick` or `convert`) | bakes the logo tile during the cart build | `imagemagick` (apt) / `brew install imagemagick` |
| **zip / unzip** | repacks the gngeo run datafile (`tools/patch_gngeo_datafile.sh`) | your OS pkg mgr (preinstalled on macOS) |
| **`doom1.wad`** | the IWAD — you supply it (see above) | shareware / your DOOM install |
| MAME *(optional)* | cycle-accurate hardware validation (needs a real SNK/UniBIOS — not the gngeo path) | https://mamedev.org |

Install ngdevkit per its README and make sure its `bin/` is on your `PATH` (verify with
`command -v m68k-neogeo-elf-gcc` and `command -v ngdevkit-gngeo`).

---

## 2. Setup (one time)

```sh
# config.mk auto-bootstraps from config.mk.SAMPLE on the first build — no manual cp needed.
# (Only edit neogeo/config.mk if one of the tools above isn't on your PATH.)
# Put your doom1.wad somewhere; you pass its path to the build below.
```

---

### Building on Windows (WSL2 + ngdevkit)

ngdevkit is a Unix toolchain, so on Windows build inside **WSL2**:

```sh
# 1. Install WSL2 + Ubuntu (run in an Administrator PowerShell), then reboot:
wsl --install -d Ubuntu

# 2. In Ubuntu, install ngdevkit + the build deps:
sudo add-apt-repository ppa:dciabrin/ngdevkit
sudo apt update && sudo apt install ngdevkit ngdevkit-toolchain ngdevkit-gngeo \
    build-essential python3 python3-numpy python3-pil libsdl2-dev imagemagick zip

# 3. Build + run as in §3 (works from /mnt/c/... or your WSL home):
make rom-vs WAD=doom1.wad && make -C neogeo run
```

The gngeo window appears via **WSLg**. If it won't take keyboard focus, force the X11
backend: `SDL_VIDEODRIVER=x11 make -C neogeo run`. For a lag-free window you can instead
build `gngeo` natively under **MSYS2 (UCRT64)** and run the cart from `neogeo/build/rom`.

---

## 3. Build + run (VSLICE, the live free-roam engine)

```sh
make rom-vs WAD=/path/to/doom1.wad      # generate ALL DOOM-derived assets from the WAD, build the cart ROM
make -C neogeo run                       # launch in gngeo
make -C neogeo mame                      # or launch in MAME (closer to real-hardware timing)
```

`make rom-vs` runs the whole pipeline: extract map + textures + sprites + title art
(`wad2c.py`), bake the legacy floor/ceiling LUTs, extract the SFX (`wadsfx.py`), bake the
node blob, extract the live **BSP geometry for all 9 maps** (`vs_extract.py` → `vs_e1.h`),
bake the live floor/ceiling perspective LUTs (`vsfloorlut.py`/`vsceillut.py`, 64 angle-sets
over 180°) and the per-flat LUT bank (`vsflatlut.py`), then compile the cart. It's
idempotent — re-run any time you change a tool or the WAD. Subsequent engine-only rebuilds:
`make -C neogeo cart`.

**No Neo Geo BIOS needed for the gngeo run path** — `make -C neogeo run` boots on the open
BIOS bundled with gngeo, so you don't need (and must not commit) any copyrighted SNK/UniBIOS
files. A real BIOS is only required for the optional MAME path (§4).

> **If floors/ceilings/flats render spatially offset or wrapped**, the C-ROM tile-chain
> offsets are stale — run `python3 tools/fix_tile0.py && make -C neogeo cart`. The LUT
> bakers hardcode the 257-tile base prefix (logo + fix-font) so a fresh clone gets this
> right automatically; this recovery is only needed if you hand-bake out of order.

### Controls

The cart has a live tuning HUD ("debug shuttle"):

- `W`/`S`/`A`/`D` — move forward/back, turn left/right
- `SPACE` — show/hide the debug HUD (game keys are live only when it's hidden)
- `P` — cycle which parameter is selected (`>` caret marks it)
- `B` / `N` — increase / decrease the selected parameter
- `C` — fire, `D` — use (open doors / lifts / exit switches)

### Debug parameters (the dials)

| # | Tag | What |
|---|-----|------|
| 0 | `dd` | draw distance (450…1000 in 50s, then 1500…5000 in 500s) |
| 1 | `dc` | per-column see-through depth cap (layers) |
| 2 | `col` | column resolution (20/32/40/64/80) — fewer = faster |
| 3 | `cap` | wall-edge bevel mode (smooths the 16px staircase silhouette) |
| 4 | `zon` | zonal flats (per-row visplane) on/off |
| 5 | `gen` | generic mode: synthetic floor/ceiling LUT vs real flats |
| 6 | `ease` | far-cull easing gain (smooths the horizon under load) |
| 7 | `wpn` | first-person weapon select |
| 8 | `mn` | far-cull floor — how far in the horizon may pull (0…5000) |
| 9 | `mbg` | far-field backdrop colour ("the colour of nothing") |
| 10 | `fog` | wall fog-band extent % (5…75 + OFF; low = heavy near fog) |
| 11/16 | `flod`/`clod` | floor / ceiling LOD crop (drop far rows) |
| 12 | `occl` | actor-vs-wall occlusion |
| 13 | `bud` | strip budget — the hard per-frame framerate cap |
| 14 | `sky` | draw sky through window/door openings |
| 15 | `seam` | seam overdraw (flicker mask) |
| 17 | `prop` | show/hide actors (barrels/baddies) |
| 18/19 | `fmrk`/`cmrk` | floor / ceiling murk rows (3-band distance gradient) |
| 20/21 | `hgun`/`hhud` | hide weapon / hide status bar |
| 22 | `dwlk` | walk through doors without opening |
| 23 | `rad` | radial far-cull v2 (uniform-radius reach; column-closing) |
| 24 | `vmap` | vertical texture map (1:1 + DOOM pegging) vs original stretch |
| 25 | `lvl` | level select (E1M1…E1M9) |
| 26 | `pf` | perf preset: `lo` / `md` / `hi` / `ul` (applies a dial set) |
| 27 | `rset` | reset all dials to defaults |

### Tuning perf (short version)

Frame cost is bounded by what's *visible*, not by the dials, so sparse views are fast even
maxed. To hold frame in dense/open vistas: set `bud` (13) to your worst-case budget (it's the
hard backstop), keep `col` (2) at **20**, keep `dd` (0) modest and let the murk gradient +
backdrop carry distance, turn on `mn` (8) easing to pull the horizon in under load, keep `dc`
(1) low, and use `flod`/`clod` to crop far floor/ceiling rows. `pf` (26) applies all of this
as `lo`/`md`/`hi`/`ul` presets; `rset` (27) restores defaults.

---

## 4. Validate on MAME (optional, hardware-accurate)

```sh
tools/make_mame_set.sh                  # package neogeo/build/rom -> dist/mame (softlist + xml)
mame -rp dist/mame/roms -hashpath dist/mame/hash neogeo doomrails -w -skip_gameinfo
```

MAME validates the BIOS strictly; the bundled open BIOS fails its CRC check, so drop a
MAME-recognised `neogeo.zip` (real SNK set or UniBIOS) into `dist/mame/roms/` to boot. MAME
is 68000-cycle-accurate, so the cart's on-screen `fps=` readout there is the true
real-hardware framerate (it runs a touch slower than gngeo by design).

---

## 5. Project layout

**Source (committed):**

```
Makefile                  top-level build (rom-vs, assets, doomng-host, bake-nodes, run)
src/render.c, dng.h,      shared offline engine (src/map.c is GENERATED)
    fixed.h, trig.h
host/main_sdl.c           offline renderer + asset baker -> the doomng-host binary
neogeo/main.c             the cart: VSLICE live renderer, menus, HUD, debug shuttle
neogeo/doomsnd.s          Z80 sound driver
neogeo/Makefile, *.mk     cartridge build (ngdevkit)
tools/*.py                asset pipeline: wad2c, vs_extract, vsfloorlut, vsceillut,
                          vsflatlut, floorlut, ceillut, wadsfx, fix_tile0, flat_gallery,
                          + dev tools (refcap, make_mame_set.sh, mame_snap.lua, ...)
```

**Generated from your WAD at build time (git-ignored, never commit):** `src/map.c`,
`neogeo/textiles.h`, `texpal.h`, `ramps.h`, `ceillut.h`, `floorlut.h`, `sprites.h`,
`hudfix.h`, `titlepic.h`, `menu.h`, `vs_e1.h`, `vsfloor.*`, `vsceil.*`, `vsflat.*`,
`nodes*.{h,c,bin}`, `*.c1`/`*.c2` C-ROM tiles, `snd_*.wav`/`*.adpcma`, `path_nodes.txt`,
`ramps_used.txt`, `neogeo/build/`, `dist/`.

---

## 6. How the live renderer works (one breath)

DOOM's BSP gives a cheap front-to-back ordering of wall segments. The 68000 walks it,
frustum- and **occlusion-culls** subtrees whose screen columns are already filled by nearer
solid walls (DOOM's `solidsegs` idea), and projects only what's visible — usually a few dozen
of a map's hundreds-to-thousands of segments. Each visible wall is emitted as 16px-wide Neo
Geo sprite columns, vertically shrink-scaled by the hardware for depth, perspective-textured
along both axes. Floors and ceilings are baked perspective LUTs; distance is sold by a
3-band murk gradient fading to a backdrop colour, bounded by a strip budget for a steady
frame rate. See `AGENTS.md` for the architecture in more depth.

---

## License

Project code: see repository. DOOM is © id Software — **not included**; supply your own
IWAD. ngdevkit is GPL — see its repository.
