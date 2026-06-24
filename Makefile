# Host prototype (the "projection sandbox"). The Neo Geo/ngdevkit backend
# will live under neogeo/ and reuse src/ unchanged.
CC      = cc
CFLAGS  = -O2 -Wall -Wextra -Isrc $(shell sdl2-config --cflags)
LDFLAGS = $(shell sdl2-config --libs) -lm
SRC     = src/render.c src/map.c host/main_sdl.c

doomng-host: $(SRC) src/dng.h src/fixed.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

# 16.16 fixed-point build (what the 68000 runs) -- A/B against the float build
doomng-host-fixed: $(SRC) src/dng.h src/fixed.h
	$(CC) $(CFLAGS) -DUSE_FIXED=1 -o $@ $(SRC) $(LDFLAGS)

# Node-render cluster bake -> neogeo/nodes_data.h (gitignored; regenerate before a cart build).
# NODE_R = cluster radius: bigger = larger play area, but more P-ROM. With ROM2 bank-switching
# (NODES split across up to 7 banked P2 banks) the ceiling is the full 8MB: ~271 nodes at R=290
# = 7 banks. Past that, coarsen NODE_S or slim per-view data. NA fixed at 36 (floor/ceiling LUTs).
# On-rails defaults: R=4000 reach, S=12 world-units between baked viewpoints. S=12 (was 24) halves
# the spatial "hop" between snapped wall-views for smoother walking (~454 nodes, ~6 of 7 P2 banks).
NODE_R ?= 4000
# SHIPPING DENSITY: S=5 x NA=24 = ~1139 nodes x 24 angles = all 8 ROM2 banks (the density trade:
# forward cadence over rotation granularity). The resample cap (1300) and LUT angle counts are
# already matched to these values -- change all three together or not at all.
NODE_S ?= 5
bake-nodes: doomng-host
	./doomng-host --bakec $(NODE_R)   # ramps_used.txt manifest ONLY (the ramps anchor needs it). --bakeS/nodes.bin dropped: the live engine never read NODES, and the dead 8MB P2 pack is removed.
.PHONY: bake-nodes

# CRITICAL: the texture extract only runs WITHOUT --ramps -- `wad2c --ramps` sys.exits before it.
# Running ONLY --ramps silently leaves stale textures (the footgun that can hide a texture-opacity
# hole-fix). rom-vs runs BOTH passes so textures and ramps stay in sync.
WAD ?= doom1.wad

# THE build: the LIVE free-roam BSP renderer -- the only engine. From a user-supplied IWAD
# (shareware doom1.wad works): map + textures + sprites + title + SFX + floor/ceiling LUTs +
# live BSP geometry, then the cart.
#   make rom-vs WAD=/path/to/doom1.wad        then:   make -C neogeo run     (WASD move/turn, A/Space caps)
rom-vs:
	@test -f "$(WAD)" || { echo "ERROR: IWAD not found at '$(WAD)' -- pass WAD=/path/to/doom1.wad (shareware works; not redistributed here)"; exit 1; }
	@if [ ! "$(WAD)" -ef doom1.wad ]; then cp "$(WAD)" doom1.wad; fi   # same-FILE test (-ef): robust when WAD is already the in-tree doom1.wad (any path form), not just the literal string
	python3 tools/wad2c.py doom1.wad E1M1 src/map.c    # bootstrap: map.c is generated, host compiles against it
	$(MAKE) doomng-host
	python3 tools/wad2c.py doom1.wad E1M1 src/map.c    # full asset pass (textures + sprites + title art)
	./doomng-host --bakefloor && python3 tools/floorlut.py   # legacy floor LUT -> floorlut.c1 (kept C-ROM OFFSET ANCHOR)
	./doomng-host --bakeceil  && python3 tools/ceillut.py    # legacy ceil  LUT -> ceillut.c1  (kept C-ROM OFFSET ANCHOR)
	python3 tools/wadsfx.py doom1.wad neogeo/
	$(MAKE) bake-nodes                                 # writes ramps_used.txt (the ramps anchor's manifest); its nodes.bin is NOT read by the live cart
	python3 tools/wad2c.py doom1.wad E1M1 src/map.c --ramps   # ramps AFTER the bake (reads ramps_used.txt)
	WAD=doom1.wad python3 tools/vs_extract.py          # live ALL-E1 BSP geometry (9 maps) -> neogeo/vs_e1.h
	FLVS=1 FLMINB=0.10 FLVIS=80 FLNA=64 FLNAL=128 FLNPHASE=4 FLOUT=/tmp/vsfloor.raw FLPAL=/tmp/vsfloor.pal ./doomng-host --bakefloor   # LIVE floor LUT raw: 64 sets / 180deg + baked depth-fade RAMP (INC2 floor): gentle onset + dark horizon so the VISIBLE far-half shows contrast (the near floor is under the gun/HUD). FLFADE/FLMINB tunable.
	CLVS=1 CLNA=128 CLNAL=256 CLPITCH=1.66 ./doomng-host --bakeceil                                           # LIVE ceiling LUT raw: 128 sets / finer-180 fold + baked depth-fade RAMP (INC2, CLFADE/CLMINB env-tunable). CLPITCH=1.66: higher = TIGHTER/denser pitch. Same tile count -> no chain shift.
	python3 tools/vsfloorlut.py                        # live hex floor LUT -> neogeo/vsfloor.{c1,c2,h} (reads /tmp/vsfloor.raw)
	python3 tools/vsceillut.py                         # live ceiling LUT  -> neogeo/vsceil.{c1,c2,h}  (reads /tmp/vsceil.raw)
	WAD=doom1.wad python3 tools/vsflatlut.py           # per-flat LUT bank -> neogeo/vsflat.{c1,c2,h} (drives ./doomng-host --bakefloor per flat; MUST follow vsfloorlut/vsceillut + wad2c)
	$(MAKE) -C neogeo cart
	$(MAKE) -C neogeo gngeo_data_doomng.zip
	@echo "=== ROM ready. Free-roam:  make -C neogeo run   (WASD move/turn, A/Space caps) ==="
.PHONY: rom-vs

assets: doomng-host
	python3 tools/wad2c.py doom1.wad E1M1 src/map.c            # textures + crom + map.c + sprites (NO --ramps!)
	./doomng-host --bakefloor && python3 tools/floorlut.py     # floor LUT: TILE0 bakes the textile count -> MUST follow any textile-size change
	./doomng-host --bakeceil  && python3 tools/ceillut.py      # ceiling LUT: ditto (sits after floorlut+sprites)
	python3 tools/wad2c.py doom1.wad E1M1 src/map.c --ramps    # ramps (reads the fresh texture+LUT+sprite tile counts)
.PHONY: assets

run: doomng-host
	./doomng-host

# Interactive node-render tuner: live cart-style render + sliders for every knob
# (draw distance, fog, caps, etc). Press 5 in `make run` too. TAB/-/= to tune.
tune: doomng-host
	TUNE=1 ./doomng-host

# Render one headless frame and convert to PNG for inspection.
dump: doomng-host
	./doomng-host --dump frame.bmp && magick frame.bmp frame.png

clean:
	rm -f doomng-host frame.bmp frame.png

.PHONY: run dump clean
