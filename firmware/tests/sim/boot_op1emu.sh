#!/usr/bin/env bash
# Boot a coop-fw plaintext LDR in op1emu headless and save a PNG screenshot.
# Reuses the op1emu + nand.bin + otp.bin set up by tools/setup_op1emu.sh (default ~/op1emu-work).
#
#   bash tests/sim/boot_op1emu.sh <plaintext.ldr> [out.png] [wait_ms]
#
# op1emu must run from its source root (it reads gui/ui.json and otp.bin from the cwd).
set -euo pipefail
LDR="${1:?usage: boot_op1emu.sh <plaintext.ldr> [out.png] [wait_ms]}"
OUT="${2:-boot.png}"
WAIT="${3:-90000}"
WORK="${WORK:-$HOME/op1emu-work}"
EMU="$WORK/op1emu"
test -f "$EMU/build/op1emu" || { echo "op1emu not built; run tools/setup_op1emu.sh"; exit 1; }
test -f "$WORK/nand.bin" || { echo "no nand.bin; run tools/setup_op1emu.sh"; exit 1; }
cp -f "$WORK/otp.bin" "$EMU/otp.bin" 2>/dev/null || true

ppm="$(mktemp --suffix=.ppm)"
scr="$(mktemp --suffix=.txt)"
printf 'accel 64 -64 512\nwait-frames 1 300000\nwait %s\nscreenshot %s\nquit\n' "$WAIT" "$ppm" > "$scr"
( cd "$EMU" && timeout 500 ./build/op1emu "$WORK/nand.bin" "$(realpath "$LDR")" --headless --input-script "$scr" ) | tail -4
test -f "$ppm" || { echo "no screenshot produced"; exit 1; }

# PPM (P6) -> PNG with stdlib only
python3 - "$ppm" "$OUT" <<'PY'
import sys, struct, zlib
f = open(sys.argv[1], 'rb'); f.readline()
tok = []
while len(tok) < 3:
    line = f.readline()
    if not line.startswith(b'#'):
        tok += line.split()
w, h, _ = (int(t) for t in tok[:3]); data = f.read(w*h*3)
raw = bytearray()
for y in range(h):
    raw.append(0); raw += data[y*w*3:(y+1)*w*3]
ch = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t+d) & 0xffffffff)
open(sys.argv[2], 'wb').write(b'\x89PNG\r\n\x1a\n'
    + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    + ch(b'IDAT', zlib.compress(bytes(raw), 9)) + ch(b'IEND', b''))
print('wrote', sys.argv[2], w, 'x', h)
PY
rm -f "$ppm" "$scr"
