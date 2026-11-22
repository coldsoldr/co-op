#!/usr/bin/env bash
# Build tests/sim/wrap_check.cc for the Blackfin and run it on the bfin simulator (bfin-elf-run): e15's effect-wrap
# kernels (core/sdram/wrap_fast.S) against the C loops of plugin_host.cc, bit for bit, guards around every array.
set -e
BIN=${BFIN_BIN:-$HOME/op1-toolchain/bfin/bin}
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
"$BIN/bfin-elf-g++" -O2 -std=c++17 -mcpu=bf524 -msim -I"$HERE/../../core/include" \
    "$HERE/wrap_check.cc" "$HERE/../../core/sdram/wrap_fast.S" -o "$OUT/wrap_check"
"$BIN/bfin-elf-run" "$OUT/wrap_check"
python3 -I "$HERE/../../tools/asm_lint.py" "$HERE/../../core/sdram/wrap_fast.S"
