# Copyright (c) 2015-2025 Damien Ciabrini
# This file is part of ngdevkit
#
# ngdevkit is free software: you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as
# published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.
#
# ngdevkit is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with ngdevkit.  If not, see <http://www.gnu.org/licenses/>.


all: cart bios

# These are various variables that you might want to customize
# based on your liking or your requirements.
# location of generated and compiled content
BUILDDIR=build
# all directories that contain source to be compiled
SRCDIRS=assets
# default build flags, can be overriden per target
# -ffunction-sections/-fdata-sections + --gc-sections: the node-render runtime never
# calls the BSP renderer (render_node/to_view) or its ~234KB ROTD/ROTS rotation LUTs,
# so let the linker garbage-collect that dead weight and reclaim P-ROM for more nodes.
CFLAGS=-I$(BUILDDIR) -I../src -DUSE_FIXED=1 -std=c99 -fomit-frame-pointer -ffunction-sections -fdata-sections -O2 -g
LDFLAGS=-Wl,--gc-sections -Wl,--entry=_start -Wl,-u,main -Wl,-u,rom_game_default -Wl,-u,rom_eye_catcher_default
Z80FLAGS=
Z80LDFLAGS=

# This is an autoconf-generated configuration for your environment
# (ngdevkit path, OS-specific configs...)
include config.mk

# This defines the layout of your game cartridge
# You can customize it to match your requirements
include rom.mk

# All the generic build targets (68k, Z80, assets, run)
include build.mk

# Some default targets for running your project via emulators
include emu.mk

# DOOM ON RAILS: prefer a locally-built gngeo patched to title the window "DOOM ON RAILS"
# (neogeo/tools/gngeo-dor, gitignored); fall back to the system ngdevkit-gngeo if absent.
ifneq ($(wildcard $(CURDIR)/tools/gngeo-dor),)
GNGEO := $(CURDIR)/tools/gngeo-dor
endif

# --- DOOM-NG: the LIVE BSP renderer is the only engine -- VSLICE is always on. ---
CFLAGS += -DVSLICE=1
CFLAGS += $(VSEXTRA)          # dev hook: extra -D defines (e.g. VS_DIAG teleport/cull tallies)
# bake-nodes now runs ONLY --bakec (the ramps_used.txt manifest the ramps anchor needs); --bakeS/nodes.bin
# and the dead 8MB P2 pack are gone (the live engine never read NODES).
FFSTAMP = $(BUILDDIR)/.mode-vs
$(FFSTAMP):
	@mkdir -p $(BUILDDIR) && rm -f $(BUILDDIR)/.mode-* && touch $@

# --- DOOM-NG: one-command run ---
# Our node-render ROM is ~800KB, but stock gngeo maps puzzledp's P-ROM as 512KB.
# Build a private datafile that maps the full 1MB (matches real hardware) and run.
# DEPENDS ON THE V-ROM size: the datafile bakes the V-ROM region size at patch time, so it MUST
# regenerate when the soundtrack grows 202-v1 (a stale declaration masks the ADPCM address ->
# silent SFX + silent late music). (P2/on-rails NODES removed -- pure ROM1 live-BSP now.)
gngeo_data_doomng.zip: tools/patch_gngeo_datafile.sh build/rom/202-v1.v1 build/rom/202-p2.p2 build/rom/202-p1.p1 build/rom/202-c1.c1 build/rom/202-c2.c2
	tools/patch_gngeo_datafile.sh
# W=fwd A=left S=back D=right; Space=fire(A) P=debug(B); N=C M=D drive the ablation rig.
# (C/D were bound to the a/d GLANCE KEYS -- every glance cycled/flipped the rig at random, the "complete mess".)
# (K119=w K97=a K115=s K100=d; the game reads UP||A, DOWN||B, LEFT||C, RIGHT||D).
WASD=A=K32,B=K112,C=K110,D=K109,UP=K119,DOWN=K115,LEFT=K97,RIGHT=K100,START=K38,COIN=K34,MENU=K27
# Sound off by default (quiet iteration). gngeo has no runtime mute key, so it's a build-time
# toggle: `gmake run` = muted, `gmake run SND=--sound` or `gmake run-sound` = audio.
SND ?= --sound
run: gngeo_data_doomng.zip
	$(MAKE) gngeo EXTRAOPTS='-d $(CURDIR)/gngeo_data_doomng.zip --screen320 --no-pal --no-vsync --p1control $(WASD) $(SND) -b soft -e none'
