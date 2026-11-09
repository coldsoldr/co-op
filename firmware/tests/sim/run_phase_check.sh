#!/usr/bin/env bash
# Bit-exactness check of core/sdram/phase_fast.S on the bfin simulator (bfin-elf-run), against the stock Phase render
# itself: tests/sim/phase_transcribe.py turns the stock disassembly (objdump of the decrypted R246 image, kept outside
# the repo) into re-assemblable source in a temp directory, with the data addresses as test arrays and the loop's
# LSETUP as the JUMP.L to coop_phase_loop_near; phase_check.cc then runs that render with g_coop_fast_phase = 0 (the
# stock loop, entered by hand as on the device) and = 1 (the rewrite) on copies of random voices, tables and pitch
# words, the state carried over the calls, and compares voice, output and the global accumulator.
#
#   tests/sim/run_phase_check.sh STOCK_DIS     (WSL; STOCK_DIS = the objdump text of the stock image)
set -e
DIS=${1:?usage: run_phase_check.sh STOCK_DIS}
BIN=${BFIN_BIN:-$HOME/op1-toolchain/bfin/bin}
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT=$(mktemp -d)
python3 -I "$HERE/phase_transcribe.py" "$DIS" "$OUT/stock_phase.S"
"$BIN/bfin-elf-g++" -O2 -std=c++17 -mcpu=bf524 -msim -DCOOP_PHASE_CHECK "$HERE/phase_check.cc" "$OUT/stock_phase.S" \
    "$HERE/../../core/sdram/phase_fast.S" -o "$OUT/phase_check"
"$BIN/bfin-elf-run" "$OUT/phase_check"
rm -rf "$OUT"
