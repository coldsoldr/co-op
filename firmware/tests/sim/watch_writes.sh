#!/usr/bin/env bash
# Who writes a guest buffer? Run an LDR headless in op1emu with its write tracer on one or more
# address ranges and print the per-writer-PC table (stores, nonzero stores, widths, address span,
# first/last guest time). Needs the op1emu work tree from tools/setup_op1emu.sh with the coop patch
# tools/op1emu-patches/0001-write-trace.patch applied (setup_op1emu.sh applies it).
#
#   tests/sim/watch_writes.sh LDR SCRIPT RANGE [RANGE...]        # table on stdout
#   WATCH_LOG=1400 WATCH_FROM=23.0 tests/sim/watch_writes.sh ...  # also log 1400 single stores
#                                                                 # from guest second 23 (order in a block)
#   OUT=/path/table.tsv   where to keep the TSV (default: $WORK/watch.tsv)
#
# RANGE is LO-HI, LO+LEN or LO (hex). Example, the voice bus and the master pair on a stock image:
#   tests/sim/watch_writes.sh ~/op1emu-work/stock.ldr ~/op1emu-work/cap.txt 0xff800bb4+0x100 0xff800d34+0x100
#
# Reads the guest clock: the audio callback runs from ~1.96 s guest time; script waits are wall time.
set -euo pipefail
WORK=${OP1EMU_WORK:-$HOME/op1emu-work}
LDR=${1:?usage: watch_writes.sh LDR SCRIPT RANGE [RANGE...]}
SCRIPT=${2:?usage: watch_writes.sh LDR SCRIPT RANGE [RANGE...]}
shift 2
[ $# -ge 1 ] || { echo "need at least one RANGE"; exit 1; }
OUT=${OUT:-$WORK/watch.tsv}
NAND=$WORK/watch_nand_$$.bin               # per-run NAND copy: runs may overlap
LOG=${LOG:-$WORK/watch.log}
EMU="$WORK/op1emu/build/op1emu"
[ -x "$EMU" ] || { echo "op1emu not built: $EMU"; exit 1; }
"$EMU" --help | grep -q -- --watch-writes || { echo "this op1emu lacks --watch-writes; apply tools/op1emu-patches and rebuild"; exit 1; }
[ -f "$WORK/pristine.bin" ] || { echo "pristine NAND not found: $WORK/pristine.bin"; exit 1; }
LDR=$(realpath "$LDR"); SCRIPT=$(realpath "$SCRIPT")

args=()
for r in "$@"; do args+=(--watch-writes "$r"); done
[ -n "${WATCH_LOG:-}" ] && args+=(--watch-log "$WATCH_LOG" --watch-from "${WATCH_FROM:-0}")

if [ -n "${NAND_KEEP:-}" ]; then          # a persistent NAND (power-cycle tests), made from pristine if missing
  NAND=$NAND_KEEP; [ -f "$NAND" ] || cp -f "$WORK/pristine.bin" "$NAND"
else
  cp -f "$WORK/pristine.bin" "$NAND"      # op1emu mutates its NAND; never the pristine one
  trap 'rm -f "$NAND"' EXIT
fi
cd "$WORK/op1emu"                                       # it reads gui/ui.json + otp.bin from its cwd
timeout "${TIMEOUT:-600}" ./build/op1emu "$NAND" "$LDR" --headless --deterministic \
    "${args[@]}" --watch-out "$OUT" --input-script "$SCRIPT" > "$LOG" 2>&1 || {
  echo "op1emu failed (exit $?); see $LOG"; tail -5 "$LOG"; exit 1; }
column -t -s $'\t' "$OUT"
echo "# table: $OUT   full log: $LOG"
