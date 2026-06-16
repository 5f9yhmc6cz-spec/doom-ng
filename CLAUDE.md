# CLAUDE.md

Project guidance for Claude Code. The full agent/architecture guide is **[AGENTS.md](AGENTS.md)**;
the user-facing overview and build steps are in **[README.md](README.md)**. Read both.

## The non-negotiables

- **Never commit generated, DOOM-derived files.** This is a source-only port; all assets are
  generated from the user's own `doom1.wad` and are `.gitignore`d (`textiles.h`, `sprites.h`,
  `vs_e1.h`, `vsflat.h`, `src/map.c`, `*.c1`/`*.c2`, `snd_*`, …). Committing them redistributes
  id Software content.
- **Build before claiming done.** `make -C neogeo cart` for engine changes (it needs the
  generated headers already present — run `make rom-vs WAD=…` once first). Report real build
  output; don't assert success you didn't see.
- **Renderer changes are ride-and-judge.** There's no automated visual test — verify in the
  emulator via the debug shuttle (`SPACE` = HUD, `P` = cycle param, `B`/`N` = adjust). Propose
  the plan, make the change, and hand it back for a ride.

## Fast facts

- Live renderer: `neogeo/main.c` (VSLICE — BSP walk → cull/close/shade → two-pass strip emit →
  floor/ceiling LUT → backdrop). Offline baker/host: `host/main_sdl.c` + `src/`. Pipeline:
  `tools/*.py`.
- **C-ROM tile chain footgun:** tiles are one concatenated chain with baked `TILE0` offsets;
  growing a mid-chain group shifts everything downstream. Re-run the bakers in order, or use
  `tools/fix_tile0.py`. The live LUTs are the chain end (safe to change).
- Debug dials live in one param table in `neogeo/main.c` (`NSEL`, the `case`/`tk[]` blocks).
- Palette slots 0–15 are system-reserved (`TEXBASE=16`); the fix layer shows cols 1–38 only.

See [AGENTS.md](AGENTS.md) for the architecture, conventions and gotchas in full.
