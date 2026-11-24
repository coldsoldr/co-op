#!/usr/bin/env bash
# Build tests/sim/strip_bench.cc for the Blackfin with the firmware's flags and run it on the bfin simulator:
# the channel strip's cycles per track and block, reference and Blackfin kernels (docs/channel-strip.md).
set -e
BIN=${BFIN_BIN:-$HOME/op1-toolchain/bfin/bin}
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
"$BIN/bfin-elf-g++" -O2 -std=c++17 -mcpu=bf524 -msim -fno-tree-loop-distribute-patterns -I"$HERE/../../core/include" \
    "$HERE/strip_bench.cc" "$HERE/../../core/sdram/strip_fast.S" -o "$OUT/strip_bench"
"$BIN/bfin-elf-run" "$OUT/strip_bench"
