#!/bin/bash
# rings 0.13, the early cut: the Makefile's `early`, `early-table` and `early-sim` (rings_early.cc does the work).
#   early.sh BUILD proofs|table|sim [BFIN]
# BUILD holds rings_early (as it ships), rings_early60 (-60 dBFS), rings_early_twin (the rule off: 0.12) and
# rings_early_freeze (a faded voice's state kept); `sim` also their .elf. The runs' files go to a temporary folder.
set -u
B=$1
MODE=$2
BFIN=${3:-$HOME/op1-toolchain/bfin/bin}
W=$(mktemp -d)                         # the runs' files (up to 40 MB each): not on the Windows drive
trap 'rm -rf "$W"' EXIT
E=$B/rings_early; E60=$B/rings_early60; T=$B/rings_early_twin; Z=$B/rings_early_freeze
fail=0

# run BIN NAME ARGS...: the run's file W/NAME.raw (+ .gain), its text W/NAME.txt; a silent render with a non-zero sample fails.
run() {
  local bin=$1 name=$2
  shift 2
  if ! "$bin" run "$W/$name.raw" "$@" > "$W/$name.txt"; then echo "  FAILED: $name ($*)"; tail -1 "$W/$name.txt"; touch "$W/failed"; fi
}
same() { if ! "$E" "$@"; then fail=1; echo "  ^ FAILED: not the same"; fi; }
add() { awk "BEGIN { printf \"%.2f\", $1 }"; }
# the first voice in W/NAME.txt that fades and sleeps a full second later (not put to sleep on the way by 0.11's rule):
# "voice seconds" (the seconds from the run's start)
first_full_fade() {
  awk '/fade from/ { v = $2; sub(":", "", v); match($0, /fade from [0-9.]+/); t = substr($0, RSTART + 10, RLENGTH - 10);
       match($0, /asleep [0-9.]+/); a = substr($0, RSTART + 7, RLENGTH - 7);
       if (a - t > 0.95) { printf "%d %.2f\n", v, t + 3 * v / 689.0; exit } }' "$W/$1.txt"
}
fade_of() {   # voice V's
  awk -v v="$2" '/fade from/ && $2 == v ":" { match($0, /fade from [0-9.]+/); printf "%.2f\n", substr($0, RSTART + 10, RLENGTH - 10) + 3 * v / 689.0; exit }' "$W/$1.txt"
}

