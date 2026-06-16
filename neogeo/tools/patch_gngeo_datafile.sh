#!/usr/bin/env bash
# Build a PRIVATE gngeo datafile whose "puzzledp" cart maps a 1MB P-ROM.
#
# Why: ngdevkit-gngeo's stock gngeo_data.zip defines the puzzledp template cart
# with a 512KB P-ROM region (rom/puzzledp.drv, size field at 0x119 = 0x00080000).
# Our node-render ROM is ~800KB, so the 68k jumps into the unmapped upper half and
# gngeo dies with "Something has gone seriously wrong". Bumping that field to
# 0x00100000 (one byte at 0x11B: 0x08 -> 0x10) maps the full 1MB -- which is exactly
# what real hardware does (ngdevkit.ld already gives ROM1 a 1MB linear window at
# 0x000000-0x0FFFFF). On a real cart / MAME this patch is unnecessary.
set -e
# Locate ngdevkit-gngeo's stock datafile portably: explicit arg 1, else relative to the
# ngdevkit-gngeo binary's install prefix, else Homebrew/Linux defaults. (gngeo run only.)
SYS="${1:-}"
if [ -z "$SYS" ]; then
  G="$(command -v ngdevkit-gngeo 2>/dev/null || true)"
  for c in \
    ${G:+"$(dirname "$G")/../share/ngdevkit-gngeo/gngeo_data.zip"} \
    /opt/homebrew/share/ngdevkit-gngeo/gngeo_data.zip \
    /opt/homebrew/Cellar/ngdevkit-gngeo/*/share/ngdevkit-gngeo/gngeo_data.zip \
    /usr/local/share/ngdevkit-gngeo/gngeo_data.zip \
    /usr/share/ngdevkit-gngeo/gngeo_data.zip ; do
    [ -f "$c" ] && { SYS="$c"; break; }
  done
fi
OUT="${2:-gngeo_data_doomng.zip}"
[ -f "$SYS" ] || { echo "error: gngeo datafile not found (pass it as arg 1): $SYS" >&2; exit 1; }
OUT_ABS="$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")"
cp "$SYS" "$OUT_ABS"
SRCDIR="$(pwd)"                 # capture before cd (to read the built ROM2 size)
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
cd "$WORK"
unzip -oq "$SYS" rom/puzzledp.drv
# P-ROM region 512KB -> 1MB (dword @0x119, hi-mid byte @0x11B=283: 0x08 -> 0x10)
printf '\x10' | dd of=rom/puzzledp.drv bs=1 seek=283 count=1 conv=notrunc 2>/dev/null
# C-ROM: gngeo's C region (Region 9) holds c1+c2 COMBINED. The build pads each file to
# CROMSIZE = 64MB (rom.mk, the full-rail FF bake), so declare 64MB per file / 128MB region.
# (HISTORY: declared sizes MUST be >= the real file sizes -- when the drv said 15MB while the
# build padded to 16MB, gngeo TRUNCATED the load and every tile >= 245,760 rendered garbage.)
#   region total dword @0x0E8 -> 0x08000000 (128MB): byte @0x0EA=234 -> 0x00, @0x0EB=235 -> 0x08
#   C1 size dword: bytes @0x1DF=479 -> 0x00, @0x1E0=480 -> 0x04 (64MB)
#   C2 size dword: bytes @0x210=528 -> 0x00, @0x211=529 -> 0x04 (64MB)
printf '\x00' | dd of=rom/puzzledp.drv bs=1 seek=234 count=1 conv=notrunc 2>/dev/null
printf '\x08' | dd of=rom/puzzledp.drv bs=1 seek=235 count=1 conv=notrunc 2>/dev/null
printf '\x00' | dd of=rom/puzzledp.drv bs=1 seek=479 count=1 conv=notrunc 2>/dev/null
printf '\x04' | dd of=rom/puzzledp.drv bs=1 seek=480 count=1 conv=notrunc 2>/dev/null
printf '\x00' | dd of=rom/puzzledp.drv bs=1 seek=528 count=1 conv=notrunc 2>/dev/null
printf '\x04' | dd of=rom/puzzledp.drv bs=1 seek=529 count=1 conv=notrunc 2>/dev/null
# (P2 / on-rails NODES region REMOVED -- the live-BSP engine never reads it. P-ROM stays P1 1MB,
#  region count stays the stock 6. This drops ~8MB of dead nodes.bin from the cart.)
# --- V-ROM: declare a power-of-2 region >= the real 202-v1 so gngeo's ADPCM address reaches
# EVERY music track AND the SFX. (Was hardcoded 1MB; the per-map soundtrack grew the V-ROM to
# ~5.3MB = 10 ADPCM-B tracks + SFX, so anything past 1MB -- tracks 2-9, the title track, and the
# SFX at 0x500000 -- wrapped to garbage => silent SFX + silent title/late-map music.)
# Region 3 alloc hi-byte @0xD2=210; 202-v1 load hi-byte @0x1AE=430. (<= 8MB fits this one byte.)
VSZ=1048576; [ -f "$SRCDIR/build/rom/202-v1.v1" ] && VSZ=$(stat -f%z "$SRCDIR/build/rom/202-v1.v1")
VP=1048576; while [ "$VP" -lt "$VSZ" ]; do VP=$((VP*2)); done   # next power of 2 >= the V-ROM file
VHB=$(printf '\\x%02x' $(( (VP>>16) & 0xFF )))                  # the size-dword >>16 byte (0x10=1MB .. 0x80=8MB)
printf "$VHB" | dd of=rom/puzzledp.drv bs=1 seek=210 count=1 conv=notrunc 2>/dev/null
printf "$VHB" | dd of=rom/puzzledp.drv bs=1 seek=430 count=1 conv=notrunc 2>/dev/null
echo "V-ROM region: $((VP/1048576))MB (declared for the $((VSZ/1048576))MB 202-v1)"
zip -q "$OUT_ABS" rom/puzzledp.drv
echo "wrote $OUT_ABS (puzzledp P-ROM: P1 1MB, no P2; C-ROM 64MB; V-ROM $((VP/1048576))MB)"
