#!/usr/bin/env bash
# Launch op1emu's interactive GUI (window + host audio) on a coop-fw image, so a human can play it.
# Mouse-driven: click the rendered OP-1's buttons and keys. Runs on a fresh copy of the NAND each
# time so the pristine image is never mutated. Requires the op1emu work tree from tools/setup_op1emu.sh
# (GUI build at build/op1emu) and, on Windows, WSLg for the window + sound.
#
#   tests/sim/play.sh [LDR]      # LDR defaults to the milestone-B image ~/op1emu-work/mbF.ldr
#
# Sound on WSLg (found 2026-11-01): WSLg's PulseAudio server runs as uid 1000 and refuses clients
# with another uid, and this distro runs as root; on WSLg 1.0.79 the advertised PULSE_SERVER socket
# /mnt/wslg/PulseServer also does not exist, the live one is /mnt/wslg/runtime-dir/pulse/native.
# So when we are root and the socket belongs to someone else, op1emu runs as that uid (setpriv) with
# PULSE_SERVER pointed at the socket. That uid must be able to reach the work tree (chmod 711 on the
# home directory, done once below) and to write the run NAND and otp.bin (op1emu opens its OTP
# storage read-write). op1emu has no device option: miniaudio plays on the server's default sink.
set -e
WORK=${OP1EMU_WORK:-$HOME/op1emu-work}
LDR=${1:-$WORK/mbF.ldr}
[ -f "$LDR" ] || { echo "LDR not found: $LDR"; exit 1; }
[ -f "$WORK/pristine.bin" ] || { echo "pristine NAND not found: $WORK/pristine.bin"; exit 1; }
cp -f "$WORK/pristine.bin" "$WORK/play_nand.bin"
cd "$WORK/op1emu"                     # op1emu reads gui/ui.json + otp.bin from its cwd

SOCK=""
for s in "${PULSE_SERVER#unix:}" /mnt/wslg/PulseServer /mnt/wslg/runtime-dir/pulse/native; do
  [ -n "$s" ] && [ -S "$s" ] && { SOCK=$s; break; }
done
if [ -n "$SOCK" ]; then
  owner=$(stat -c %u "$SOCK")
  if [ "$(id -u)" = 0 ] && [ "$owner" != 0 ]; then
    echo "Sound: running as uid $owner (WSLg PulseAudio refuses root) via $SOCK"
    [ "$(stat -c %a "$HOME")" = 700 ] && chmod 711 "$HOME"   # let that uid traverse into the work tree
    chmod 666 "$WORK/play_nand.bin" otp.bin
    echo "Launching op1emu GUI on $(basename "$LDR") (fresh NAND). Close the window to quit."
    exec setpriv --reuid="$owner" --regid="$owner" --clear-groups \
      env HOME=/tmp PULSE_SERVER="unix:$SOCK" ./build/op1emu "$WORK/play_nand.bin" "$LDR"
  fi
  export PULSE_SERVER="unix:$SOCK"
else
  echo "Sound: no PulseAudio socket found (PULSE_SERVER=${PULSE_SERVER:-unset}); the window will be silent"
fi
echo "Launching op1emu GUI on $(basename "$LDR") (fresh NAND). Close the window to quit."
exec ./build/op1emu "$WORK/play_nand.bin" "$LDR"
