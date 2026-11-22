#!/bin/bash
# rings 0.14 against 0.13 on the long held-key scenarios (the Makefile's `v014-long`): rings_early.cc's, rings_sleep.cc's
# and rings_idle.cc's drivers, each built from both versions as they ship (the early cut on), every voice rendered
# every block, every render's 16-bit output (and rings_early's gains) compared byte for byte.
#   v014long.sh BUILD
# BUILD holds {early,sleep,idle}_v14 and _v13.
set -u
B=$1
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
P=${P:-8}
one() {   # kind tag args...: one comparison
  local kind=$1 tag=$2
  shift 2
  "$B/${kind}_v14" run "$W/$tag.n" "$@" > /dev/null 2>&1
  "$B/${kind}_v13" run "$W/$tag.o" "$@" > /dev/null 2>&1
  local r="the same"
  cmp -s "$W/$tag.n" "$W/$tag.o" || r=DIFFERENT
  [ -f "$W/$tag.n.gain" ] && { cmp -s "$W/$tag.n.gain" "$W/$tag.o.gain" || r="DIFFERENT (gains)"; }
  [ -s "$W/$tag.n" ] || r="EMPTY"
  echo "$kind $*: $r"
  rm -f "$W/$tag".*
}
export -f one
export B W
{
  # rings_early: the six models x four polyphony settings x three dampings, keys struck again every 4 s (all:4) and
  # left to die (none); two strike scripts into fades and a faded voice; the structure at 0 for FM (its tails stall).
  for m in 0 1 2 3 4 5; do for q in 0 1 2 3; do for d in 22937 28000 31000; do
    echo "early e$m$q${d}a $m $q $d 40 6 all:4"
    echo "early e$m$q${d}b $m $q $d 60 6 none"
    echo "early e$m$q${d}c $m $q $d 60 6 0@20,1@30.5,2@45 0"
  done; done; done
  # rings_sleep: 60 s, voice 0 struck again every 7 s, voice 1 at 40 s; and tails that stall, 150 s
  for m in 0 1 2 3 4 5; do for q in 0 1 2 3; do for d in 22937 28000; do
    echo "sleep s$m$q$d $m $q $d 60 6 7"
  done; done; done
  for c in "0 28000 8192 32767" "3 26000 0 19660" "5 28000 0 19660" "1 28000 0 32767"; do
    set -- $c; echo "sleep t$1$2$3 $1 3 $2 150 6 0 $3 $4"
  done
  # rings_idle: the reverb's idle state, damping 24,000 held 24 s and 29,000 held 100 s
  echo "idle i1 24000 24"
  echo "idle i2 29000 100"
} | xargs -P "$P" -L 1 bash -c 'one "$@"' _ > "$W/log"
sort "$W/log" | sed 's/^/  /'
n=$(wc -l < "$W/log"); bad=$(grep -vc ": the same$" "$W/log")
echo "v014-long: $n comparisons, $bad not the same"
[ "$bad" = 0 ]
