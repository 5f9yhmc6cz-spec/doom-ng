# DOOM-NG — Doom for the Neo Geo

*Design notes — draft. This is a living document.*

> ⚠️ **HISTORICAL — pre-VSLICE draft.** This captures the original plan (E1M1-only, flat-shaded
> floors, mac/Homebrew framing) and does **not** describe the shipped engine. The live cart is
> **VSLICE**: all nine E1 maps (E1M1–E1M9) with baked perspective floor/ceiling LUTs. For current
> reality see **README.md** and **AGENTS.md**; kept here for the original design rationale only.

## Goal

A recognizable, playable **Doom** on the SNK Neo Geo (AES/MVS) — built from scratch.
Not a byte-for-byte id port; a Doom that *feels* like Doom on hardware that was never
meant to render it.

**In scope:** first-person movement, arbitrary-angle BSP level geometry, texture-mapped
walls, billboarded scaled enemies/items, distance/sector lighting, doors & lifts.
**Compromised early, revisited later:** floors/ceilings (flat-shaded first).
**Likely out of scope:** full id map parity, perspective-perfect floor/ceiling texturing.

**Target scope: E1M1 ("Hangar") only — done to the nines** (geometry from the freely
distributable Doom shareware WAD). Nobody benchmarks "but does it run E1M2"; the iconic
first level *is* the proof. One level dissolves the two hardest problems (see Memory) and
inverts the C-ROM budget from a constraint into a *luxury* — the entire cartridge spent on
one level's textures, monster frames, pre-baked angle panels, and light ramps — polished, not
blocky.

## Design principle: stylized projection, not linear perspective

Doom is fake 3D. Super Scaler games are fake 3D. People *love* both. Euclidean linear
perspective is a *self-imposed* constraint — and it's the one that generates the wall-shear
problem. Drop it. The projection is designed *around* what the hardware does cheaply (scale whole
sprites) and tuned by eye for feel, not CAD-correctness.

This is permission the genre has always taken:
- Doom's monsters/items are flat billboards snapping between 8 discrete angles — iconic, not
  broken. **Angle-snapped *wall* panels are the same trick, extended to architecture.**
- Doom shears the whole screen to fake looking up/down, can't do room-over-room, fakes all
  lighting. It's fakery all the way down — and the best-feeling 3D of its era.
- Super Scaler games hand-tuned their distance→size curves for drama/readability, not 1/d.

**Levers bent freely:** distance→size curve (compress the far field for visibility); a
little cylindrical/fish-eye curvature (reads as "retro 3D", kills per-column shear);
angle-snapped pre-baked wall panels; depth quantized into slabs.

**Discipline kept (so "stylized" reads as intentional, not glitched):**
- *Temporal stability* — scale & position vary smoothly as the player moves; swimming/jitter
  is the real enemy, not wrong angles.
- *Monotonic depth* — nearer is always bigger; occlusion order correct (BSP earns its keep).

Consequence: this is an *aesthetic* target, only dialable by eye on running hardware. **M1 is
a live "projection sandbox,"** not a math exercise.

## The core idea: superscaler, not framebuffer

The Neo Geo has **no framebuffer** and cannot plot arbitrary pixels. Every attempt to
"misuse the sprite engine as a pixel renderer" dies here. So that approach is dropped.

Doom is *already* a column renderer: for each screen column it draws a vertically-scaled
texture strip. A Neo Geo sprite *is* a 16px-wide vertical strip with an independent
vertical-shrink value. The shapes match. So each frame the 68000 computes only
**per-column control words** (distance → V-shrink, texture-U → tile, light → palette) and
writes Sprite Control Blocks. The **LSPC hardware scaler fills every pixel for free.**
That is what makes this tractable on a 1.75-MIPS 68000 with no blitter.

### Superscaler ≠ raycaster — and the hardware picks the answer

These are different paradigms, not one knob:
- **Raycaster** decomposes *surfaces* into one thin vertical strip per screen column.
- **Super Scaler** (Space Harrier / OutRun) scales *whole objects* and never strip-renders
  a wall at all.

