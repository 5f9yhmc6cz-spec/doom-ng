# AGENTS.md — working in this repo

Guidance for AI agents and human contributors. Read this before making changes.

## What this is

A live, free-roam renderer of DOOM Episode 1 on a **stock Neo Geo** (68000 @ 12 MHz, no
expansion). The CPU walks DOOM's BSP for visibility; the Neo Geo sprite scaler draws the
pixels. The live engine is **VSLICE** (`neogeo/main.c`). See `README.md` for the user-facing
overview and the full build/run instructions.

## Hard rules

1. **No DOOM assets in git, ever.** This repo is source-only. Every texture/sprite/map/
   sound/LUT is generated from the user's own `doom1.wad` at build time and is `.gitignore`d.
   Never commit a generated file (`textiles.h`, `sprites.h`, `vs_e1.h`, `vsflat.h`, `src/map.c`,
   `*.c1`/`*.c2`, `snd_*`, etc.) — it would redistribute id Software content.
2. **Keep it anonymous / asset-free** if preparing a public export.
3. **The cart is the constraint.** Work RAM, C-ROM tiles and the ~380 hardware sprite slots
   per scanline are all finite. Budget before adding.

## Build / run / verify

```sh
make rom-vs WAD=/path/to/doom1.wad   # full pipeline from the WAD -> cart ROM (idempotent)
make -C neogeo cart                  # engine-only rebuild (after editing neogeo/main.c)
make -C neogeo run                   # gngeo
make -C neogeo mame                  # MAME (stricter timing; closest to hardware)
```

Verification is **ride-and-judge** in the emulator via the debug shuttle (`SPACE` toggles the
HUD; `P` cycles a param; `B`/`N` adjust). There is no automated visual test — most changes to
the renderer must be eyeballed. Build cleanly first (`gmake cart`, watch for errors), then ride.

## Architecture (the VSLICE frame)

One front-to-back pass with a single distance authority and three "verbs":

1. **Per-frame setup** — resolve `dd`/`dc`/`bud` dials; ease the far-cull `g_murk_eff` between
   the `mmin` floor and the `dd` ceiling under budget pressure.
2. **BSP walk** (front-to-back) with bbox occlusion cull. For each wall fragment, exactly one of:
   - **CULL** — remove it (near plane, FOV cone, beyond far-cull, back-face, off-screen, budget).
   - **CLOSE** — mark the column done (solid wall, shut door, depth-cap layers, or out of budget);
     this terminates that column's walk.
   - **SHADE** — keep it, recolour by depth (3 wall fog bands; floor/ceiling 3-band murk gradient).
3. **Two-pass emit** — `vs_strip` *records* strips into a buffer during the walk; `vs_strip_emit`
   *bursts* them to the SCB after the walk (this is the dense-room flicker fix).
4. **Floor/ceiling** — `vs_lut` draws baked perspective LUTs (independent of the wall clip state).
5. **Backdrop** (`mbg`) fills everything not drawn.

Texturing: horizontal **wall-U** is perspective-correct (baked per-seg `VEU0`/`VEULEN`); vertical
**V** is 1:1 + tiling + DOOM pegging (`voff`), but **tile-quantized to 16px** (so sub-16px sidedef
yoffsets vanish). Radial cull (`rad`, default off) is the same `g_murk_eff` measured as a euclidean
radius per column.

## C-ROM tile chain — the main hazard

Tiles are one concatenated chain: `base-logo · logo · textiles · floorlut · sprites · ceillut ·
ramps · vsfloor · vsceil · vsflat`. Each group's `TILE0` offset is the sum of all preceding
groups' tile counts. **Growing any mid-chain group shifts every downstream offset.** The LUT
bakers recompute their own `TILE0` when re-run in order; `tools/fix_tile0.py` patches the
committed offsets (incl. embedded AVG/TITLE/MENU banks) when a re-bake isn't run. The live
floor/ceiling/per-flat LUTs are the chain **end**, so changing them needs no `fix_tile0`.

**Provenance / the 257-tile base prefix.** The chain head — `base-logo` (`base-crom-logo.c1/.c2`,
256 C-ROM tiles) and the fix-layer font (`base-srom-text-shadow.fix`, 1280 fix tiles) — is
ngdevkit's own freely-licensed example art (NOT id/DOOM content), committed under
`neogeo/assets/prebuilt/` past the `*.c1/*.c2/*.fix` ignore rules. DOOM textures therefore start
at C-ROM tile **257** (`base-logo` 256 + `logo` 1). `wad2c.py`, `floorlut.py`, `ceillut.py` — and
now `vsfloorlut.py`/`vsceillut.py`/`vsflatlut.py` + `fix_tile0.py` — **hardcode that 257**:
`build/assets/base-crom-logo.c1` is only created at the cart-link step, so on a FRESH clone it
doesn't exist when the LUT bakers run; stat-ing it returned 0 and shifted every floor/ceiling/flat
LUT 257 tiles early (offset/wrapped surfaces that still "built fine"). Hardcoding 257 makes a
first-from-clone build correct with no manual `fix_tile0`.

## Conventions

- Comments are dense and explain *why* (the cart is full of hard-won micro-decisions). Match that.
- Debug dials live in one param table in `neogeo/main.c` (`NSEL` count, the `case`/`tk[]` blocks,
  the 4-column HUD). Add a param = bump `NSEL`, add a `case`, add a `tk[]` label.
- The fix layer (status bar/HUD) only renders cols 1..38; palette slots 0..15 are system-reserved
  (`TEXBASE=16`), so HUD/murk palettes are scarce.
- Prefer illusion-first approximations the user can ride over exact-but-slow correctness.

## Gotchas

- `make -C neogeo cart` does **not** regenerate assets; it needs the generated headers present
  (run `make rom-vs` once, or the LUT `/tmp/*.raw` host bakes are purged → use `fix_tile0.py`).
- The floor/ceiling **murk** applies only in generic mode (`gen=1`); real flats override it.
- MAME rejects the open BIOS (CRC); use a real/UniBIOS neogeo.zip for MAME.
- Sprite budget is the worst-case bound; tune `bud` for the densest room, not the average.