run-sound: ; $(MAKE) run SND=--sound
# one-command live free-roam run: W/S move, A/D turn, A/Space caps toggle, perf readout.
# (MUST build `cart` too -- `run` alone only repacks the datafile, NOT the program ROM.)
run-vs: ; $(MAKE) cart && $(MAKE) run
.PHONY: run run-sound run-vs

# Convenience: the native node-render tuner lives in the root Makefile (it's the host app, not the cart).
tune:
	$(MAKE) -C .. tune
.PHONY: tune

# program ROM: your main program
# Add your dependencies below to compile your sources into an ELF binary
# that is used as the content of the program ROM (referenced by symbol PROM1)
# 
# Note: build rules (%.c -> %.o -> %.elf) are defined in Makefile.build

ELF=$(BUILDDIR)/rom.elf
$(ELF):	$(BUILDDIR)/main.o   # dead on-rails engine delinked (render.o/map.o/nodes_idx.o) -> frees ~40KB work-RAM .bss + ~434KB P1 text; the live VSLICE path needs none of it
$(PROM1): $(ELF)
# BANKED GEOMETRY (godzilla / all-Ultimate): 202-p2 = the 3-bank live-BSP geometry blob
# (tools/vs_extract.py -> neogeo/vs_geo.bin), byte-swabbed exactly like P1. The engine reads it through
# the 0x200000 banked window via P_ROM_SWITCH_BANK(VE_BANK[m]) in vs_set_map(). (Re-adds a P2 chip;
# task #44 retired the dead on-rails P2 -- this one is LIVE geometry, not the old pre-baked NODES.)
$(PROM2): vs_geo.bin | $(ROM)
	cp $< $@ && dd if=$@ of=$@ conv=notrunc,swab status=none
$(CART): $(PROM2)

# our engine core (shared with the host), compiled fixed-point for the 68000
$(BUILDDIR)/render.o: ../src/render.c ../src/dng.h ../src/fixed.h
	$(M68KGCC) $(NGCFLAGS) $(CFLAGS) -c $< -o $@
$(BUILDDIR)/map.o: ../src/map.c ../src/dng.h
	$(M68KGCC) $(NGCFLAGS) $(CFLAGS) -c $< -o $@
# the node INDEX -> a normal object in ROM1 (always mapped, whichever P2 bank is selected);
# the runtime reads it to find each node's bank+offset. NODES itself is raw data in PROM2.
$(BUILDDIR)/nodes_idx.o: nodes_idx.c nodes_data.h
	$(M68KGCC) $(NGCFLAGS) $(CFLAGS) -c $< -o $@
# main.o embeds the generated headers -- rebuild it when a bake regenerates them
# (FFSTAMP: also when switching between the normal cart and the FFRIDE assessment build)
$(BUILDDIR)/main.o: main.c textiles.h floorlut.h sprites.h ramps.h ceillut.h hudfix.h gunhand.h fbsolid.h fogcm.h vs_e1.h vsfloor.h vsceil.h vsflat.h interpic.h $(FFSTAMP)



# fixed tiles ROM: all your 8x8 pixel tiles for the fixed layer
# Add your dependencies below to convert images to fixed tile binary data
# and pack it into the fixed tile ROM (referenced by symbol SROM1)
# 
# By default, this makefile creates a tileset ROM with small and tall
# latin characters for printing ASCII string, and tiles that are
# displayed during the attract mode.
# Note: build rules (%.gif -> %.fix) are defined in Makefile.build
# ngdevkit-examples base assets (logo tiles 0..256 + the fix-layer font), shipped prebuilt under
# assets/prebuilt (ngdevkit's own freely-licensed art -- NOT id assets). Tile numbering depends on
# them: DOOM textures start at tile 257.
$(BUILDDIR)/assets/%: assets/prebuilt/%
	@mkdir -p $(dir $@); cp $< $@