Which applies is decided by one hardware fact: **the Neo Geo can only *shrink* sprites —
no rotation, no shear, no affine.** A perspective vertical surface seen at an angle needs
horizontal *shear*; with no shear, the only representation is to slice it into vertical
bands, each needing only a vertical scale. That slicing is a geometric consequence of
perspective on shear-less hardware — *not* "secretly a raycaster."

So the split (this honors "Neo Geo can have huge sprites"):
- **Objects (things: enemies, items) → pure Super Scaler.** Camera-facing billboards need
  only uniform scaling — the hardware's home turf, with *huge* sprites. Likely better than
  the GBA here.
- **Walls → superscaler-first, band only when forced.** A wall you're *facing* needs ~no
  shear and ~uniform scale → render as one / a few **huge** scaled sprites (cheap, big).
  Only as a wall *rakes* away does shear error grow, forcing finer vertical banding. Band
  width is the single knob — for walls only, and only because of the missing shear.

**Binding limit is 96 sprites / scanline, not 380 / frame.** Head-on walls as huge sprites
cost very few per line, leaving budget for the superscaler objects.

### Why the GBA 3D playbook doesn't transfer (it sharpens the plan)

The obvious references — SM64 on GBA, Driv3r / V-Rally 3 / Asterix XXL — all lean on two
things the Neo Geo **does not have**:
- **A writable framebuffer.** SM64-GBA is a from-scratch *software rasterizer* plotting
  pixels into GBA bitmap mode — and still only hits 5–15 fps on a faster 16.78 MHz 32-bit
  ARM with ~4.5× the RAM. The Neo Geo has *no* writable pixel buffer: all graphics are read
  by the video chip straight from cartridge ROM; the 68000 can never write a computed
  pixel. This is *why* Doom is "impossible" here, and why SM64-GBA can't be lifted over.
- **Affine transforms (Mode 7 + affine sprites).** The GBA racers fake the ground plane
  with per-scanline affine background matrices and rotate/scale sprites. The Neo Geo has no
  affine background and shrink-only sprites.

Conclusion: the Neo Geo is *less* 3D-capable than the GBA in the two ways those games won —
so those approaches can't be imitated. The one axis where Neo Geo beats the GBA:
**massive numbers of large, cheaply shrink-scaled sprites.** Hence *more* superscaler, not
a software renderer. Take the GBA lesson as *optimization intensity*, not architecture.

## Renderer architecture: BSP + adaptive superscaler

1. **BSP traversal, front-to-back.** Gives correct draw order for a system with no
   z-buffer: assign sprite priority in traversal order.
