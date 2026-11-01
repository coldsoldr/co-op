#!/usr/bin/env bash
# Set up op1emu as coop-fw's test harness (see docs/emulator.md). Run this in WSL:
#
#   wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/coop-fw && OTP_TXT=/path/to/otp.txt bash tools/setup_op1emu.sh'
#
# It builds third-party code (op1emu + GLFW via CMake FetchContent, and mkyaffs2 from yaffs2utils),
# so Claude Code's auto mode will not run it unattended; run it yourself or grant the permission.
#
# Inputs:
#   FW       the stock firmware package      (default: incoming/op1_246.op1)
#   OTP_TXT  a community OTP text dump        (e.g. Tolsi/op1dumps TE-OP-1-BF524-OTP-dump.txt)
#            if unset, this fetches that public dump (the key is the same on every OP-1)
#   WORK     where to build/run              (default: ~/op1emu-work)
#
# Output in $WORK: op1emu (the emulator), nand.bin, otp.bin, and boot.ppm (a screenshot proving it
# booted). None of these are committed; they are the owner's own device material.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
FW="${FW:-$REPO/incoming/op1_246.op1}"
WORK="${WORK:-$HOME/op1emu-work}"
JOBS="$(nproc)"
mkdir -p "$WORK"; cd "$WORK"

echo "== deps =="
export DEBIAN_FRONTEND=noninteractive
sudo apt-get update -qq
sudo apt-get install -y -qq ninja-build pkg-config libgl1-mesa-dev xorg-dev python3-pip git curl >/dev/null
python3 -m pip install --user -q "cmake>=3.20"
export PATH="$HOME/.local/bin:$PATH"

echo "== op1emu (clone + build op1emu, ldrdump) =="
[ -d op1emu ] || git clone --recurse-submodules -j"$JOBS" https://github.com/op1emu/op1emu.git
cmake -S op1emu -B op1emu/build -G Ninja -DGLFW_BUILD_WAYLAND=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build op1emu/build --target op1emu ldrdump -j"$JOBS"

echo "== mkyaffs2 (yaffs2utils) =="
[ -d yaffs2utils ] || git clone --depth 1 https://github.com/ljvblfz/yaffs2utils.git
make -C yaffs2utils CC=gcc mkyaffs2

echo "== provision_nand.py (op1emu PR #76, GosuBoy branch) =="
curl -fsSL "https://raw.githubusercontent.com/GosuBoy/op1emu/5320259677ac637787b303c1804662c613b8fbec/tools/provision_nand.py" -o provision_nand.py
python3 provision_nand.py --firmware "$FW" --output nand.bin --mkyaffs2 "$PWD/yaffs2utils/mkyaffs2"

echo "== otp.bin =="
if [ -z "${OTP_TXT:-}" ]; then
  echo "   fetching the public OTP dump (Tolsi/op1dumps); the key is identical on every OP-1"
  curl -fsSL "https://raw.githubusercontent.com/Tolsi/op1dumps/master/TE-OP-1-BF524-OTP-dump.txt" -o otp.txt
  OTP_TXT="$PWD/otp.txt"
fi
python3 "$REPO/tools/otp_txt_to_bin.py" "$OTP_TXT" otp.bin

echo "== verify the key against kennel's known pairs (our gate) =="
KEYHEX="$(python3 - "$PWD/otp.bin" <<'PY'
import sys; d=open(sys.argv[1],'rb').read(); print(d[0xD0*16:0xD0*16+16].hex())
PY
)"
python3 "$REPO/tools/l1_decrypt.py" check-key "$REPO/third_party/kennel/synth_sdk/l1_patches.json" >/dev/null 2>&1 || true
# the stock LDR for the check: extract it from the firmware package if present
if [ -f "$REPO/../te-op1-cfw/op1 firmware/op1_246/OP1_vdk.ldr" ]; then
  python3 "$REPO/tools/l1_decrypt.py" check-key "$REPO/../te-op1-cfw/op1 firmware/op1_246/OP1_vdk.ldr" \
    "$REPO/third_party/kennel/synth_sdk/l1_patches.json" "$KEYHEX" || echo "   (key check could not run; continuing)"
fi

echo "== boot the stock firmware headless and screenshot =="
# op1emu reads otp.bin and bootrom from the working dir; the desktop build has the boot ROM compiled in.
# Pass the encrypted app LDR; op1emu's emulated boot ROM decrypts it with otp.bin.
LDR="$REPO/../te-op1-cfw/op1 firmware/op1_246/OP1_vdk.ldr"
cat > boot.txt <<'SCRIPT'
accel 64 -64 512
wait-frames 1 300000
wait-frames 20 900000
wait 5000
screenshot boot.ppm
quit
SCRIPT
./op1emu/build/op1emu nand.bin "$LDR" --headless --nand-snapshot --input-script boot.txt || {
  echo "boot with the encrypted LDR + otp.bin failed; if otp.bin byte order is wrong, try:"
  echo "  python3 $REPO/tools/otp_txt_to_bin.py $OTP_TXT otp.bin --no-reverse"
  exit 1
}
echo "DONE: $WORK/boot.ppm written if the firmware reached a frame."
ls -la boot.ppm 2>/dev/null || echo "(no screenshot; check op1emu output above)"
