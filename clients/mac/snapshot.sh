#!/bin/sh
# Screenshot of the panel with sample to-dos (no network) for the README. Needs Screen
# Recording for the terminal. Launches through `open`, never the binary directly.
set -e
cd "$(dirname "$0")"
out=${1:-../../docs/images/menubar.png}
id=$(mktemp)
pkill -x SkyTile 2>/dev/null || true
sleep 1
open ~/Applications/SkyTile.app --args --demo --show --window-id "$id"
sleep 5
screencapture -x -l "$(cat "$id")" "$out"
rm -f "$id"
pkill -x SkyTile || true
open ~/Applications/SkyTile.app
echo "Wrote $out"
