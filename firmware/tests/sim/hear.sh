#!/usr/bin/env bash
# Hear a headless op1emu run: write what the codec would play (the SPORT transmit frames) to a WAV.
# This is the end-to-end "does the firmware make sound" test; bus probes are not (milestone B's image
# passed every bus probe and produced silence here, docs/audio-graph.md second correction).
#
#   tests/sim/hear.sh LDR SCRIPT OUT.wav        # deterministic headless run, 16-bit stereo 44.1 kHz
#   COPY_TO=/mnt/c/Users/you/Downloads tests/sim/hear.sh ...   # also copy the WAV there to listen
#   NAND_KEEP=/path/nand.bin tests/sim/hear.sh ...               # keep (and reuse) a NAND copy across runs: power cycles
#
# Needs the op1emu work tree from tools/setup_op1emu.sh with tools/op1emu-patches applied (--audio-dump).
set -euo pipefail
WORK=${OP1EMU_WORK:-$HOME/op1emu-work}
LDR=${1:?usage: hear.sh LDR SCRIPT OUT.wav}
SCRIPT=${2:?usage: hear.sh LDR SCRIPT OUT.wav}
OUT=${3:?usage: hear.sh LDR SCRIPT OUT.wav}
EMU="$WORK/op1emu/build/op1emu"
[ -x "$EMU" ] || { echo "op1emu not built: $EMU"; exit 1; }
"$EMU" --help 2>&1 | grep -c -- --audio-dump >/dev/null ||   # grep -c reads it all: -q under pipefail could SIGPIPE op1emu (a false "lacks")
  { echo "this op1emu lacks --audio-dump; apply tools/op1emu-patches and rebuild"; exit 1; }
[ -f "$WORK/pristine.bin" ] || { echo "pristine NAND not found: $WORK/pristine.bin"; exit 1; }
LDR=$(realpath "$LDR"); SCRIPT=$(realpath "$SCRIPT"); OUT=$(realpath -m "$OUT")
LOG=${LOG:-${OUT%.wav}.log}
if [ -n "${NAND_KEEP:-}" ]; then          # a persistent NAND (power-cycle tests): made from pristine if missing, kept
  NAND=$NAND_KEEP
  [ -f "$NAND" ] || cp -f "$WORK/pristine.bin" "$NAND"
else
  NAND=$WORK/hear_nand_$$.bin               # per-run NAND copy: runs may overlap
  cp -f "$WORK/pristine.bin" "$NAND"
  trap 'rm -f "$NAND"' EXIT
fi
cd "$WORK/op1emu"
timeout "${TIMEOUT:-600}" ./build/op1emu "$NAND" "$LDR" --headless --deterministic \
    --audio-dump "$OUT" --input-script "$SCRIPT" > "$LOG" 2>&1 || { echo "op1emu failed; see $LOG"; tail -5 "$LOG"; exit 1; }
grep "Audio dump:" "$LOG" | tail -2
[ -n "${COPY_TO:-}" ] && cp -f "$OUT" "$COPY_TO/" && echo "copied to $COPY_TO/$(basename "$OUT")"
echo "# wav: $OUT   log: $LOG"
