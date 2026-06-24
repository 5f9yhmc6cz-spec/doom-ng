# DOOM-NG — free-roam DOOM on a stock Neo Geo

A live, free-roam renderer of **DOOM Episode 1** (all nine maps, E1M1–E1M9) running on an
unmodified Neo Geo (Motorola 68000 @ 12 MHz, no expansion hardware). You walk and turn
freely; each frame the 68000 walks DOOM's original **BSP tree** to find what's visible and
projects it, and the Neo Geo's **hardware sprite scaler** draws the textured wall columns.
The CPU does visibility; the sprite hardware does the pixels. No per-pixel software
rasterisation on the CPU at all.

The live renderer is called **VSLICE** (a BSP walk that emits the scene as vertical hardware
sprite strips). Build it with `make rom-vs`.

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
| **Python 3.11+** with **Pillow** | the asset pipeline (`tools/*.py`) + ngdevkit's `tiletool.py` (needs 3.11's `typing.Self`); see `requirements.txt` | your OS / pip |
| **C compiler + make + SDL2** | builds `doomng-host`, the offline renderer/baker the pipeline drives | your OS (`gcc`/`clang`, `libsdl2-dev` / `sdl2`) |
| **ImageMagick** (`magick`) | generates the attract-mode logo tile during the cart build (and `make dump`) | your OS (`imagemagick`) |
| **`zip` / `unzip`** | repack the gngeo run datafile (`make -C neogeo run`) | usually preinstalled |
| **`doom1.wad`** | the IWAD — you supply it (see above) | shareware / your DOOM install |
| MAME *(optional)* | cycle-accurate hardware validation | https://mamedev.org |

Install ngdevkit per its README and make sure its `bin/` is on your `PATH` (verify with
`command -v m68k-neogeo-elf-gcc` and `command -v ngdevkit-gngeo`).

> **Heads-up (macOS especially):** make sure `python3 --version` reports **3.11 or newer** — ngdevkit's
> `tiletool.py` imports `typing.Self`, so an older default (e.g. Xcode's Python 3.9) fails the cart build
> with `ImportError: cannot import name 'Self'`. If you have a newer Python from Homebrew, put its `bin`
> ahead of `/usr/bin` on your `PATH` (or `brew install python`).

---

## 2. Setup (one time)

```sh
cp neogeo/config.mk.SAMPLE neogeo/config.mk   # toolchain config (edit only if a tool isn't on PATH)
# put your doom1.wad somewhere; you pass its path to the build.
```

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

> **Audio:** sound effects are extracted from your WAD automatically. The **music** is
> bring-your-own — the cart ships a *silent* soundtrack loop until you supply MIDIs + a soundfont
> and run `sh tools/genmusic.sh` (`SF=/path/to.sf2 MIDIDIR=/path/to/midis` to point it at yours).

### Controls

The cart has a live tuning HUD ("debug shuttle"). Game keys and tuning keys never overlap:
game keys work only while the HUD is hidden, tuning keys only while it's shown.

(Keys below are the default gngeo keyboard mapping from `make -C neogeo run`.)

**Playing (HUD hidden):**

- `W`/`S` — move forward / back  ·  `A`/`D` — turn left / right
- `N` — fire  ·  `M` — use (open doors / lifts / exit switches)
- `P` — cycle first-person weapon
- `SPACE` — show the debug HUD

**Tuning (HUD shown, `SPACE` toggles it):**

- `W`/`A`/`S`/`D` — move the `>` caret around the parameter grid
- `N` / `M` — decrease / increase the selected dial
- `P` — toggle the BSP-walk visualiser (watch the live front-to-back sweep)
- `SPACE` — hide the HUD (back to playing)

### Debug parameters (the dials)

The HUD exposes ~50 tuning dials on a navigable grid. The ones you'll actually reach for are
below, with their shipped defaults; the **full reference is in
[`neogeo/DEBUG_PARAMS.md`](neogeo/DEBUG_PARAMS.md)** and live on the in-cart HUD. Reset all
dials to these defaults with `cf` → `rset`.

| Tag | What | Default |
|-----|------|---------|
| `dd` | draw distance, world units (0 = fog-only … 32000 ≈ whole map) | 750 |
| `dc` | per-column see-through depth cap (layers) | 5 |
| `col` | column resolution (20/32/40/64/80) — fewer = faster | 32 |
| `gen` | synthetic blanket floor/ceiling (1) vs real per-flat LUT (0) | 1 |
| `ease` | far-horizon trim 0…16 — pulls the draw horizon inward | 16 |
| `wpn` | first-person weapon select | — |
| `mn` | far-cull floor — how far in the horizon may pull | 150 |
| `mbg` | far-field backdrop colour ("the colour of nothing") | 1 |
| `fog` | wall fog-band extent % (5…75 + OFF; low = heavy near fog) | 35 |
| `flod` / `clod` | floor / ceiling LOD crop (drop far rows) | 0 / 1 |
| `bud` | strip budget — the hard per-frame framerate cap | 300 |
| `cmap` | colmap floor+ceiling depth-fade dim (1) vs full-bright (0) | 1 |
| `prop` | show / hide actors (baddies + barrels) | 1 |
| `hmap` / `vmap` | per-axis perspective texture mapping, 0…3 | 2 / 2 |
| `lvl` | level select (E1M1…E1M9) | — |
| `cf` | perf-preset config select / reset (`rset`) | — |
| `ncul` | node far-cull — prune BSP subtrees past the horizon | 1 |
| `nclp` | near-clip plane | 16 |
| `bxc` | BSP walk budget — cap node-box tests (bounds worst case) | 100 |
| `gov` | graceful governor: target fps; recedes the horizon under load | 5 |

### Tuning perf (short version)

Frame cost is bounded by what's *visible*, not by the dials, so sparse views are fast even
maxed. To hold frame in dense/open vistas: `bud` is the hard backstop — set it to your
worst-case budget; drop `col` toward **20**; keep `dd` modest and let the murk gradient +
backdrop carry distance; turn `ease`/`mn` up to pull the horizon in; keep `dc` low; use
`flod`/`clod` to crop far floor/ceiling rows; and dial `bxc` down on node-dense maps to cap
the BSP walk. `gov` automates the trade — pick a target fps and it recedes the horizon under
load and extends it with headroom. `cf` applies whole preset sets; `cf` → `rset` restores the
shipped defaults above.

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
`hudfix.h`, `gunhand.h`, `titlepic.h`, `interpic.h`, `menu.h`, `vs_e1.h`, `vs_geo.bin`
(banked BSP geometry), `vsfloor.*`, `vsceil.*`, `vsflat.*`, `*.c1`/`*.c2` C-ROM tiles,
`snd_*.wav`/`*.adpcma` (SFX), `*.adpcmb` (music), `path_nodes.txt`, `ramps_used.txt`,
`neogeo/build/`, `dist/`.

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
