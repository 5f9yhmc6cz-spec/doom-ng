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
# --- BANKED GEOMETRY P2 (godzilla / all-Ultimate): declare a 7th region so gngeo maps 202-p2 at
# 0x200000. 202-p2 = the live-BSP geometry blob (3 banks now; auto-sized from the built file).
P2MB=1; [ -f "$SRCDIR/build/rom/202-p2.p2" ] && P2MB=$(( ($(stat -c%s "$SRCDIR/build/rom/202-p2.p2" 2>/dev/null || stat -f%z "$SRCDIR/build/rom/202-p2.p2")+1048575)/1048576 )); [ "$P2MB" -lt 1 ] && P2MB=1
# Region-count dword @236(0xEC): 6 -> 7.
printf '\x07' | dd of=rom/puzzledp.drv bs=1 seek=236 count=1 conv=notrunc 2>/dev/null
# Grow the P-ROM region so P1@0 AND P2@0x200000 fit one allocation: size = (1+P2MB) MB.
# hi-mid byte @230(0xE6) = ((1+P2MB)*1MB)>>16 = (1+P2MB)<<4. (gngeo's bankswitch maps file offset
# (n+1)*1MB into the 0x200000 window, so bank n=0..P2MB-1 covers all of P2.)
printf "$(printf '\\x%02x' $(( (1+P2MB)<<4 )))" | dd of=rom/puzzledp.drv bs=1 seek=230 count=1 conv=notrunc 2>/dev/null
# --- V-ROM: declare a power-of-2 region >= the real 202-v1 so gngeo's ADPCM address reaches
# EVERY music track AND the SFX. (Was hardcoded 1MB; the per-map soundtrack grew the V-ROM to
# ~5.3MB = 10 ADPCM-B tracks + SFX, so anything past 1MB -- tracks 2-9, the title track, and the
# SFX at 0x500000 -- wrapped to garbage => silent SFX + silent title/late-map music.)
# Region 3 alloc size-dword @0xD0=208 (LE); 202-v1 load size-dword @0x1AC=428 (LE). The >>16 byte is at
# 210/430 and the >>24 byte at 211/431 -- BOTH must be written: 16MB=0x01000000 carries its 0x01 in the
# >>24 byte, so writing only >>16 (=0x00) collapsed the whole dword to 0 => gngeo silenced ALL ADPCM.
VSZ=1048576; [ -f "$SRCDIR/build/rom/202-v1.v1" ] && VSZ=$(stat -c%s "$SRCDIR/build/rom/202-v1.v1" 2>/dev/null || stat -f%z "$SRCDIR/build/rom/202-v1.v1")  # GNU stat (Linux) first, BSD/macOS fallback
VP=1048576; while [ "$VP" -lt "$VSZ" ]; do VP=$((VP*2)); done   # next power of 2 >= the V-ROM file
VHB=$(printf '\\x%02x' $(( (VP>>16) & 0xFF )))                  # size-dword byte 2 (>>16)
VHB2=$(printf '\\x%02x' $(( (VP>>24) & 0xFF )))                 # size-dword byte 3 (>>24) -- 0x01 at 16MB
printf "$VHB"  | dd of=rom/puzzledp.drv bs=1 seek=210 count=1 conv=notrunc 2>/dev/null
printf "$VHB2" | dd of=rom/puzzledp.drv bs=1 seek=211 count=1 conv=notrunc 2>/dev/null
printf "$VHB"  | dd of=rom/puzzledp.drv bs=1 seek=430 count=1 conv=notrunc 2>/dev/null
printf "$VHB2" | dd of=rom/puzzledp.drv bs=1 seek=431 count=1 conv=notrunc 2>/dev/null
echo "V-ROM region: $((VP/1048576))MB (declared for the $((VSZ/1048576))MB 202-v1)"
# Append a 49-byte P-ROM region entry for 202-p2.bin: type=0x08 @meta+16; dest(load)=0x10<<16=0x100000
# @meta+23 (bank 0 -- gngeo maps file offset (n+1)*1MB into the 0x200000 window); size=(P2MB<<4)
# @meta+27 ((P2MB*1MB)>>16); CRC=0 @meta+29 (gngeo doesn't verify CRC in -i raw mode).
python3 -c "import sys; sys.stdout.buffer.write(b'202-p2.bin'+b'\x00'*6 + b'\x00'*16 + b'\x08' + b'\x00'*6 + b'\x10' + b'\x00'*3 + bytes([$P2MB<<4]) + b'\x00' + b'\x00'*4)" >> rom/puzzledp.drv
zip -q "$OUT_ABS" rom/puzzledp.drv
echo "wrote $OUT_ABS (puzzledp P-ROM: P1 1MB + P2 ${P2MB}MB banked; C-ROM 64MB; V-ROM $((VP/1048576))MB)"
