#!/usr/bin/env bash
# Launch op1emu's interactive GUI (window + host audio) on a coop-fw image, so a human can play it.
# Mouse-driven: click the rendered OP-1's buttons and keys. Runs on a fresh copy of the NAND each
# time so the pristine image is never mutated. Requires the op1emu work tree from tools/setup_op1emu.sh
# (GUI build at build/op1emu) and, on Windows, WSLg for the window + sound.
#
#   tests/sim/play.sh [LDR]      # LDR defaults to the milestone-B image ~/op1emu-work/mbF.ldr
set -e
WORK=${OP1EMU_WORK:-$HOME/op1emu-work}
LDR=${1:-$WORK/mbF.ldr}
[ -f "$LDR" ] || { echo "LDR not found: $LDR"; exit 1; }
[ -f "$WORK/pristine.bin" ] || { echo "pristine NAND not found: $WORK/pristine.bin"; exit 1; }
cp "$WORK/pristine.bin" "$WORK/play_nand.bin"
echo "Launching op1emu GUI on $(basename "$LDR") (fresh NAND). Close the window to quit."
cd "$WORK/op1emu"                     # op1emu reads gui/ui.json + otp.bin from its cwd
exec ./build/op1emu "$WORK/play_nand.bin" "$LDR"
