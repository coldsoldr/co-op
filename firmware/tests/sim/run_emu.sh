#!/usr/bin/env bash
# The coop test runner (p5b, core/sdram/coop_runner.cc) in op1emu: one scenario played twice on the same image,
#   A: op1emu's own key input (the scenario as an input script, pristine NAND),
#   B: the runner (the scenario compiled to user/coop_autorun.crun on the NAND overlay, the image's
#      g_coop_run_autostart set so the main loop starts it where A's first event lands, an idle input script),
# then compares the two dumps on the onset (wav_compare --align) and prints B's coop_run.txt.
#
#   tests/sim/run_emu.sh SCENARIO IDLE_SCRIPT [TAG]     e.g. tests/sim/run_check.txt tests/sim/idle_60s.txt
#
# Needs make -C core/sdram combined, ~/Downloads/op1_246.op1 (or STOCK_OP1) and the op1emu work tree. Every build and
# emulator run takes /root/op1emu-work/emu.lock (gate runs must be sequential, docs/emulator.md).
set -euo pipefail
here=$(cd "$(dirname "$0")/../.." && pwd)
WORK=${OP1EMU_WORK:-$HOME/op1emu-work}
SCEN=${1:?usage: run_emu.sh SCENARIO IDLE_SCRIPT [TAG]}
IDLE=${2:?usage: run_emu.sh SCENARIO IDLE_SCRIPT [TAG]}
TAG=${3:-run}
STOCK_OP1=${STOCK_OP1:-/mnt/c/Users/byron/Downloads/op1_246.op1}
LOCK=$WORK/emu.lock
IMG="--tape-stereo --set-word g_coop_spread=32767 --set-word g_coop_force_stereo_out=1 ${RUN_EXTRA:-}"   # as the A/B gates (RUN_EXTRA: more --set-word pairs); the packages: --tape-stereo + spread

ov=$(mktemp -d); trap 'rm -rf "$ov"' EXIT
mkdir -p "$ov/user"
info=$(python3 -I "$here/tools/run_script.py" "$SCEN" -o "$ov/user/coop_autorun.crun")
echo "$info"
origin=$(echo "$info" | sed -n 's/.*block 0 = op1emu guest \([0-9]*\) ms.*/\1/p')
# g_blocks counts from the first audio block (~1.96 s guest, docs/emulator.md); the runner arms on the main-loop pass
# that sees the count and starts on the next block.
auto=$(( (origin - 1960) * 44100 / 64000 ))
echo "# block 0 at guest $origin ms: g_coop_run_autostart=$auto"

flock "$LOCK" "$here/tools/build_image.sh" "$WORK/${TAG}_img.ldr" $IMG --set-word g_coop_run_autostart=0 >/dev/null
flock "$LOCK" "$here/tools/build_image.sh" "$WORK/${TAG}_auto.ldr" $IMG --set-word g_coop_run_autostart=$auto >/dev/null
rm -f "$WORK/${TAG}_nand.bin"
flock "$LOCK" python3 -I "$here/tools/make_nand.py" --provision "$WORK/provision_nand.py" --firmware "$STOCK_OP1" \
    --mkyaffs2 "$WORK/yaffs2utils/mkyaffs2" --overlay "$ov" --output "$WORK/${TAG}_nand.bin" >/dev/null

flock "$LOCK" "$here/tests/sim/hear.sh" "$WORK/${TAG}_img.ldr" "$SCEN" "$WORK/${TAG}_a.wav" | tail -1
NAND_KEEP=$WORK/${TAG}_nand.bin flock "$LOCK" "$here/tests/sim/hear.sh" "$WORK/${TAG}_auto.ldr" "$IDLE" "$WORK/${TAG}_b.wav" | tail -1

echo "== A (op1emu input) vs B (runner), aligned on the onset"
python3 -I "$here/tools/wav_compare.py" "$WORK/${TAG}_a.wav" "$WORK/${TAG}_b.wav" --skip-seconds 5 --align 200 || true
python3 -I "$here/tools/wav_levels.py" "$WORK/${TAG}_a.wav" 2>/dev/null | tail -3 || true
python3 -I "$here/tools/wav_levels.py" "$WORK/${TAG}_b.wav" 2>/dev/null | tail -3 || true
echo "== B: coop_run.txt"
python3 -I "$here/tools/nand_file.py" "$WORK/${TAG}_nand.bin" coop_run.txt || true
grep -ciE "fault|exception" "$WORK/${TAG}_a.log" "$WORK/${TAG}_b.log" || true
