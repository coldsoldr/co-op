#!/usr/bin/env bash
# Build tests/sim/kern_check.cc for the Blackfin and run it on the bfin simulator (bfin-elf-run): the T4 assembly
# kernels in coop_stereo.h against their C references, bit for bit, for the actual device codegen.
set -e
BIN=${BFIN_BIN:-$HOME/op1-toolchain/bfin/bin}
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT=$(mktemp -d)
"$BIN/bfin-elf-g++" -O2 -std=c++17 -mcpu=bf524 -msim -I"$HERE/../../core/include" \
    "$HERE/kern_check.cc" -o "$OUT/kern_check"
"$BIN/bfin-elf-run" "$OUT/kern_check"
