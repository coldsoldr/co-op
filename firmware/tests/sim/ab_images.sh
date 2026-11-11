#!/usr/bin/env bash
# Compare two built op1emu images on one scenario, one run at a time (docs/emulator.md: never in parallel): the
# regression gate for a change that has no switch word (instrumentation, a new stage), e.g. the previous package's
# image against the new one. Prints faults, the callback's average emulator cycles per block from guest second FROM
# (g_last of each image's own ELF), the sample compare and the per-second levels. Run from the repo root in WSL.
#
#   tests/sim/ab_images.sh A.ldr A.elf B.ldr B.elf SCENARIO [FROM]
#   tests/sim/ab_images.sh ~/op1emu-work/p4b_ref.ldr ../coop-fw-p4b/build/core/coop_combined.elf \
#       ~/op1emu-work/new.ldr build/core/coop_combined.elf det_drum
set -euo pipefail
A=${1:?image A}; AE=${2:?elf A}; B=${3:?image B}; BE=${4:?elf B}; SC=${5:?scenario}; FROM=${6:-34}
W=${OP1EMU_WORK:-$HOME/op1emu-work}; T=$(pwd)
NM=~/op1-toolchain/bfin/bin/bfin-elf-nm
n=0
for pair in "$A|$AE" "$B|$BE"; do
  img=${pair%%|*}; elf=${pair##*|}
  LAST=$($NM "$elf" | awk '$3=="_g_last"{print $1}')
  cp -f "$W/pristine.bin" "$W/abi_nand.bin"
  (cd "$W/op1emu" && timeout 1500 ./build/op1emu "$W/abi_nand.bin" "$img" --headless --deterministic \
      --audio-dump "$W/abi_$n.wav" --watch-writes "0x$LAST+4" --watch-log 400 --watch-from "$FROM" \
      --input-script "$T/tests/sim/$SC.txt" > "$W/abi_$n.log" 2>&1 || true)
  echo "$(basename "$img"): faults $(grep -ci 'fault\|exception\|assert' "$W/abi_$n.log")"
  grep "store t=" "$W/abi_$n.log" | awk '{ for (i = 1; i <= NF; i++) if ($i ~ /^value=/) { s += strtonum(substr($i, 7)); k++ } }
      END { if (k) printf "  callback avg %d cycles per block over %d blocks\n", s / k, k }'
  n=$((n + 1))
done
python3 -I "$T/tools/wav_compare.py" "$W/abi_0.wav" "$W/abi_1.wav" --align 200 | tail -1 || true
python3 -I "$T/tools/wav_levels.py" "$W/abi_0.wav" "$W/abi_1.wav" --from 26 --seconds 26 || true
