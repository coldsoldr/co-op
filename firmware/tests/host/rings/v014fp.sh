#!/bin/bash
# rings 0.14: code per render and per block, and compute per render, in the GNU simulator (the Makefile's
# `v014-footprint`), the build as it ships (plugin.json's flags) from the plugin folder SRC.
#   v014fp.sh BUILD BFIN SRC TAG
# Scenarios (rings_compare.cc): "chord" = the device's six-key chord (docs/device-tests.md, 2026-11-22), held, the
# damping at the top, knobs still (-DRINGS_CHORD), traced blocks 260..299; "play" = the random scenario at one model,
# knobs still, no jumps or bends, blocks 120..159; "walk" = the same with the knob walk and jumps. Polyphony 4.
# Prints code_footprint.py's report and the compute (CYCLES around each render, the plugin without stage timers).
set -u
B=$1; BF=$2; SRC=$3; TAG=$4
FLAGS="-DOP1_COOP -DOP1RINGS_CONTROL_HALVED=1 -DOP1RINGS_CONTROL_BATCHED=1 -DOP1RINGS_COOP_POLY4=1 -DOP1RINGS_COOP_POLY_ALL=1 -DOP1RINGS_VOICE_44K=1"
SIM="-O2 -std=c++17 -mcpu=bf524 -msim -fno-exceptions -fno-rtti -fno-tree-loop-distribute-patterns -Wno-class-memaccess -DOP1RINGS_SAMPLE_RATE=22050 -DOP1RINGS_CONTROL_BLOCK_SIZE=32 -DOP1RINGS_UPSAMPLE=4"
DSP="src/op1_voice.cc src/string_voice.cc src/string_fixed.cc src/reverb_fixed.cc src/fm_voice_fixed.cc src/modal_resonator.cc src/fixed_tables.cc"
ASM=$(ls "$SRC"/src/*.S)
PLUGIN="$SRC/rings.cc $(for f in $DSP; do echo "$SRC/$f"; done) $ASM"
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
mkdir -p "$B"
build() {   # out, scenario flags
  "$BF/bfin-elf-g++" $SIM $FLAGS $2 -DRINGS_TRACE -I"$SRC" -I../../../sdk rings_compare.cc $PLUGIN -o "$1"
}
cyc() {     # out, scenario flags: the driver timed (-DRINGS_PROFILE -DRINGS_CYCLES), the plugin as it ships
  "$BF/bfin-elf-g++" $SIM $FLAGS $2 -DRINGS_PROFILE -DRINGS_CYCLES -I"$SRC" -I../../../sdk -c rings_compare.cc -o "$1.o"
  "$BF/bfin-elf-g++" $SIM $FLAGS $2 -I"$SRC" -I../../../sdk "$1.o" $PLUGIN -o "$1"
  rm -f "$1.o"
}
build "$B/fp_chord_$TAG.elf" "-DRINGS_CHORD" &
build "$B/fp_play_$TAG.elf" "-DRINGS_STILL -DRINGS_NO_JUMP -DRINGS_NO_BEND" &
build "$B/fp_walk_$TAG.elf" "" &
cyc "$B/cy_chord_$TAG.elf" "-DRINGS_CHORD" &
cyc "$B/cy_play_$TAG.elf" "-DRINGS_STILL -DRINGS_NO_JUMP -DRINGS_NO_BEND" &
cyc "$B/cy_walk_$TAG.elf" "" &
wait
for m in 5 2 1 4 0 3; do
  ( "$BF/bfin-elf-run" --trace-insn=on "$B/fp_chord_$TAG.elf" /dev/null 300 $m 2 2>&1 |
      python3 -I code_footprint.py "$B/fp_chord_$TAG.elf" "$BF/bfin-elf-nm" 260 300 > "$W/chord$m" ) &
  ( "$BF/bfin-elf-run" --trace-insn=on "$B/fp_play_$TAG.elf" /dev/null 160 $m 2 2>&1 |
      python3 -I code_footprint.py "$B/fp_play_$TAG.elf" "$BF/bfin-elf-nm" 120 160 > "$W/play$m" ) &
  ( "$BF/bfin-elf-run" --trace-insn=on "$B/fp_walk_$TAG.elf" /dev/null 160 $m 2 2>&1 |
      python3 -I code_footprint.py "$B/fp_walk_$TAG.elf" "$BF/bfin-elf-nm" 120 160 > "$W/walk$m" ) &
  for s in chord play walk; do
    ( "$BF/bfin-elf-run" "$B/cy_${s}_$TAG.elf" /dev/null 3000 $m 2 2>/dev/null |
        grep -E "voiced|^[a-z]+ +[a-z0-9]+ +[0-9]" > "$W/cy$s$m" ) &
  done
done
wait
for m in 5 2 1 4 0 3; do
  echo "== $TAG chord, model $m, polyphony 4"
  head -30 "$W/chord$m"
  for s in play walk; do
    echo "== $TAG $s, model $m, polyphony 4"
    head -5 "$W/$s$m" | grep -v "^  "
  done
  for s in chord play walk; do sed "s/^/  $TAG $s compute: /" "$W/cy$s$m"; done
done
rm -f "$B"/fp_*_"$TAG".elf "$B"/cy_*_"$TAG".elf
