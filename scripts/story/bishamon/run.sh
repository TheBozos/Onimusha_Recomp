#!/bin/bash
# The optional branch: the whole story from leg 00 with everything optional, on states of its own (STORY_PREFIX, b_ by
# default: leg.sh renames a script's saves). A leg in this folder takes the place of the main one where the branch
# differs (the Fluorites' pickups, the Great Armor, the item menu shifted by the Bishamon Ocarina); leg 68 (the Dark
# Realm) takes the place of leg 50, leg 69 (the Bishamon Sword) comes after leg 65.
# Usage: run.sh [first leg] [last leg] (0 and 67 by default; 68 or 69 resume at those).
cd "$(dirname "$0")/../../.."
export STORY_PREFIX=${STORY_PREFIX:-b_}
declare -A START=( [0]=r6.state [1]=s113.state [2]=s114.state [3]=s115.state [4]=s116.state [5]=s116b.state [6]=s005.state [7]=s100.state [8]=s200.state [9]=s125.state [10]=s126.state [11]=s126b.state [12]=s124.state [13]=s230.state [14]=s200b.state [15]=s500.state [16]=s505.state [17]=s506.state [18]=s509b.state [19]=s510.state [20]=s501b.state [21]=s515.state [22]=s515b.state [23]=s518.state [24]=s519.state [25]=s520.state [26]=s520b.state [27]=s521.state [28]=s521b.state [29]=s522.state [30]=s524.state [31]=s524c.state [32]=s525.state [33]=s530.state [34]=s530c.state [35]=s508c.state [36]=s503.state [37]=s224.state [38]=s225d.state [39]=s224d.state [40]=s233c.state [41]=s508k.state [42]=s218k.state [43]=s218m.state [44]=s222k.state [45]=s222m.state [46]=s238k.state [47]=s239k.state [48]=s244k.state [49]=s242k.state [50]=s510k.state [51]=s501k.state [52]=s208k.state [53]=s219k.state [54]=s220k.state [55]=s208m.state [56]=s400k.state [57]=s200k.state [58]=s303k.state [59]=s307k.state [60]=s300k.state [61]=s304k.state [62]=s103k.state [63]=s106k.state [64]=s313k.state [65]=s508q.state [66]=s402k.state [67]=s404k.state ) # the main legs' start states (each the one the leg before saves)
run() { out=$(scripts/story/leg.sh $1 $2 ${3:-40000} | grep "bot\] frame" | tail -1); echo "$(basename $2): $out"; case "$out" in *": end") ;; *) exit 1 ;; esac; }
order="$(seq 0 49) 68 $(seq 51 65) 69 66 67"
first=${1:-0}; last=${2:-67}; on=0
for n in $order; do
  [ "$n" -eq "$first" ] && on=1
  [ $on -eq 1 ] || continue
  case $n in
    0) s=build/r6.state ;;                          # (the first leg: the main run's own start)
    68) s=build/story/${STORY_PREFIX}s510k.state ;;
    69) s=build/story/${STORY_PREFIX}s402k.state ;;
    66) s=build/story/${STORY_PREFIX}s402s.state ;;      # (after leg 69: the Bishamon Sword)
    *) s=build/story/${STORY_PREFIX}${START[$n]} ;;
  esac
  f=$(ls scripts/story/bishamon/$(printf %02d $n)_*.txt 2>/dev/null || ls scripts/story/$(printf %02d $n)_*.txt)
  frames=40000; [ "$n" -eq 68 ] && frames=160000; [ "$n" -eq 67 ] && frames=60000
  run $s $f $frames
  [ "$n" -eq "$last" ] && break
done
