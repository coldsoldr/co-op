#!/bin/bash
# rings 0.14: the simulator against the host (the Makefile's `v014-sim`). v014sim.sh BUILD BFIN BLOCKS
set -u
B=$1; BF=$2; N=$3
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
one() {
  local tag=$1; shift
  case $tag in
    c*) "$B/rings_v14" "$W/$tag.h" "$@" > /dev/null 2>&1; "$BF/bfin-elf-run" "$B/rings_v14.elf" "$W/$tag.s" "$@" > /dev/null 2>&1 ;;
    e*) "$B/early_v14" run "$W/$tag.h" "$@" > /dev/null 2>&1; "$BF/bfin-elf-run" "$B/early_v14.elf" run "$W/$tag.s" "$@" > /dev/null 2>&1 ;;
  esac
  local r="the same"
  cmp -s "$W/$tag.h" "$W/$tag.s" || r=DIFFERENT
  [ -s "$W/$tag.h" ] || r=EMPTY
  echo "$tag $*: $r"
  rm -f "$W/$tag".*
}
export -f one
export B BF W
{
  for m in 0 1 2 3 4 5; do for q in 2 3; do echo "c$m$q $N $m $q"; done; done
  for m in 0 1 2 3 4 5; do echo "e${m}a $m 2 30000 20 6 all:4"; echo "e${m}b $m 3 26000 20 6 none 0"; done
} | xargs -P 12 -L 1 bash -c 'one "$@"' _ > "$W/log"
sort "$W/log" | sed 's/^/  /'
bad=$(grep -vc ": the same$" "$W/log")
echo "v014-sim: $(wc -l < "$W/log") comparisons, $bad not the same"
[ "$bad" = 0 ]
