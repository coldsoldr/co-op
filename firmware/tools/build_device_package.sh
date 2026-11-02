#!/usr/bin/env bash
# Build a flashable .op1 for the device from the combined ELF: build_image.sh --device (re-encrypted L1),
# the stock content directory without te-boot.ldr, build_package.py, then the two mandatory checks
# (tools/verify_bootloader.py, tools/pkg_check.py; CLAUDE.md hard rules). Runs in WSL.
#
#   tools/build_device_package.sh OUT.op1 [--set-word NAME=VALUE ...]
#   STOCK_DIR=path   the unpacked stock firmware (default ../te-op1-cfw/op1 firmware/op1_246)
#
# The owner flashes; record the result in docs/device-tests.md. Read docs/brick-safety.md first.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:?usage: build_device_package.sh OUT.op1 [--set-word NAME=VALUE ...]}
shift
STOCK_DIR=${STOCK_DIR:-$here/../te-op1-cfw/op1 firmware/op1_246}
[ -f "$STOCK_DIR/OP1_vdk.ldr" ] || { echo "stock firmware dir not found: $STOCK_DIR"; exit 1; }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
STOCK_LDR="$STOCK_DIR/OP1_vdk.ldr" bash "$here/tools/build_image.sh" "$work/plain.ldr" "$@" --device "$work/OP1_vdk.ldr"
mkdir -p "$work/fw"
cp -r "$STOCK_DIR/content" "$work/fw/content"
cp "$work/OP1_vdk.ldr" "$work/fw/OP1_vdk.ldr"          # no te-boot.ldr: the bootloader is never touched
mkdir -p "$(dirname "$OUT")"
python3 -I "$here/tools/build_package.py" "$work/fw" "$OUT"
python3 -I "$here/tools/verify_bootloader.py" "$OUT"
python3 -I "$here/tools/pkg_check.py" "$OUT" "$STOCK_DIR"
sha256sum "$OUT"
