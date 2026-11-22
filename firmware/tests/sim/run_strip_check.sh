#!/usr/bin/env bash
# Build tests/sim/strip_check.cc for the Blackfin and run it on the bfin simulator (bfin-elf-run): the channel
# strip's kernels (core/sdram/strip_fast.S) against their C model (core/include/coop_strip_bf.h), bit for bit,
# guards around every array, then the whole strip with the assembly against the C (docs/channel-strip.md "CS2").
set -e
BIN=${BFIN_BIN:-$HOME/op1-toolchain/bfin/bin}
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
"$BIN/bfin-elf-g++" -O2 -std=c++17 -mcpu=bf524 -msim -fno-tree-loop-distribute-patterns -I"$HERE/../../core/include" \
    "$HERE/strip_check.cc" "$HERE/../../core/sdram/strip_fast.S" -o "$OUT/strip_check"
"$BIN/bfin-elf-run" "$OUT/strip_check"
python3 -I "$HERE/../../tools/asm_lint.py" "$HERE/../../core/sdram/strip_fast.S"