2. **Column clipping (Doom's solidsegs / ceiling+floor clip) on the CPU.** Determines each
   column's *visible* vertical extent. Fully-occluded walls emit **zero** sprites — this is
   the main lever for staying under 96/line, not just for ordering.
3. **Superscaler-first walls.** BSP gives each seg's screen-space slope before emitting:
   - head-on segs → one / a few **huge** scaled sprites (cheap, big — true superscaler)
   - raking segs → finer vertical bands (the missing shear, paid down in sprite count)
   Use the huge-sprite capability wherever shear error is small; band only where it isn't.
4. **Emit chunk-sprites:** per visible chunk set X = screen pos, V-shrink = height,
   H-shrink = perspective width, tile = texture region, palette = light level.

## Subsystems

- **Enemies / items — the easy win.** Billboards with 8 rotations + distance scaling is
  *exactly* what the Neo Geo does in every game. Store frames in C-ROM (huge — hundreds of
  Mbit), let hardware scale. Depth-sort via sprite priority. Store at max on-screen size
  (shrink-only hardware) and scale down.
- **Lighting ≈ Doom COLORMAP, 1:1.** Each sprite (SCB) selects 1 of 256 palettes. Pre-bake
  ~8–16 brightness ramps per texture; pick palette per column by distance + sector light →
  free per-column diminished lighting.
- **Floors / ceilings — the hard mismatch** (horizontal spans vs. vertical sprites). The
  GBA racers solve this with a Mode-7 affine ground plane; **there is no affine background**,
  so that option is off the table.
  - v1: flat-shade per sector (solid color, distance-darkened via palette). Very "retro."
  - later: per-scanline raster tricks via timer IRQ for faked textured floor.
- **Memory — the real boss, not CPU.** Only 64KB work RAM; a full Doom level's runtime
  structures are far larger.
  - All **static** geometry (BSP, segs, linedefs, sidedefs, vertexes, textures) lives in
    **cartridge ROM**, read-only.
  - Only **mutable** state in the 64KB: door/lift positions, sector heights, thing
    states, player. ("ROM-delta" model.)
  - A custom **WAD→ROM compiler** emits a compact format, not id's runtime layout.
  - **Single-level scope makes this tractable now:** E1M1's mutable state is tiny and
    hand-verifiable, and the compile step (`tools/wad2c.py`) handles *one* known level —
    no general-purpose edge-case handling needed yet.

## Hardware constraints (reference)

- CPU: Motorola 68000 @ 12 MHz (~1.75 MIPS). Slow MUL/DIV → precompute reciprocal /
  yslope / finesine / tantoangle tables in ROM (Doom already does most of this).
- Work RAM: **64 KB**. VRAM: 64 KB (sprite control blocks).
- No framebuffer. No tile *background* scroll layer (only the fix layer for HUD/text).
- Sprites: vertical strips of 16×16 tiles, up to 32 tiles tall (16×512). **380/frame,
  96/scanline.**
- Scaling: vertical 8-bit (256 levels), horizontal 4-bit (1–16px). **Shrink only — never
  enlarge.** V-shrink propagates through chained sprites; **H-shrink does not** (distribute
  manually).
- Palette: 4096 on-screen from 256 × 16-color palettes; per-sprite palette select.
- Resolution: 320×224 (≈304 usable wide).

## Open risks / things to measure (not theorize)

1. **Texture-U addressing (M1 blocker).** How does H-shrink pick source columns? Likely
   need textures stored transposed / pre-sliced in C-ROM. *Measure on emulator first.*
2. Floor/ceiling fidelity vs. flat-shaded acceptability.
3. Worst-case sprites/scanline in real level geometry.
4. Level mutable-state footprint vs. 64KB.
5. 68000 frame budget for BSP traversal + clip + SCB writes at target column count.
6. **Pre-baked angled walls (fork to race at M1).** Can't shear at runtime, but C-ROM is
   huge — bake perspective-sheared wall art for a discrete set of angles and place whole
   scaled "panels." Trades abundant ROM for the missing shear. Limits: angle discretization
   (snapping) and perspective ≠ uniform scale across a panel (likely degrades to a few
   pre-sheared bands per wall). Prototype against plain runtime banding; keep the winner.

## Milestones

*Status: host sandbox renders FULLY TEXTURED E1M1 — perspective-correct
**walls** + per-sector floor-cast **floors & ceilings** (54 textures + flats baked into a
425 KB "C-ROM"), via the WAD's prebuilt BSP. Free-look camera + 4 view modes (1st/2nd/3rd/
top-down automap), **real monster/item sprites** (WAD art, transparency, depth-sorted,
distance-scaled; single-rotation), **collision + step physics** (BSP floor-follow, 24u step
rule, wall-slide; walk/fly toggle), and **Z-to-shoot** hitscan that kills monsters. Doom.
**Caveat:** host floor-cast is
per-pixel (the ideal); real Neo Geo floors become coarse depth-bands — the true floor budget
is the open hardware question (per-column proxy currently reads 64/96). Pipeline:
`tools/wad2c.py` -> `src/map.c` + `e1m1.crom`. Remaining: the **ngdevkit on-hardware
backend** (next — fixed-point port, real SCB emission, the floor depth-band reckoning, run
in gngeo), then 8-rotation sprites, palette lighting (strobe rooms), weapon sprites/HUD.*

- **M0** Toolchain up (ngdevkit); minimal ROM boots and shows a sprite in gngeo.
- **M1** One spinning **textured wall** — validate texture-U + V/H-shrink pipeline. *(crux)*
- **M2** Single BSP sector room: walk/turn, flat floor+ceiling, palette-lit walls.
- **M3** Multi-sector BSP level, variable heights, column clipping, adaptive chunks.
- **M4** Enemies/items: hardware-scaled billboards + depth sort + budget management.
- **M5** Compile **E1M1** via `tools/wad2c.py`; two-sided walls, doors/lifts; ROM-delta RAM model.

