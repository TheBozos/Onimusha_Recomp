#!/bin/bash
# Oni Spirits through its twelve holy grounds, played by the bot (scripts/story/oni_spirits/*.txt): the GAME CLEAR DATA
# saved from the branch's end state (build/story/b_s_end.state, from scripts/story/bishamon/run.sh) to a fresh test
# card, a boot with that card into Special Feature's Oni Spirits, then the grounds one at a time from their own save
# states (build/story/os_gNN.state, saved at each ground's start; os_end.state at the final RESULT). One-hit kills;
# infinite health until each ground's quota (3_grounds.txt). A ground lost (its quota not met: the enemies took too
# many pots) is played again from its state a few frames later, up to four tries.
# Usage: run.sh [N]: from ground N (its state from an earlier run). Headless, no sound; about 8 minutes.
cd "$(dirname "$0")/../../.."
cards=build/story/os_cards/$(date +%Y%m%d_%H%M%S)
mkdir -p "$cards"
startOf() { timeout 300 build/bozo/oni-headless.exe --quiet --load-state $1 --frames 1 2>&1 | grep -o "loaded.*(frame [0-9]*" | grep -o "[0-9]*$"; }
bot() { # <state or -> <script> <frames past the state>
  local load="" start=0
  if [ "$1" != "-" ]; then load="--load-state $1"; start=$(startOf $1); fi
  ONI_BOT=$2 ONI_CHEAT_HEALTH=1 ONI_CHEAT_KILLS=1 ONI_CUT_TRACE=1 timeout 3600 build/bozo/oni-headless.exe --quiet \
    --card-dir "$cards" $load --frames $(( ${start:-0} + $3 )) 2>&1 | grep -E "oni:bot\] frame|native failure|FAULT"
}
ONI_BOT=scripts/story/oni_spirits/1_clear_data.txt timeout 600 build/bozo/oni-headless.exe --quiet --card-dir "$cards" \
  --load-state build/story/b_s_end.state --frames 535500 > /dev/null 2>&1
ls "$cards"/mc0/BASLUS-20018/BASLUS-20018 > /dev/null || { echo "no clear data on the card"; exit 1; }
first=${1:-1}
if [ "$first" = 1 ]; then
  bot - scripts/story/oni_spirits/2_boot.txt 6000 | grep -E "save|failure|FAULT"
fi
for g in $(seq $first 12); do
  n=$(printf "%02d" $g)
  # This ground's lines of 3_grounds.txt (from its pots step to the next one).
  awk -v g=$g '/^pots/{k++} k==g' scripts/story/oni_spirits/3_grounds.txt > build/story/.os_ground.txt
  for w in 0 7 13 29; do
    (printf "wait %d\n" $w; cat build/story/.os_ground.txt) > build/story/.os_try.txt
    out=$(bot build/story/os_g$n.state build/story/.os_try.txt 12000)
    met=$(echo "$out" | grep -o "ground $g: [0-9]* released, the quota [0-9]* met")
    if [ -n "$met" ]; then echo "$met"; break; fi
    echo "ground $g lost (start + $w frames): $(echo "$out" | grep -E "no pot left|STOPPED|failure|FAULT" | head -1)"
  done
  [ -z "$met" ] && { echo "ground $g: not won in four tries"; exit 1; }
done
echo "$out" | grep -E "os_end|end$" | tail -1
