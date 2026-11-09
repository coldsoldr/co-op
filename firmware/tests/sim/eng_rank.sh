#!/usr/bin/env bash
# Rank the synth engines in op1emu: for each factory sound slot, a six-note chord held 10 s (det_dsynth6 with the slot
# swapped), and the per-voice-render cost of the first engine heard (g_coop_cost[COOP_COST_ENG], index 15) and its
# render function (g_coop_eng_fn[0]). One run at a time under the emulator lock. Run from the repo root in WSL after
# `make -C core/sdram combined`.
#   tests/sim/eng_rank.sh [SLOTS...]        (default s1 .. s8)
set -euo pipefail
W=${OP1EMU_WORK:-$HOME/op1emu-work}; T=$(pwd)
NM=~/op1-toolchain/bfin/bin/bfin-elf-nm
ELF=build/core/coop_combined.elf
COST=$($NM $ELF | awk '$3=="_g_coop_cost"{print $1}'); FN=$($NM $ELF | awk '$3=="_g_coop_eng_fn"{print $1}')
E=$(printf '0x%x' $((0x$COST + 16 * 15)))
flock "$W/emu.lock" tools/build_image.sh "$W/rank.ldr" --tape-stereo --set-word g_coop_spread=32767 --set-word g_coop_force_stereo_out=1 > /dev/null
for s in ${@:-s1 s2 s3 s4 s5 s6 s7 s8}; do
  sed "s/^tap s5 200$/tap $s 200/" tests/sim/det_dsynth6.txt > "$W/rank_$s.txt"
  cp -f "$W/pristine.bin" "$W/rank_nand.bin"
  (cd "$W/op1emu" && flock "$W/emu.lock" timeout 1500 ./build/op1emu "$W/rank_nand.bin" "$W/rank.ldr" --headless --deterministic \
      --watch-writes "$E+4" --watch-writes "0x$FN+4" --watch-log 2000000 --watch-from 0 --input-script "$W/rank_$s.txt" > "$W/rank_$s.log" 2>&1 || true)
  grep "store t=" "$W/rank_$s.log" | awk -v e="$E" -v f="0x$FN" '
    { for (i = 1; i <= NF; i++) { if ($i ~ /^addr=/) a = strtonum(substr($i, 6)); if ($i ~ /^value=/) v = strtonum(substr($i, 7)) }
      if (a == strtonum(f) && v) fn = v; else if (a == strtonum(e)) { if (p) { d += v - p; m++ } p = v } }
    END { printf "%s render 0x%08x  %d cycles per voice render over %d renders\n", "'$s'", fn, m ? d / m : 0, m }'
done