$(SROM1): $(BUILDDIR)/assets/base-srom-text-shadow.fix hudfix.fix gunhand.fix fbsolid.fix
# hudfix.fix: the status bar + face expressions as fix tiles (generated by wad2c.py main pass);
# concatenated AFTER the font, so HUD tiles start at index 1280 (= 40960/32) = HUDFIX_BASE.
# gunhand.fix: the 6 first-person weapons as fix tiles (tools/gunbake.py), cat'd AFTER hudfix -> GUNHAND_BASE=1466.
gunhand.fix gunhand.h: hudfix.fix
	cd $(CURDIR)/.. && python3 tools/gunbake.py doom1.wad
# fbsolid.fix: FB-spike 15 solid colour tiles, cat'd LAST (chain end, safe region) -> FB_SOLID_TILE_BASE=3616.
fbsolid.fix fbsolid.h: gunhand.h
	cd $(CURDIR)/.. && python3 tools/fb_solidtiles.py
# fogcm.h: COLORMAP floor/ceiling fade bands (DOOM light-diminish curve), parsed from the LUT palettes + WAD.
fogcm.h: floorlut.h ceillut.h
	cd $(CURDIR)/.. && python3 tools/fb_fogcm.py



# sprite ROM: all your 16x16 pixel tiles for sprites
# Add your dependencies below to convert images to fixed sprite binary data
# and pack it into two separate sprite ROMs that hold odd and even bits of
# color information (referenced by symbols CROM1 and CROM2)
#
# By default, this makefile creates CROMs with tiles for displaying a ngdevkit
# logo during the attract mode.
# Note: build rules (%.gif -> %.c<1,2>) are defined in Makefile.build
$(CROM1): $(BUILDDIR)/assets/base-crom-logo.c1
$(CROM2): $(BUILDDIR)/assets/base-crom-logo.c2
$(CROM1): $(BUILDDIR)/assets/logo.c1
$(CROM2): $(BUILDDIR)/assets/logo.c2
# real Doom texture tiles (generated by tools/wad2c.py), placed at tile 257+
$(CROM1): textiles.c1
$(CROM2): textiles.c2
# periodic floor-cast LUT (generated by tools/floorlut.py), placed right after textiles
$(CROM1): floorlut.c1
$(CROM2): floorlut.c2
# demo sprites: first-person pistol, imp, status bar (generated by tools/wad2c.py), after the LUT
$(CROM1): sprites.c1
$(CROM2): sprites.c2
# authentic ceiling LUT (generated by tools/ceillut.py)
$(CROM1): ceillut.c1
$(CROM2): ceillut.c2
# smooth-wall diagonal-alpha edge tiles (generated by tools/wad2c.py --ramps) -> RAMP_TILE0 = end of the STATIC chain
$(CROM1): ramps.c1
$(CROM2): ramps.c2
# LIVE-ENGINE hex floor LUT (tools/vsfloorlut.py) -- after ramps; VSFLOOR_TILE0 in vsfloor.h
$(CROM1): vsfloor.c1
$(CROM2): vsfloor.c2
# LIVE-ENGINE grey ceiling LUT (tools/vsceillut.py, 90deg fold) -- after vsfloor; VSCEIL_TILE0 in vsceil.h
$(CROM1): vsceil.c1
$(CROM2): vsceil.c2
# PER-FLAT perspective LUT bank (tools/vsflatlut.py) -- NEW chain end, after vsceil; VSFLAT_TILE0 in vsflat.h
$(CROM1): vsflat.c1
$(CROM2): vsflat.c2
# (the FF full-frame engine + its fftiles C-ROM block were removed -- live BSP renderer only.
#  vsfloor/vsceil are the chain end now, so their TILE0 is fixed and FF-bake-independent.)
# vsfloor/vsceil TILE0 are computed from the chain length BEFORE them (logo..ramps for vsfloor, +vsfloor for
# vsceil) -- now INDEPENDENT of fftiles (which trails them), so an FF re-bake no longer invalidates them.
# Regenerate when the last upstream component (ramps) changes. (tools assume CWD=repo root.)
vsfloor.c1 vsfloor.c2 vsfloor.h: ramps.c1
	cd $(CURDIR)/.. && python3 tools/vsfloorlut.py