proofs() {
  echo "1. above the level: every key struck again every 4 s (no voice is ever 5 s under it), 40 s, the default patch;"
  echo "   as it ships and the -60 dBFS build against the twin without the rule, every sample of the six voices:"
  for m in 0 1 2 3 4 5; do for q in 0 1 2 3; do
    run "$T" b $m $q 22937 40 6 all:4 &
    run "$E" a $m $q 22937 40 6 all:4 &
    run "$E60" c $m $q 22937 40 6 all:4 &
    wait
    printf "  model $m, polyphony setting $q, -66:"; same cmp "$W/a.raw" "$W/b.raw"
    printf "  model $m, polyphony setting $q, -60:"; same cmp "$W/c.raw" "$W/b.raw"
  done; done
  echo "2. keys held and left to die (60 s): the twin's samples until the first voice fades; then the fades."
  echo "   (a fade's step: the largest difference between two neighbouring samples of a voice's 16-bit output, from the"
  echo "   fade's first render to the render after its last)"
  # (the FM voice at the default structure holds its note or dies fast: with the structure at 0 it stalls and fades)
  for ms in "0 8192" "1 8192" "2 8192" "3 8192" "3 0" "4 8192" "5 8192"; do for d in 22937 26000 28000 30000; do for q in 2 3; do
    set -- $ms; m=$1; st=$2
    run "$T" b $m $q $d 60 6 none $st &
    run "$E" a $m $q $d 60 6 none $st &
    run "$E60" c $m $q $d 60 6 none $st &
    wait
    printf "  model $m, polyphony setting $q, damping $d, structure $st:"; same until "$W/a.raw" "$W/b.raw"
    "$E" fade "$W/a.raw" "$W/b.raw" $m
    printf "   the -60 dBFS build:"; same until "$W/c.raw" "$W/b.raw"
    "$E60" fade "$W/c.raw" "$W/b.raw" $m
  done; done; done
  echo "3. a key struck again: 2 s before its voice would fade (the hold), half a second into the fade, 3 s after it"
  echo "   (asleep); against the twin without the rule, the same script. \"cleared\" is as it ships (a faded voice is"
  echo "   struck from rest), \"kept\" the build that keeps the faded voice's state (-DOP1RINGS_EARLY_FREEZE=1):"
  for md in "2 28000 8192" "2 30000 8192" "0 28000 8192" "3 26000 0" "1 28000 8192" "4 28000 8192" "5 28000 8192" "5 30000 8192"; do
    set -- $md; m=$1; d=$2; st=$3
    run "$E" n $m 3 $d 70 6 none $st
    set -- $(first_full_fade n)
    if [ $# -lt 2 ]; then echo "  model $m, damping $d: no voice fades all the way"; continue; fi
    v=$1; tf=$2
    echo "  model $m, damping $d, structure $st: voice $v fades from $tf s"
    t=$(add "$tf - 2")
    run "$E" a $m 3 $d 70 6 "$v@$t" $st & run "$T" b $m 3 $d 70 6 "$v@$t" $st & wait
    printf "   in the hold:"; "$E" strike "$W/a.raw" "$W/b.raw" $v $t; printf "   "; same until "$W/a.raw" "$W/b.raw"
    t=$(add "$tf + 0.5")
    run "$E" a $m 3 $d 70 6 "$v@$t" $st & run "$T" b $m 3 $d 70 6 "$v@$t" $st & wait
    printf "   in the fade:"; "$E" strike "$W/a.raw" "$W/b.raw" $v $t
    t=$(add "$tf + 3")
    run "$E" a $m 3 $d 70 6 "$v@$t" $st & run "$T" b $m 3 $d 70 6 "$v@$t" $st & run "$Z" c $m 3 $d 70 6 "$v@$t" $st & wait
    printf "   asleep, cleared:"; "$E" strike "$W/a.raw" "$W/b.raw" $v $t
    printf "   asleep, kept:   "; "$E" strike "$W/c.raw" "$W/b.raw" $v $t
    printf "   cleared against kept:"; "$E" strike "$W/a.raw" "$W/c.raw" $v $t
  done
  echo "4. a steal while a voice fades: polyphony 4, four keys held, a fifth key struck half a second into the oldest"
  echo "   voice's fade (voice 0: the new key, voice 4, takes its string over and voice 0 is faded out in 3 ms):"
  for md in "2 28000" "0 28000" "5 28000"; do
    set -- $md; m=$1; d=$2
    run "$E" n $m 2 $d 70 4 none
    tf=$(fade_of n 0)
    if [ -z "$tf" ]; then echo "  model $m, damping $d: voice 0 does not fade"; continue; fi
    t=$(add "$tf + 0.5")
    echo "  model $m, damping $d: voice 0 fades from $tf s, voice 4 struck at $t s"
    run "$E" a $m 2 $d 70 4 "4@$t" & run "$T" b $m 2 $d 70 4 "4@$t" & wait
    printf "   the new voice:"; "$E" strike "$W/a.raw" "$W/b.raw" 4 $t
    printf "   the stolen one:"; "$E" strike "$W/a.raw" "$W/b.raw" 0 $t
    grep "silent renders:" "$W/a.txt"
  done
  echo "5. the level comes back with no strike: FM, structure 0, damping 26000, six keys; half a second into the fades"
  echo "   the damping goes to the top (the FM voice's envelope then rises again by itself):"
  run "$E" n 3 3 26000 30 6 none 0
  set -- $(first_full_fade n)
  t=$(add "$2 + 0.6")
  run "$E" a 3 3 26000 30 6 "d@$t:32767" 0 & run "$T" b 3 3 26000 30 6 "d@$t:32767" 0 & wait
  echo "  the damping moved at $t s: $(grep -c 'turned back' "$W/a.txt") voices' fades turned back, the last at full gain at $(grep 'turned back' "$W/a.txt" | tail -1 | sed 's/.*again at //')"
  "$E" fade "$W/a.raw" "$W/b.raw" 5
  if [ $fail -ne 0 ] || [ -e "$W/failed" ]; then echo "early: FAILED"; exit 1; fi
  echo "early: every run's silent renders were 64 zeros; the comparisons marked \"the same\" hold"
}

# One line a run: when each voice fades and sleeps, with the summed output's level then.
summary() {
  awk '
    /voice [0-9]: / && !/asleep 0\.0[0-9] s/ {
      v = $2; sub(":", "", v); s = " v" v
      if (match($0, /fade from [0-9.]+ s/)) { f = substr($0, RSTART + 10, RLENGTH - 12); match($0, /LSB, -?[0-9.]+ dBFS; its/); s = s " fade " f " s (" substr($0, RSTART + 5, RLENGTH - 14) ")" }
      match($0, /asleep [0-9.]+ s/); a = substr($0, RSTART + 7, RLENGTH - 9)
      n = split($0, parts, "LSB, "); lv = parts[n]; sub(/ dBFS.*/, "", lv)
      line = line s " asleep " a " s (" lv ");"
    }
    /every voice asleep|still awake/ { t = $0; sub(/^ +/, "", t); line = line " | " t }
    END { print line }'
}

table() {
  echo "when each voice fades and sleeps (seconds after its strike), with the summed 16-bit output's peak in the quarter"
  echo "second before (dBFS); the default patch but for the damping; 480 s held"
  for m in 5 2; do for n in 6 1; do for d in 22937 28000 30000 31000 32767; do
    for bn in "$T 0.12" "$E -66" "$E60 -60"; do
      set -- $bn
      printf "model $m, $n keys, damping $d, $2:"; "$1" run - $m 3 $d 480 $n none | summary
    done
  done; done; done
  echo "the other models, six keys, 150 s:"
  for m in 0 1 3 4; do for d in 22937 28000 30000 32767; do
    for bn in "$T 0.12" "$E -66" "$E60 -60"; do
      set -- $bn
      printf "model $m, 6 keys, damping $d, $2:"; "$1" run - $m 3 $d 150 6 none | summary
    done
  done; done
}

# The simulator. First against the host with the assembly kernels (fades, a strike into a fade and one into a faded
# voice, in the reverb, string, FM and modal models), then the cost curves: cycles a block per 5 s. Thirty runs side
# by side, the longest 180 s of sound: about half an hour.
sim() {
  local R=$BFIN/bfin-elf-run
  echo "simulator against host, the 16-bit output of every render:"
  for mds in "5 28000 40 5@15.7,3@27 8192" "2 28000 22 0@16.2 8192" "3 26000 22 1@14.5,2@17 0" "0 22937 24 3@15.2,4@18 8192"; do
    set -- $mds
    ( "$R" "$E.elf" run "$W/s$1.raw" $1 3 $2 $3 6 $4 $5 > "$W/s$1.txt"; "$E" run "$W/h$1.raw" $1 3 $2 $3 6 $4 $5 > "$W/h$1.txt" ) &
  done
  wait
  for m in 5 2 3 0; do
    printf "  model $m:"; if cmp -s "$W/s$m.raw" "$W/h$m.raw" && cmp -s "$W/s$m.raw.gain" "$W/h$m.raw.gain"; then
      echo " the same file ($(stat -c %s "$W/s$m.raw") bytes), the same gains; $(grep -c 'fade from' "$W/s$m.txt") voices faded"
    else echo " DIFFERENT"; fail=1; fi
  done
  echo "cycles a block (compute only), per 5 s:"
  for mnd in "5 6 22937 25" "5 6 28000 45" "5 6 30000 100" "5 6 31000 180" "5 6 32767 130" "2 6 28000 25" "2 6 30000 30" \
             "2 6 32767 130" "5 1 30000 115" "0 6 28000 70"; do
    set -- $mnd
    for bn in "_twin 0.12" " -66" "60 -60"; do
      set -- $mnd $bn
      [ $# -eq 5 ] && set -- $1 $2 $3 $4 "" $5
      "$R" "$B/rings_early$5.elf" run - $1 3 $3 $4 $2 none > "$W/cost_m$1_keys$2_damping$3_$6.txt" &
    done
  done
  wait
  for f in "$W"/cost_*.txt; do
    echo "== ${f##*/cost_}"; grep -E " s: |every|still" "$f" | sed 's/summed peak//; s/, [0-9] silent renders//'
  done
  if [ $fail -ne 0 ]; then echo "early-sim: FAILED"; exit 1; fi
}

case $MODE in
  proofs) proofs ;;
  table) table ;;
  sim) sim ;;
  *) echo "usage: $0 BUILD proofs|table|sim [BFIN]"; exit 2 ;;
esac
