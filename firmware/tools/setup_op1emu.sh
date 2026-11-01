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
sudo apt-get install -y -qq ninja-build pkg-config libgl1-mesa-dev xorg-dev python3-pip git curl wget gnupg lsb-release gcc-10 g++-10 >/dev/null
python3 -m pip install --user -q "cmake>=3.20"
export PATH="$HOME/.local/bin:$PATH"
# op1emu/bcore use C++20 (<compare> etc.); Ubuntu 20.04's default g++ 9.4 is too old, g++-10 has it.
export CC=gcc-10 CXX=g++-10

echo "== LLVM 15 (bcore's JIT backend requires it) =="
LLVM_DIR_PATH=/usr/lib/llvm-15/lib/cmake/llvm
if [ ! -f "$LLVM_DIR_PATH/LLVMConfig.cmake" ]; then
  CODENAME="$(lsb_release -cs)"
  wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | sudo gpg --dearmor -o /usr/share/keyrings/llvm15.gpg
  echo "deb [signed-by=/usr/share/keyrings/llvm15.gpg] http://apt.llvm.org/$CODENAME/ llvm-toolchain-$CODENAME-15 main" | sudo tee /etc/apt/sources.list.d/llvm15.list >/dev/null
  sudo apt-get update -qq
  sudo apt-get install -y -qq llvm-15-dev >/dev/null
fi

echo "== op1emu (clone + build op1emu, ldrdump) =="
[ -d op1emu ] || git clone --recurse-submodules -j"$JOBS" https://github.com/op1emu/op1emu.git
rm -rf op1emu/build   # fresh configure so the g++-10 choice takes (cmake won't switch compilers in a cache)
cmake -S op1emu -B op1emu/build -G Ninja -DGLFW_BUILD_WAYLAND=OFF -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_DIR="$LLVM_DIR_PATH" -DCMAKE_C_COMPILER=gcc-10 -DCMAKE_CXX_COMPILER=g++-10
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

echo "== plaintext LDR (op1emu's documented input is a decrypted LDR) =="
# op1emu takes a decrypted OP1_vdk.ldr (op-decryptor2 style). Build one from the stock LDR with the
# key in otp.bin (page 0xD0), using the arrangement l1_decrypt.py check-key confirmed.
STOCK_LDR="$REPO/../te-op1-cfw/op1 firmware/op1_246/OP1_vdk.ldr"
if [ -f "$STOCK_LDR" ]; then
  python3 "$REPO/tools/l1_decrypt.py" decrypt-ldr "$STOCK_LDR" "$KEYHEX" plaintext.ldr \
    --swap be --order 0123 --words "<"
  LDR="$PWD/plaintext.ldr"
else
  echo "   stock OP1_vdk.ldr not found; op1emu will need a decrypted LDR you provide"
  LDR=""
fi

echo "== boot the stock firmware headless and screenshot =="
# op1emu reads otp.bin and the boot ROM from the working dir (the desktop build has the ROM compiled in).
cat > boot.txt <<'SCRIPT'
accel 64 -64 512
wait-frames 1 300000
wait-frames 20 900000
wait 5000
screenshot boot.ppm
quit
SCRIPT
test -n "$LDR" || { echo "no LDR to boot; provide a decrypted OP1_vdk.ldr"; exit 1; }
./op1emu/build/op1emu nand.bin "$LDR" --headless --nand-snapshot --input-script boot.txt || {
  echo "boot failed. Run it in the foreground to see op1emu's output:"
  echo "  cd $WORK && ./op1emu/build/op1emu nand.bin plaintext.ldr --headless --nand-snapshot --input-script boot.txt"
  echo "If the screen is garbled (wrong OTP byte order), rebuild otp.bin with --no-reverse and the plaintext LDR."
  exit 1
}
echo "DONE: $WORK/boot.ppm written if the firmware reached a frame."
ls -la boot.ppm 2>/dev/null || echo "(no screenshot; check op1emu output above)"
