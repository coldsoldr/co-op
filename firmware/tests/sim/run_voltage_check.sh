#!/usr/bin/env bash
# Bit-exactness check of core/sdram/voltage_fast.S on the bfin simulator (bfin-elf-run), against the stock Voltage
# render itself: tests/sim/voltage_transcribe.py turns the stock disassembly (objdump of the decrypted R246 image, kept
# outside the repo) into re-assemblable source in a temp directory, with the data addresses as test arrays and the
# loop's LSETUP as the JUMP.L to coop_voltage_loop_near; voltage_check.cc then runs that render with
# g_coop_fast_voltage = 0 (the stock loop, entered by hand as on the device) and = 1 (the rewrite) on copies of random
# voices, tables and pitch words, the state carried over 2400 calls, and compares voice, output and display copy.
#
#   tests/sim/run_voltage_check.sh STOCK_DIS     (WSL; STOCK_DIS = the objdump text of the stock image)
set -e
DIS=${1:?usage: run_voltage_check.sh STOCK_DIS}
BIN=${BFIN_BIN:-$HOME/op1-toolchain/bfin/bin}
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT=$(mktemp -d)
python3 -I "$HERE/voltage_transcribe.py" "$DIS" "$OUT/stock_voltage.S"
"$BIN/bfin-elf-g++" -O2 -std=c++17 -mcpu=bf524 -msim -DCOOP_VOLTAGE_CHECK "$HERE/voltage_check.cc" "$OUT/stock_voltage.S" \
    "$HERE/../../core/sdram/voltage_fast.S" -o "$OUT/voltage_check"
"$BIN/bfin-elf-run" "$OUT/voltage_check"
rm -rf "$OUT"
