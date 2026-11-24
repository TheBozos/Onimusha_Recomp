#!/bin/bash
# One story leg: the bot's script from a state, headless (no sound), the cheats on (health, one-hit kills, souls, magic).
# Usage: leg.sh <state> <script> <frames> [extra grep pattern]: <frames> past the state's own frame; the run ends there or
# at the script's last step.
cd "$(dirname "$0")/../.."
start=$(timeout 300 build/bozo/oni-headless.exe --quiet --load-state $1 --frames 1 2>&1 | grep -o "loaded.*(frame [0-9]*" | grep -o "[0-9]*$")
printf "%s\nwait 5\nend\n" "$(cat $2)" > build/story/.leg_$$.txt
ONI_BOT=build/story/.leg_$$.txt ONI_CHEAT_HEALTH=1 ONI_CHEAT_KILLS=1 ONI_CHEAT_SOULS=1 ONI_CHEAT_MAGIC=1 ONI_CUT_TRACE=1 timeout 1800 \
  build/bozo/oni-headless.exe --quiet --load-state $1 --frames $(( ${start:-0} + $3 )) 2>&1 \
  | grep -E "oni:bot|oni:cut|native failure|FAULT|not translated${4:+|$4}" | grep -v "aot  *[0-9]* at" | cut -c1-160
rm -f build/story/.leg_$$.txt
