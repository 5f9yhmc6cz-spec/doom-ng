#!/bin/sh
# Regenerate the per-map ADPCM-B soundtrack for DOOM-NG: E1M1..E1M9 + the title/intro track.
# Source = the hand-made MIDIs in neogeo/../midi/ (M_E1M1.mid..M_E1M9.mid, M_INTRO.mid). The earlier
# MUS->MIDI auto-conversion sounded wrong; these are the good MIDIs.
# Pipeline: MIDI -> dry WAV (fluidsynth, no reverb/chorus, gain .5) -> mono 18.5kHz (sox)
#   -> ADPCM-B (adpcmtool) -> truncate to the 512KB-per-track V-ROM region (~56s loop).
# 18.5kHz encode rate is LOCKED to doomsnd.s ADPCM-B delta-N 0x5540 -- do not change it.
# .adpcmb outputs are gitignored (music is copyright); this + the midi/ dir are the source.
# Run from the repo root: sh tools/genmusic.sh
set -e
# Soundfont: explicit SF= wins; else auto-detect a General-MIDI .sf2 from common
# locations (Linux distro paths first, then macOS/Homebrew). Override with SF=/path.
if [ -z "${SF:-}" ]; then
  for _c in \
    /usr/share/sounds/sf2/FluidR3_GM.sf2 \
    /usr/share/sounds/sf2/default-GM.sf2 \
    /usr/share/soundfonts/default.sf2 \
    /usr/share/soundfonts/FluidR3_GM.sf2 \
    /opt/homebrew/share/fluid-synth/sf2/VintageDreamsWaves-v2.sf2 \
    /opt/homebrew/Cellar/fluid-synth/*/share/fluid-synth/sf2/*.sf2 \
    /usr/local/share/fluid-synth/sf2/*.sf2 ; do
    [ -f "$_c" ] && { SF="$_c"; break; }
  done
fi
HERE="$(cd "$(dirname "$0")" && pwd)"
MIDIDIR="${MIDIDIR:-$HERE/../midi}"
{ [ -n "${SF:-}" ] && [ -f "$SF" ]; } || { echo "no soundfont found -- install one (e.g. 'apt install fluid-soundfont-gm', or 'brew install fluid-synth') or set SF=/path/to.sf2" >&2; exit 1; }
echo "soundfont: $SF"
[ -d "$MIDIDIR" ] || { echo "need MIDI dir $MIDIDIR (set MIDIDIR=...)"; exit 1; }
render() {   # $1 = midi path, $2 = output basename (e1m1..e1m9, e1mi)
  [ -f "$1" ] || { echo "MISSING $1 -- skipping (silent)"; python3 -c "open('$HERE/../neogeo/$2.adpcmb','wb').write(bytes(524288))"; return; }
  fluidsynth -ni -R 0 -C 0 -g 0.5 -F "/tmp/$2.wav" -r 22050 "$SF" "$1" >/dev/null 2>&1
  sox "/tmp/$2.wav" -c 1 -r 18500 "/tmp/${2}_mono.wav" 2>/dev/null
  adpcmtool.py -b -e -r 18500 -o "/tmp/$2.adpcmb" "/tmp/${2}_mono.wav" >/dev/null 2>&1
  # each track occupies exactly one 512KB V-ROM region: truncate long tracks, REPEAT-fill short
  # ones (e.g. the ~13s intro) so the hardware 512KB loop stays musical instead of silent.
  python3 -c "import sys; d=open('/tmp/$2.adpcmb','rb').read(); d=(d*(524288//max(1,len(d))+1))[:524288] if len(d)<524288 else d[:524288]; open('$HERE/../neogeo/$2.adpcmb','wb').write(d)"
  printf '%-12s %8d B  (full %8d B)\n' "$2.adpcmb" "$(wc -c < "$HERE/../neogeo/$2.adpcmb")" "$(wc -c < /tmp/$2.adpcmb)"
}
for n in 1 2 3 4 5 6 7 8 9; do render "$MIDIDIR/M_E1M$n.mid" "e1m$n"; done
render "$MIDIDIR/M_INTRO.mid" "e1mi"   # title/intro track -> V-ROM slot 9 (cmd 0x19)
echo "done -- 10 tracks (9 maps + intro) at 512KB each = $((10*524288)) B of ADPCM-B"
