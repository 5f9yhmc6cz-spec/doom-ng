#!/bin/sh
# Bake the E2/E3 per-map ADPCM-B soundtrack for DOOM-NG from the Ultimate MIDI Pack (ultimidi.zip).
# E1 stays as-is (neogeo/e1m*.adpcmb from tools/genmusic.sh); this ADDS e2m1..e2m9 + e3m1..e3m9.
# E4 maps REUSE E1-E3 tracks at runtime (real-DOOM assignment, see main.c MUSIC[]), so no e4 bake.
#
# Source = the Ultimate MIDI Pack (Ivan Stanton / Doomworld community, 2021; CC-style free release).
# Same pipeline + LOCKED 18.5kHz encode rate as genmusic.sh (doomsnd.s ADPCM-B delta-N 0x5540):
#   MIDI -> dry WAV (fluidsynth) -> mono 18.5kHz (sox) -> ADPCM-B (adpcmtool) -> 512KB V-ROM region.
# .adpcmb outputs are gitignored (music is third-party); ultimidi.zip is the source. Run from repo root:
#   sh tools/genmusic_ultimidi.sh
set -e
SF="${SF:-/opt/homebrew/Cellar/fluid-synth/2.5.4/share/fluid-synth/sf2/VintageDreamsWaves-v2.sf2}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ZIP="${ULTIMIDI:-$HERE/../ultimidi.zip}"
[ -f "$SF" ]  || { echo "need soundfont $SF (set SF=...)"; exit 1; }
[ -f "$ZIP" ] || { echo "need $ZIP (set ULTIMIDI=...)"; exit 1; }
MID="$(mktemp -d)"; trap 'rm -rf "$MID"' EXIT
# ultimidi.zip nests the real midi pack as an inner ultimidi.zip; extract its midis/ dir
unzip -oq "$ZIP" ultimidi.zip -d "$MID"
unzip -oq "$MID/ultimidi.zip" 'midis/*' -d "$MID"
render() {   # $1 = midi path, $2 = output basename (e2m1.. e3m9)
  [ -f "$1" ] || { echo "MISSING $1 -- skipping (silent)"; python3 -c "open('$HERE/../neogeo/$2.adpcmb','wb').write(bytes(524288))"; return; }
  fluidsynth -ni -R 0 -C 0 -g 0.5 -F "/tmp/$2.wav" -r 22050 "$SF" "$1" >/dev/null 2>&1
  sox "/tmp/$2.wav" -c 1 -r 18500 "/tmp/${2}_mono.wav" 2>/dev/null
  adpcmtool.py -b -e -r 18500 -o "/tmp/$2.adpcmb" "/tmp/${2}_mono.wav" >/dev/null 2>&1
  # each track = exactly one 512KB V-ROM region: truncate long tracks, REPEAT-fill short ones so the loop stays musical
  python3 -c "import sys; d=open('/tmp/$2.adpcmb','rb').read(); d=(d*(524288//max(1,len(d))+1))[:524288] if len(d)<524288 else d[:524288]; open('$HERE/../neogeo/$2.adpcmb','wb').write(d)"
  printf '%-12s %8d B  <- %s\n' "$2.adpcmb" "$(wc -c < "$HERE/../neogeo/$2.adpcmb")" "$(basename "$1")"
}
for ep in 2 3; do
  for n in 1 2 3 4 5 6 7 8 9; do
    f=$(ls "$MID"/midis/E${ep}M${n}\ *.mid 2>/dev/null | head -1)
    render "$f" "e${ep}m${n}"
  done
done
echo "done -- 18 tracks (E2M1-9 + E3M1-9) at 512KB each = $((18*524288)) B of ADPCM-B"