vsceil.c1 vsceil.c2 vsceil.h: vsfloor.c1
	cd $(CURDIR)/.. && python3 tools/vsceillut.py
vsflat.c1 vsflat.c2 vsflat.h: vsceil.c1
	cd $(CURDIR)/.. && WAD=$(CURDIR)/../doom1.wad python3 tools/vsflatlut.py



# sound driver ROM: nullsound + your configured sound commands
# Add your dependencies below to compile your z80 sources into a HEX binary
# that is used as the sound driver ROM (referenced by symbol MROM1)
#
# By default, this makefile uses the empty sound driver provided by ngdevkit
# Note: build rules (%.s -> %.o -> %.ihx) are defined in Makefile.build
# DOOM-NG: custom standalone Z80 driver that loops the ADPCM-B music on boot.
# Link WITHOUT nullsound.lib (we provide our own boot vectors at 0x0000).
SOUND_DRIVER=$(BUILDDIR)/doomsnd.ihx
$(BUILDDIR)/doomsnd.ihx: $(BUILDDIR)/doomsnd.rel
	$(Z80SDLD) -i $@ $<
$(MROM1): $(SOUND_DRIVER)



# sound FX ROM: all your ADPCM samples
# Add your dependencies below to convert your samples into suitable
# ADPCM data and pack it into the sound ROM (referenced by symbol VROM1)
#
# DOOM-NG: At Doom's Gate, encoded as one ADPCM-B sample (first 512KB), looped
# in hardware by our Z80 driver.
# V-ROM = music (ADPCM-B @0x00000) + pistol (ADPCM-A @0x80000) + imp death (@0x80B00).
# Both SFX are 256-byte aligned (2816 & 3584 = 11 & 14 blocks) so a plain cat keeps the
# ADPCM-A start/stop addresses in doomsnd's table valid.
# SFX conversion (wavs come from tools/wadsfx.py; 256-byte ADPCM-A block alignment preserved by cat)
%.adpcma: %.wav
	$(ADPCMTOOL) -a -e $< -o $@

# V-ROM = 10 ADPCM-B tracks (E1M1..E1M9 then the intro/title track e1mi, 512KB each = 5MB) then the
# ADPCM-A SFX. Track k lives at byte k*512KB (k=9 = intro); the SFX follow at 0xE00000 (28 tracks; doomsnd _sfxtab matches).
# Music slots: 0-8 E1M1-9, 9 intro, 10-18 E2M1-9, 19-27 E3M1-9 (E4 reuses E1-E3, see main.c MUSIC[]).
$(VROM1): e1m1.adpcmb e1m2.adpcmb e1m3.adpcmb e1m4.adpcmb e1m5.adpcmb e1m6.adpcmb e1m7.adpcmb e1m8.adpcmb e1m9.adpcmb e1mi.adpcmb e2m1.adpcmb e2m2.adpcmb e2m3.adpcmb e2m4.adpcmb e2m5.adpcmb e2m6.adpcmb e2m7.adpcmb e2m8.adpcmb e2m9.adpcmb e3m1.adpcmb e3m2.adpcmb e3m3.adpcmb e3m4.adpcmb e3m5.adpcmb e3m6.adpcmb e3m7.adpcmb e3m8.adpcmb e3m9.adpcmb snd_pistol.adpcma snd_impdeath.adpcma snd_boom.adpcma snd_itemup.adpcma snd_dooropn.adpcma snd_impdth.adpcma snd_posdth.adpcma snd_dorcls.adpcma snd_pstart.adpcma snd_pstop.adpcma
	cat $^ > $@ && ln -sf $(notdir $@) $(ROM)/202-v1.bin
