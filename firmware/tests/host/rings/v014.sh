#!/bin/bash
# rings 0.14 against 0.13 (the Makefile's `v014`): the build as it ships (plugin.json's flags) and its variants, on
# the host, rings_compare.cc's scenario (notes on any voice, steals, fades, restrikes, voices the "stock" stops
# rendering, knob jumps and walks, bends, quiet gaps) with several seeds, every model x polyphony on its own and the
# whole 90 s run; every 16-bit sample compared.
#   v014.sh BUILD OLD [BLOCKS]
# BUILD holds rings_v14 / rings_v13 (as they ship), rings_v14p / rings_v13p (the new with OP1RINGS_POOL_POISON: every
# string taken from the pool full of junk), rings_v14n / rings_v13n (the te-op1-cfw configuration, -DOP1_COOP only).
set -u
B=$1
BLOCKS=${3:-62010}
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
fail=0
cmpr() {   # name, new bin, old bin, env, args...
  local name=$1 n=$2 o=$3 e=$4
  shift 4
  env $e "$n" "$W/n_$name.raw" "$@" > "$W/n_$name.txt" 2> "$W/n_$name.err"
  env $e "$o" "$W/o_$name.raw" "$@" > /dev/null 2>&1
  if cmp -s "$W/n_$name.raw" "$W/o_$name.raw" && cmp -s "$W/n_$name.raw.mix" "$W/o_$name.raw.mix"; then
    echo "  $name ($e $*): the same bytes, $(cut -d' ' -f1-6 "$W/n_$name.txt"); $(grep pool "$W/n_$name.err")"
  else
    echo "  $name ($e $*): DIFFERENT"; fail=1
  fi
  rm -f "$W/n_$name.raw" "$W/o_$name.raw" "$W/n_$name.raw.mix" "$W/o_$name.raw.mix"
}
echo "the whole scenario ($BLOCKS blocks), seeds:"
for seed in 0x2545F491 1 2 3 4 5 6 7; do
  cmpr s$seed "$B/rings_v14" "$B/rings_v13" "RINGS_SEED=$seed" "$BLOCKS" &
  cmpr p$seed "$B/rings_v14p" "$B/rings_v13" "RINGS_SEED=$seed" "$BLOCKS" &
  wait
done
cmpr nolease "$B/rings_v14" "$B/rings_v13" "RINGS_NO_LEASE=1" "$BLOCKS"
cmpr te "$B/rings_v14n" "$B/rings_v13n" "RINGS_SEED=0x2545F491" "$BLOCKS"
echo "each model x polyphony setting on its own, 8,000 blocks, two seeds, poisoned pool:"
for m in 0 1 2 3 4 5; do
  for q in 0 1 2 3; do
    cmpr m${m}q${q}a "$B/rings_v14p" "$B/rings_v13" "RINGS_SEED=11" 8000 $m $q &
    cmpr m${m}q${q}b "$B/rings_v14" "$B/rings_v13" "RINGS_SEED=12" 8000 $m $q &
  done
  wait
done
echo "every struck voice rendered every block from its strike on (RINGS_LONG: no fade is ever left unrendered), the"
echo "sympathetic models, 8,000 blocks: nothing taken back from a fade; the same with a pool of 15 (rings_v14s) shows"
echo "the 16 are needed (a fade cut short, the output different):"
for m in 1 4; do
  for q in 0 1 2 3; do
    cmpr L${m}q${q} "$B/rings_v14p" "$B/rings_v13" "RINGS_LONG=1 RINGS_SEED=21" 8000 $m $q &
  done
  wait
done
for m in 1 4; do for q in 1 2 3; do
  env RINGS_LONG=1 RINGS_SEED=21 "$B/rings_v14s" "$W/x.raw" 8000 $m $q 2>&1 >/dev/null | sed "s/^/  pool of 15, model $m, polyphony setting $q: /" | grep pool
done; done
rm -f "$W/x.raw" "$W/x.raw.mix"
[ $fail = 0 ] && echo "v014: all the same" || { echo "v014: FAILED"; exit 1; }
