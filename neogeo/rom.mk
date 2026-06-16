# -*- makefile -*-
# TODO tool to generate those fields 
# TODO move the build name outside of this makefile

BUILDDIR?=build
ROM?=$(BUILDDIR)/rom

# ROM name
GAMEROM=puzzledp

# program ROM (grown to 1MB: ROM1 region is 1MB, directly addressable, no banking)
PROMSIZE=1048576
PROM1=$(ROM)/202-p1.p1
# 8MB expansion: NODES live in ROM2 (banked P-ROM @ 0x200000). Non-banked for now
# (one 1MB bank holds the current 940KB cluster); PROM2SIZE turns on banking for >1MB.
PROM2=$(ROM)/202-p2.p2
# use variable PROM2SIZE to build PROM2 as a banked ROM, i.e
# the concatenation of all the banked ELF files built in your
# project. By default, PROM2 only contains the higher half of
# the 2MB code segment build from your ELF file.
# PROM2SIZE=4194304

# sprite ROM (grown to 15MB/file, 30MB C region via the .drv patch). Holds the
# floor+ceiling LUTs + ramps (now incl. 2-sided upper/lower caps) + textures + fog tiles.
# HW max is 64MB/file (20-bit tiles) so this is well within spec.
CROMSIZE=67108864   # 64MB/pair = 2^20 tiles, the HW max (20-bit tile #) -- needed for the FULL-RAIL FULL-FRAME bake (~350k FF tiles + 145k base chain blows past 16MB=2^18 and even 32MB=2^19). MUST be a power-of-two tile count: MAME (and real boards) mask sprite tile numbers by pow2 -- at 15MB the ramp-cap tiles past 131071 wrapped into the texture banks and rendered as solid squares (the MAME "bottom caps" bug). gngeo never masked, which hid it.
CROM1=$(ROM)/202-c1.c1
# use variable CROMxSIZE to customize the size of a particular ROM
# by default, CROMSIZE value is used for every CROM
# CROM2SIZE=1048576
CROM2=$(ROM)/202-c2.c2

# fixed tile ROM
SROMSIZE=131072
SROM1=$(ROM)/202-s1.s1

# sound driver ROM
MROMSIZE=131072
MROM1=$(ROM)/202-m1.m1

# sound FX ROM
VROMSIZE=524288
VROM1=$(ROM)/202-v1.v1
# use variable VROMTEMPLATE below to build your music and SFX
# assets with vromtool instead of defining dependencies manually
# in makefile. Se build.mk for more details
# VROMTEMPLATE=$(ROM)/202-vX.vX
