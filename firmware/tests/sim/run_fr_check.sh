#!/usr/bin/env bash
# Build tests/sim/fr_check.cc for the Blackfin and run it on the bfin simulator (bfin-elf-run),
# verifying coop_mul_fr / the biquad against the int64 reference for the actual device codegen.
set -e
BIN=${BFIN_BIN:-$HOME/op1-toolchain/bfin/bin}
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT=$(mktemp -d)
"$BIN/bfin-elf-g++" -O2 -std=c++17 -mcpu=bf524 -msim -I"$HERE/../../core/include" \
    "$HERE/fr_check.cc" -o "$OUT/fr_check"
"$BIN/bfin-elf-run" "$OUT/fr_check"
