#!/usr/bin/env bash
# Run any OP-1 firmware package in op1emu's window (WSLg on Windows 11): coop, stock, SilverTape, opus, hundekraft.
#
#   tools/op1emu_gui.sh ~/Downloads/SilverTape-beta2.op1            # name from the file: fw_SilverTape-beta2
#   tools/op1emu_gui.sh build/pkg/op1_246_coop_p5f_perf.op1 coop    # or give the name
#   FRESH=1 tools/op1emu_gui.sh PACKAGE.op1                          # rebuild the NAND (a factory-fresh OP-1)
#
# First run per name: unpack the package (tools/op1_unpack.py), decrypt its L1 with the key in otp.bin (op1emu boots a
# plaintext LDR), and build a NAND from the package's own content (tools/make_nand.py). Both stay in
# ~/op1emu-work/fw_NAME/, and the NAND keeps what you do between runs. Needs the op1emu work tree
# (tools/setup_op1emu.sh) with coop's patches, 0003 for the controls below.
#
# Controls (gui/ui.json "keymap", patch 0003):
#   keyboard    z s x d c v b h n j m = F3..E4,  q 2 w 3 e 4 r t 6 y 7 u = F4..E5
#   modes       F1 synth  F2 drum  F3 tape  F4 mixer        T1..T4 = F5..F8
#   sounds      keypad 1..8 or Ctrl+1..8 = the eight sound slots
#   shift       Left or Right Shift
#   transport   Space play  Enter rec  Backspace stop  Left / Right arrows
#   tape        Home lift  End drop  Page Down split
#   other       Tab seq  F9 mic  F10 com  F11 tempo (metronome)  F12 help
#   encoders    drag up / right = clockwise, down / left = anticlockwise; mouse wheel = one detent per notch;
#               a click without moving = the encoder's push; the volume knob drags and wheels the same way
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
WORK=${OP1EMU_WORK:-$HOME/op1emu-work}
PKG=${1:?usage: op1emu_gui.sh PACKAGE.op1 [NAME]}
PKG=$(realpath "$PKG")
NAME=${2:-$(basename "${PKG%.*}")}
DIR=$WORK/fw_$NAME
mkdir -p "$DIR/overlay"
LOCK=$WORK/emu.lock

if [ ! -f "$DIR/plain.ldr" ]; then
  echo "== $NAME: unpack and decrypt"
  [ -d "$DIR/unpacked/fw" ] || python3 -I "$here/tools/op1_unpack.py" "$PKG" "$DIR/unpacked" > "$DIR/files.txt"
  key=$(xxd -s 3328 -l 16 -p "$WORK/otp.bin")          # page 0xD0; stays in this shell
  python3 -I "$here/tools/l1_decrypt.py" decrypt-ldr "$DIR/unpacked/fw/OP1_vdk.ldr" "$key" "$DIR/plain.ldr" \
      --patches "$here/third_party/kennel/synth_sdk/l1_patches.json" | tail -1
fi
if [ -n "${FRESH:-}" ] || [ ! -f "$DIR/nand.bin" ]; then
  echo "== $NAME: NAND from the package"
  [ -f "$DIR/nand.bin" ] && mv -f "$DIR/nand.bin" "$DIR/nand.old.bin"
  flock "$LOCK" python3 -I "$here/tools/make_nand.py" --provision "$WORK/provision_nand.py" --firmware "$PKG" \
      --mkyaffs2 "$WORK/yaffs2utils/mkyaffs2" --overlay "$DIR/overlay" --output "$DIR/nand.bin" | tail -1
fi
sed -n '/^# Controls/,/^set -euo/p' "$0" | sed '$d'
cd "$WORK/op1emu"
exec ./build/op1emu "$DIR/nand.bin" "$DIR/plain.ldr"