# Music provenance: the e1m*.adpcmb are NOT shipped (id/Bobby Prince copyright). Regenerate all 9
# from the WAD with `sh tools/genmusic.sh` (doom1.wad + a soundfont -> mus2mid -> fluidsynth ->
# adpcmtool @18.5kHz). Per-track e1mN.wav also works. With neither, the build falls back to a
# silent 512KB loop per track so the cart still builds + plays.
e1m%.adpcmb:
	@w=$(@:.adpcmb=.wav); if [ -f $$w ]; then echo "converting $$w -> $@"; adpcmtool.py -b -e $$w -o $@ || /opt/homebrew/bin/adpcmtool.py -b -e $$w -o $@; else echo "NOTE: no $@ -- run 'sh tools/genmusic.sh' for real music; building SILENT"; python3 -c "open('$@','wb').write(bytes(524288))"; fi
# E2/E3 tracks come from the Ultimate MIDI Pack: `sh tools/genmusic_ultimidi.sh` (ultimidi.zip + soundfont).
# TWO SEPARATE pattern rules on purpose: a single `e2m%.adpcmb e3m%.adpcmb:` is a MULTI-TARGET pattern rule,
# so GNU make believes one recipe run produces BOTH stems -> it marks e3m<stem> built without ever writing it
# -> the V-ROM cat fails on a clean checkout. Each stem now gets its own recipe run. (Mirror the e1m% .wav->
# adpcm path so a real Ultimate-MIDI-Pack bake is picked up too; otherwise a silent 512KB placeholder.)
e2m%.adpcmb:
	@w=$(@:.adpcmb=.wav); if [ -f $$w ]; then echo "converting $$w -> $@"; adpcmtool.py -b -e $$w -o $@ || /opt/homebrew/bin/adpcmtool.py -b -e $$w -o $@; else echo "NOTE: no $@ -- run 'sh tools/genmusic_ultimidi.sh' for real E2/E3 music; building SILENT"; python3 -c "open('$@','wb').write(bytes(524288))"; fi
e3m%.adpcmb:
	@w=$(@:.adpcmb=.wav); if [ -f $$w ]; then echo "converting $$w -> $@"; adpcmtool.py -b -e $$w -o $@ || /opt/homebrew/bin/adpcmtool.py -b -e $$w -o $@; else echo "NOTE: no $@ -- run 'sh tools/genmusic_ultimidi.sh' for real E2/E3 music; building SILENT"; python3 -c "open('$@','wb').write(bytes(524288))"; fi
# Note: build rules (%.wav-> %.adpcm<a,b>) are defined in Makefile.build



# One-time asset preprocessing
# Various assets may have to be processed before they can be used to
# build your ROM, for example:
#   . the ngdevkit assets must be converted to sprite and text tiles
#   . converting your SFX assets from .mp3 into .wav so they can be
#     processed by ngdevkit tools
# To handle these pre-processing requirements, `make` automatically
# runs into all subdirectories under ./setup/, so you get a chance
# to preprocess what you need before anything is built.
#
# Other assets must be processed only once to generate source files
# that gets built into your ROM, for example:
#   . converting your musics from .fur to z80 code and data files
#   . generating make dependencies to pack gfx or sound assets into
#     the right ROM bank based on your input assets
# Those actions can be achieved by creating custom make targets
# that get invoked automatically when added to the variable below.
CUSTOM_GENERATE_TARGETS=assets/logo.gif
assets/logo.gif:
	magick \( -size 1x1 xc:black \) \( -size 1x1 xc:white \) +append /tmp/dngpal.gif
	magick -size 16x16 xc:white -remap /tmp/dngpal.gif GIF:$@


# a default `clean` target removes .o .elf and .ihx from the builddir
# a default `distclean` target removes the build directory entirely
# you can customize clean up by adding dependencies to those targets
distclean: distclean-logo
distclean-logo:
	rm -f assets/logo.gif
.PHONY: distclean-logo

