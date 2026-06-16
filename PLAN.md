# PLAN — RESTART: maximum quality first, graceful degradation second

the author's directive (Jun 12, late): the ablation rig proved the runtime innocent — all nine
subsystems off changes almost nothing. The degradation lives in the BAKED DATA / pipeline.
Park (tag parked-v67-rig), review the build process unbiased, rebuild one rigorous step at
a time: MAXIMUM possible quality first ("I need to see a good image"), then degrade the
right way, knob by knob, with eyes at every step.

## Step-1 findings (done)
- The spawn four-way (/tmp/audit/spawn_4way.png): refcap vs HOST IDEAL — the renderer is
  GOOD, near-DOOM. The two "cart-faithful" record panels looked near-empty — but the REAL
  CART at the same spot draws the full room: **draw_nodeview (the host preview) is out of
  v2 parity and is NOT EVIDENCE.** Fix or retire it early in the rebuild.
- MAXQ=1 env gate (committed): budgets uncapped, du cap off, fog/light folds off — the
  one-diff inventory of bake compromises. Default byte-identical.
- A micro-rail (R=700) parks instantly (spawn sits at its end) — step 2 needs a ridable
  span.

## The compromise inventory (what MAXQ turns off / what stays)
Bake: WALL 88 / FLAT 64 strip budgets; du cap ±32 (71% of strips compression-starved — a
prime "buggered texture" suspect); fog/light dep folds; NODE_S=5 spacing; NA=24 bins;
T_DRAWDIST; CULL_MINW=2. Cart (not MAXQ-gated): 16px chunk granularity (hw), AVG far-LOD,
ramp caps, murk palettes, FOG_HORIZON_CULL=0. Path: centring shifts; min-run-5 PATHANG.

## The steps
2. RIDABLE MAXQ RAIL: bake R≈1300 MAXQ (spawn-start span the dolly actually rides; ROM
   irrelevant — 2-3 banks is fine). Cart capture; judge frame-by-frame against refcap at
   decoded coords. THIS is "the good image" on real silicon. Iterate here until the author signs
   off the stills.
3. FULL-RAIL MAXQ: if over 8 banks, accept it for ASSESSMENT builds (or bake in halves) —
   the goal is the quality ceiling on the whole level, not shippability.
4. GRACEFUL DEGRADATION: reintroduce ONE compromise at a time (budgets → du cap → murk →
   density), capture + eyeball at each, STOP at the first visible step-down and negotiate
   it deliberately. Every step gets a tagged build.
5. Only then: re-評価 the runtime (tween/defer/pace) ON TOP of signed-off data.

## the author's two standing questions (answered in-session, recorded here)
- Texture reprojection on multi-tile blocks: per-16px-chunk perspective-correct u via
  persp_u + 16 sub-tile phases; WITHIN a chunk the column is flat (one phase, du linear).
  No vertical magnification (hw). du cap ±32 truncates oblique compression on 71% of
  strips (stretching) — MAXQ uncaps to ±127 (record field limit).
- Lighting: ALL baked (fog level + folded sector light in the dep byte at bake). Runtime
  touches: nukage palette cycle, murk backdrop, ceiling murk-fade row. Nothing dynamic
  per-frame; two visits to one view are pixel-identical.

## Standing constraints (unchanged)
RAM stack gap ≥5KB; grep the SIX flags (TWEEN/HOSTILE/TITLE_FLOW/AUTOPILOT/DEFER_FAR/
SIGCHECK) before measuring; offline twins can't verify speed-changing edits (SIGCHECK
readback is ground truth); the F-counter/PT/PAD tells for hostile-input contamination;
fixed/sticky slots everywhere (cursors cascade); `git checkout` never touches files with
uncommitted features.
