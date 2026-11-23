#!/bin/bash
# One story leg: the bot's script from a state, headless, sound off (headless has none).
# Usage: leg.sh <state> <script> <frames> [extra grep pattern]
cd "$(dirname "$0")/../.."
ONI_BOT=$2 ONI_CHEAT_HEALTH=1 ONI_CHEAT_KILLS=1 ONI_CUT_TRACE=1 timeout 1200 build/bozo/oni-headless.exe --quiet --load-state $1 --frames $3 2>&1 \
  | grep -E "oni:bot|oni:cut|native failure|FAULT|not translated${4:+|$4}" | grep -v "aot  *[0-9]* at" | cut -c1-160