## Toolchain

- **ngdevkit** (dciabrin) via Homebrew tap — m68k-neogeo cross-gcc, gngeo emulator,
  sprite/ROM tooling. No Docker on Apple Silicon.
- Target test: gngeo emulator → later real hardware / MiSTer.
- Language: C + 68000 asm for the hot path (SCB emission, fixed-point math).

## Prior art (study, not fork — built from scratch)

- **NGRayEx** (lantus) — proves the column-sprite + V-shrink pipeline at Wolf3D grade.
- Sega Super Scaler (Space Harrier / OutRun) — the chunk-sprite pseudo-3D technique.
- NeoGeo Dev Wiki — Sprites, Sprite shrinking pages.

## Lessons stolen from other "impossible" ports

- **ZX Spectrum FPS / raycaster** (~80 fps): no trigonometry at all — everything is LUTs;
  *differential rendering* ("do nothing unless absolutely necessary") — skip columns that
  didn't change, update only edges/color when they barely changed; as few as 32 ray columns
  still read as 3D. **Direct steal:** SCBs persist in VRAM, so only rewrite the control
  blocks that actually changed between frames → near-zero 68000 cost when the view is static
  or panning slowly. Chunky column counts are fine — far more sprite budget is available.
- **NES "Mode 7"** (z80artist et al.): software-renders transforms into **CHR-RAM**
  (writable character memory) — the NES's escape hatch. The Neo Geo's specific limitation is it
  has *no* writable character memory, so the only lane is pure hardware sprite scaling, not
  software charmem rendering. *Open item: confirm no Neo Geo cart mapper exposes writable
  character RAM — if one did, a hybrid software path reopens.*

## Performance model (host-measured op counts -> 68000 estimate)

The renderer counts its per-frame work. Worst E1M1 spawn view (~275 visible segs):
~2200 perspective divides (→ ~550 if `1/depth` is cached per seg-endpoint; near-free with a
reciprocal LUT), ~900 vertex transforms, ~80 wall bands, ~190 sprite-control writes.
**There is no per-pixel term** — the LSPC fills every wall/floor pixel in hardware.

68000 @ 12 MHz = 200k cyc/frame @60fps, 400k @30. Estimate: **~12–15 fps** naive
fixed-point; **~30 fps** with the standard wins (cache reciprocals / reciprocal LUT,
transform each vertex once, differential SCB updates). Estimate only — confirm via the
gngeo/hardware port. Video budget holds even counting flats as sprites: worst scanline
64/96; textured/skewed floors eat the remaining ~32/line headroom.

## Signature techniques ("eurekas")

1. **The scaler deletes Doom's hot loop.** Software Doom is bound by per-pixel fills; all of it
   is offloaded to the LSPC and the 68000 does only geometry. The Neo Geo's missing
   framebuffer is *why* a 12 MHz CPU suffices.
2. **Hardware interpolates perspective for free** — divides per wall-endpoint (~2/seg), not
   per screen column.
