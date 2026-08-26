#!/usr/bin/env bash
# A reference run for each of the eight skills, one at a time.
#
# "Just dig!" stocks only diggers, so seven of these can never happen by
# playing it. Both sides get the --skills poke - see record_level.py - and
# each run drives ONE assignment through the original and keeps its screens
# AND its lemming array, so a transcription of that skill has something to be
# checked against before it is written rather than after.
#
#   bash tools/record_skills.sh [frames]
#
# Sequential on purpose: two emulators on one machine halve each other's frame
# rate, and a run that falls behind its ceiling looks exactly like a level
# that ended early.
set -u
cd "$(dirname "$0")/.."
frames=${1:-400}
for b in 2 3 4 5 6 7 8 9; do
    out="out/ref_s$b"
    [ -f "$out/manifest.json" ] && { echo "  button $b: already recorded"; continue; }
    uv run python tools/make_skill_script.py --button "$b" --out "out/s$b.txt" \
        >/dev/null 2>&1 || { echo "  button $b: no script"; continue; }
    rm -rf "$out"
    uv run python tools/record_level.py --frames "$frames" --out "$out" \
        --script "out/s$b.txt" --seconds $((frames / 2 + 200)) \
        --skills 20 --lemmings > "out/s$b.log" 2>&1
    n=$(ls "$out"/*.scrn 2>/dev/null | wc -l)
    echo "  button $b: $n frames -> $out"
done
