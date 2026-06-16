# DOOM-NG — outstanding features

Live free-roam DOOM E1 on a stock Neo Geo (VSLICE engine). This is the running list of
what's *not yet done* — so it doesn't have to live in chat. Roughly priority-ordered.

## In progress / next
- **"Caps" — stair-stepped wall-top silhouettes** (column quantization, not texturing). Each
  16px strip has a flat top so sloped wall tops staircase. Lever = the `col=` dial (narrower
  = smoother, more strips). Consider a cheaper silhouette smoothing pass.
- **Flats v2** (#45) — per-zone flat selectivity + ceiling-flip review. NEXT.
- **±90° LUT — mirror variant** (optional follow-up). The 180° rebake (shipped) is a 180°
  ROTATIONAL fold; the author's "vertical symmetry" is a left-right MIRROR fold, which needs column
  reversal on the multi-sprite floor. Do only if the rotational period reads wrong on ride.

## Polish
- **HUD ARMS panel: light up owned weapons** (weapon-select values brighten for owned, grey
  for not — currently a static panel).
- **Fog: subtler falloff.** Wall fog is 3 palette-shade bands (L0/L1/L2); more bands would
  need more per-texture palette variants (slots are maxed). Dial (param 10) tunes onset/extent.

## Performance
- **1% lows — further levers.** Radial cull is DONE (below). Remaining: collapse `dd` once the
  murk-gradient far-field carries the distance (fewer segs everywhere); wire that as the default.
- **Dynamic sentinel.** Damped auto-tuner that nudges dd/budget to hold 60 (hysteresis +
  dead-band so it settles instead of hunting).

## Deferred features
- **Level-end (intermission) tally screens** between maps (kills %/time; items/secrets/par
  aren't tracked by the live engine — would be omitted or faked).
- **Door see-through top-clip** — baddies' heads show through a half-open door; needs a
  per-column top occluder + a clip-aware actor test.
- **Soundtrack E1M2–E1M9** (the .adpcmb recipe is recovered; per-map track growth).
- **Floor/ceiling scroll motion-valence** (#40) — kill the jamiroquai reversal (scroll dir).

## Structural / cleanup
- **Remove the dead on-rails engine** (#44) — frees a P-ROM bank (P2) + reclaims C-ROM now
  that VSLICE is the live renderer.
- **Status bar: 3 rows high, moved up** (#43) — reclaim a render row for the play view.
- **Flats v2** (#45) — per-zone flat selectivity + ceiling-flip review.
- **Pipeline consolidation** (from the visibility synthesis) — one `murk_reach[]` authority,
  `CULL`/`CLOSE`/`SHADE` naming, one `close_column()` helper. Pure clarity, ~no runtime cost.

## Done this session (reference)
C-ROM chain-shift corruption fix + `fix_tile0.py`; corpses (all baddies); col20 / dd / murk
dials; sprite garbage-trail; title centre → stretch-to-224 → palette-cap; corrupted-menus fix;
HUD full-width edge sprites + composed yellow numerals + digit-grey match; door triggers
(walk + switch, tag system); **perspective wall-U texture mapping**; 2-layer floor/ceiling
murk gradient; mmin 0–2000 range; fog dial 35–75; **V companion (1:1 vertical map + tiling +
clip-aware peg)**; **radial draw-distance cull** (now a toggle, param 23, default off — net-slower);
**V companion stage 2a+2b** (upper peg-bottom + full DOOM pegging: sidedef yoffset + DONTPEG
flags baked, off-by-one-tile bug caught by adversarial verify); debug HUD row-11/12 freed;
**V-map toggle (param 24, default on)** to A/B the companion vs original stretch; **4-col debug
HUD** (fits all 26 params, bottom row 9); **bazooka+minigun lowered 2 rows**; **radial cull v2**
(column-close, param 23); **V-map toggle** (param 24); **floor/ceiling LUT → 180°** (FLNA/CLNA 64,
2-fold). ARMS lighting SKIPPED (the author's call — too entangled with the baked HUD). *(All pushed; awaiting ride.)*