3. **Lighting is a palette index, not a blend** (= Doom COLORMAP). Dynamic light (weapon
   flash, Hangar's strobe sectors) = swap a palette bank — free.
4. **Translucency without alpha:** checkerboard dither + 60 Hz odd/even flicker.
5. **Fake Mode-7 floors:** horizontal depth-bands of tiled scaled sprites scrolled by player
   position; ceiling = floor mirrored; prebake skew strips per angle into C-ROM.
6. **Impostor LOD:** a distant room is one pre-baked scaled sprite, not 20 slivers.
7. **Don't re-render:** SCBs persist; recompute a seg only when its projection moved ≥1px;
   re-walk the BSP only when crossing a node.
8. **C-ROM is a ~700 Mbit cache** for anything CPU can't afford — and one level to spend it
   all on. The unifying move: trade abundant ROM for scarce CPU/RAM/shear/alpha.

## Hardware backend (ngdevkit) — status & plan

**Pipeline proven:** the installed ngdevkit toolchain builds a real, loadable
Neo Geo ROM — m68k-gcc 11.4 + z80 sdcc + asset tools (tiletool/paltool/
romtool) + bundled BIOS (`neogeo.zip`) + gngeo. Verified by building ngdevkit-examples
`02-sprite` → P-ROM/C-ROM/M-ROM/S-ROM + romset zip. toolchain → ROM → emulator works.
Note: needs GNU `make` 4+ (`brew install make`), not macOS's 3.81.

**Headless caveat:** `gngeo --bench=N` (run N frames, report avg fps) is the intended
headless perf probe, but it needs a display to terminate — hangs in a headless context
here. Get fps by running `make gngeo` with a display, or build a dedicated bench harness.

**Port steps (the renderer -> Neo Geo):**
1. [DONE] `neogeo/` is a standalone ngdevkit project that builds the ROM. `neogeo/main.c`
   animates SCB2 vertical-shrink to prove the LSPC scaler on target. Build env: `PATH` with
   brew python3, `PKG_CONFIG_PATH=/opt/homebrew/opt/ngdevkit/share/pkgconfig`, then `gmake`.
   SCB map learned: SCB1=tiles, **SCB2=zoom (the engine's scale)**, SCB3=Y+height, SCB4=X.
2. [DONE] Fixed-point (16.16) core math behind `USE_FIXED` (`src/fixed.h`): a `real` type
   (float by default / Q16.16 with `-DUSE_FIXED=1`), `rmul`/`rdiv` with int64 intermediates,
   and a projection that clamps the coord/depth ratio so 16.16 can't overflow off-screen.
   **A/B verified:** float vs fixed render the same E1M1 frame (~5% pixels differ = sub-pixel
   rounding, visually identical). Build the fixed one with `make doomng-host-fixed`.
3. [BUILDS] SCB backend: `neogeo/main.c` drives `render_world`'s DrawList onto real SCBs
   (SCB1 tiles, SCB2 zoom=engine scale, SCB3/4 Y/X). Core compiled fixed-point for the 68000
   and **linked into a ROM that fits 64KB** — needed: pack SpriteCmd to int16, MAX_CMDS
   16384->1024, sector arrays 2048->256, const sine table + integer sqrt (no float libm on
   68k). **visually confirmed on hardware**: wall/geometry positions land correctly in gngeo
   (SCB Y/X/zoom verified by eye); now rendering the full color-coded flat-shaded scene
   (gray walls, brown floors, blue ceilings, red things, distance-shaded via palette).
   Iteration relied on a visual display, since gngeo won't run headless in this environment.
4. [STARTED] Art pipeline. **Proven on host:** per-texture 16-colour quantization (`wad2c.py`,
   54/76 textures reduced) keeps Doom's gritty look fully intact — the Neo Geo's 256 palettes
   hold one 16-colour set per texture. Remaining: pack the quantized textures into C-ROM as
   16px tile-columns + emit each texture's 16-colour palette (+ ~4-5 distance-darkened ramps,
   ~50 tex x 5 < 256 palettes); SCB picks tile-column (texture-U) + palette+ramp.
5. Floors as depth-bands (the per-pixel host cast won't port) -> confront the real budget.
6. Bench in gngeo; iterate to playable fps.

HUD (status bar) and music + SFX (Z80 + YM2610 via nullsound / furtool) come after the renderer.

**Recent:** `F_SKY1` sky now renders — the real SKY1 mountain backdrop, scrolled
by view angle (vertical strips, no perspective: the one effect *more* sprite-friendly than
floors). Ceilings flagged `ceiltex = -2`; `SKY_TEX` holds the graphic.
**Doors [DONE]:** `U` use-ray (64u) opens the door you face — open/wait/close, mutable
sector ceiling animated in `world_update`, and collision reads the *live* ceiling so you
pass only once it's open. The canonical ROM-delta feature, proven (static geometry in ROM,
the moving ceiling in the 64 KB). Sky scroll quartered for a distant-backdrop feel.
**Fixed-point port** (when the hardware backend resumes) will use a `USE_FIXED` A/B compile
switch so "identical to the float renderer" is a provable claim, not a vibe.
