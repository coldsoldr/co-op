#!/usr/bin/env bash
# Build a coop-fw image for op1emu (plaintext LDR) from the combined ELF, and optionally the encrypted
# LDR for the device. Runs in WSL (needs python3; the toolchain is not needed, the ELF is an input).
#
#   tools/build_image.sh OUT.ldr [--set-word NAME=VALUE ...] [--device OUT_enc.ldr]
#
#   OUT.ldr                  plaintext image op1emu takes directly (tests/sim/hear.sh, play.sh)
#   --set-word g_coop_spread=32767      image-time controls (inject_app --set-word; see stereo_bus.cc,
#   --set-word g_coop_master_flags=1    master_suite.cc for the words)
#   --device OUT_enc.ldr     also re-encrypt the L1 for the device (l1_decrypt encrypt-ldr)
#   ELF=path                 the payload (default build/core/coop_combined.elf)
#   STOCK_LDR=path           the stock encrypted OP1_vdk.ldr (default ../te-op1-cfw/op1 firmware/op1_246/OP1_vdk.ldr)
#   OTP_BIN=path             the OTP image whose page 0xD0 holds the XTEA key (default ~/op1emu-work/otp.bin)
#
# Pipeline: inject_app (region A payload + callback imm seam) -> l1_decrypt decrypt-ldr (plaintext L1,
# key read from otp.bin at run time, never stored) -> l1_insert --imm-pair-site (coop master stage at the
# getter site, docs/audio-graph.md) -> l1_stereo_insert (milestone C1 trampolines + site rewrites).
# Every L1 patch checks the stock bytes before writing (CLAUDE.md hard rules).
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:?usage: build_image.sh OUT.ldr [--set-word NAME=VALUE ...] [--device OUT_enc.ldr]}
shift
ELF=${ELF:-$here/build/core/coop_combined.elf}
STOCK_LDR=${STOCK_LDR:-$here/../te-op1-cfw/op1 firmware/op1_246/OP1_vdk.ldr}
OTP_BIN=${OTP_BIN:-$HOME/op1emu-work/otp.bin}
PATCHES=$here/third_party/kennel/synth_sdk/l1_patches.json
setwords=()
device=""
while [ $# -gt 0 ]; do
  case "$1" in
    --set-word) setwords+=(--set-word "$2"); shift 2 ;;
    --device) device=$2; shift 2 ;;
    *) echo "unknown argument: $1"; exit 2 ;;
  esac
done
[ -f "$ELF" ] || { echo "ELF not found: $ELF (make -C core/sdram combined)"; exit 1; }
[ -f "$STOCK_LDR" ] || { echo "stock LDR not found: $STOCK_LDR"; exit 1; }
[ -f "$OTP_BIN" ] || { echo "otp.bin not found: $OTP_BIN"; exit 1; }

key=$(xxd -s 3328 -l 16 -p "$OTP_BIN")          # page 0xD0; stays in this shell only
master_fn=$(cd "$here/tools" && python3 -I -c "
import sys; sys.path.insert(0, '.')
from inject_app import read_elf_symbols, symbol_address
print('%#x' % symbol_address(read_elf_symbols(sys.argv[1]), 'coop_master_apply')[0])" "$ELF")

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
python3 -I "$here/tools/inject_app.py" "$STOCK_LDR" "$work/inj.ldr" "$ELF" "${setwords[@]}"
python3 -I "$here/tools/l1_decrypt.py" decrypt-ldr "$work/inj.ldr" "$key" "$work/plain.ldr" --patches "$PATCHES"
python3 -I "$here/tools/l1_insert.py" "$work/plain.ldr" "$work/master.ldr" \
    --imm-pair-site 0xffa017a4 --imm-stock 0x019d90e8 --fn "$master_fn"
python3 -I "$here/tools/l1_stereo_insert.py" "$work/master.ldr" "$OUT" --elf "$ELF"
echo "# $OUT: plaintext coop image (op1emu); payload $ELF; master stage at $master_fn"
if [ -n "$device" ]; then
  python3 -I "$here/tools/l1_decrypt.py" encrypt-ldr "$OUT" "$key" "$device" --patches "$PATCHES"
  echo "# $device: L1 re-encrypted for the device (package with tools/build_package.py; run verify_bootloader + pkg_check)"
fi
