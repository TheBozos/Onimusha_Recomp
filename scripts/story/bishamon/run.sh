#!/bin/bash
# The optional branch: the Dark Realm and the Bishamon Sword. Leg 68 from the main story's state before leg 50 (the
# offer taken instead of declined), then the main legs 51-67 on the branch's own states (STORY_PREFIX=b_), a leg here
# taking the place of the main one where the branch differs (55: the Evil Plate one row further up), and leg 69 (the
# Bishamon Sword) after leg 65. Usage: run.sh [first leg] (68 by default; 51.. to resume after it).
cd "$(dirname "$0")/../../.."
export STORY_PREFIX=${STORY_PREFIX:-b_}
declare -A START=( [51]=s501k.state [52]=s208k.state [53]=s219k.state [54]=s220k.state [55]=s208m.state [56]=s400k.state [57]=s200k.state [58]=s303k.state [59]=s307k.state [60]=s300k.state [61]=s304k.state [62]=s103k.state [63]=s106k.state [64]=s313k.state [65]=s508q.state [66]=s402k.state [67]=s404k.state ) # the main legs' start states (each the one the leg before saves)
start_of() { echo ${START[$1]}; }
run() { out=$(scripts/story/leg.sh $1 $2 ${3:-40000} | grep "bot\] frame" | tail -1); echo "$(basename $2): $out"; }
first=${1:-68}
[ "$first" -eq 68 ] && run build/story/s510k.state scripts/story/bishamon/68_dark_realm.txt 150000
for n in $(seq 51 67); do
  [ "$first" -ne 68 ] && [ "$n" -lt "$first" ] && continue
  f=$(ls scripts/story/bishamon/${n}_*.txt 2>/dev/null || ls scripts/story/${n}_*.txt)
  s=$(start_of $n); s=build/story/${STORY_PREFIX}$(basename $s)
  [ "$n" -eq 66 ] && s=build/story/${STORY_PREFIX}s402s.state # (after leg 69: the Bishamon Sword)
  run $s $f $([ "$n" -eq 67 ] && echo 60000)
  if [ "$n" -eq 65 ] && ls scripts/story/bishamon/69_*.txt >/dev/null 2>&1; then
    run build/story/${STORY_PREFIX}s402k.state $(ls scripts/story/bishamon/69_*.txt)
  fi
done
