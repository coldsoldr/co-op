#!/usr/bin/env bash
# A/B a performance rewrite in op1emu: two images that differ only in one switch word (0 = stock path, 1 = the
# rewrite), one scenario on each, one at a time. Prints the audio callback's average emulator cycles per block
# (g_last, from guest second FROM), the per-call cost of one g_coop_cost entry if asked, faults, the sample compare
# and the per-second levels (tools/wav_levels.py). Run from the repo root in WSL after `make -C core/sdram combined`.
#
#   tests/sim/ab_switch.sh SWITCH SCENARIO [COST_INDEX] [FROM]
#   tests/sim/ab_switch.sh g_coop_fast_cwo det_dsynth6_cwo 12 41
#
# COST_INDEX: core/include/coop_cost.h (0 stage, 1 s2 instr, 2 s2 master, 3 master, 4 fx1 plugin, 5 + stock type).
# Pass: no faults, identical samples, or equal levels with a whole-block offset (see tools/wav_levels.py). Run every
# rewrite in at least two scenarios (one that exercises the code with its inputs moving: knobs, modulation, chords
# starting and stopping): a value that looks constant in one run may follow the signal in another.
set -euo pipefail
SW=${1:?switch word}; SC=${2:?scenario name in tests/sim}; IDX=${3:-}; FROM=${4:-34}
W=${OP1EMU_WORK:-$HOME/op1emu-work}; T=$(pwd)
NM=~/op1-toolchain/bfin/bin/bfin-elf-nm
B="--tape-stereo --set-word g_coop_spread=32767 --set-word g_coop_force_stereo_out=1"
for v in 0 1; do tools/build_image.sh "$W/ab_$v.ldr" $B --set-word "$SW=$v" > /dev/null; done
LAST=$($NM build/core/coop_combined.elf | awk '$3=="_g_last"{print $1}')
ARGS=(--watch-writes "0x$LAST+4")
if [ -n "$IDX" ]; then
  COST=$($NM build/core/coop_combined.elf | awk '$3=="_g_coop_cost"{print $1}')
  ARGS+=(--watch-writes "$(printf '0x%x' $((0x$COST + 16 * IDX)))+4")
fi
cd "$W/op1emu"
for v in 0 1; do
  cp -f "$W/pristine.bin" "$W/ab_nand.bin"
  timeout 1500 ./build/op1emu "$W/ab_nand.bin" "$W/ab_$v.ldr" --headless --deterministic --audio-dump "$W/ab_$v.wav" \
      "${ARGS[@]}" --watch-log 400 --watch-from "$FROM" --watch-out "$W/ab_$v.tsv" \
      --input-script "$T/tests/sim/$SC.txt" > "$W/ab_$v.log" 2>&1 || true
  echo "$SW=$v: faults $(grep -ci 'fault\|exception\|assert' "$W/ab_$v.log")"
  grep "store t=" "$W/ab_$v.log" | awk -v last="0x$LAST" '
    { for (i = 1; i <= NF; i++) { if ($i ~ /^addr=/) a = substr($i, 6); if ($i ~ /^value=/) v = strtonum(substr($i, 7)) }
      if (strtonum(a) == strtonum(last)) { s += v; n++ } else { if (p) { d += v - p; m++ } p = v } }
    END { if (n) printf "  callback avg %d cycles per block over %d blocks\n", s / n, n;
          if (m) printf "  cost entry avg %d cycles per call over %d calls\n", d / m, m }'
done
python3 -I "$T/tools/wav_compare.py" "$W/ab_0.wav" "$W/ab_1.wav" --align 200 | tail -1
python3 -I "$T/tools/wav_levels.py" "$W/ab_0.wav" "$W/ab_1.wav" --from 26 --seconds 26
