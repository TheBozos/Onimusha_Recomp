#!/usr/bin/env bash
# The headless runner: scripts/run.sh [--frames N] [...]; ONI_WINDOW_RUN=1 runs the window build instead.
source "$(dirname "$0")/common.sh"
cd "$ROOT"
BIN="$GAME_BUILD/oni-headless$EXE"
[ -n "${ONI_WINDOW_RUN:-}" ] && BIN="$GAME_BUILD/onimusha$EXE"
[ -x "$BIN" ] || { echo "missing $BIN; run scripts/build_runtime.sh first"; exit 1; }
exec "$BIN" "$@"
