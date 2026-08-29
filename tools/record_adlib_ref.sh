#!/usr/bin/env bash
# Record the AdLib reference pair verify_adlib.py needs, getting all six
# things right that the recipe in its header used to get wrong.
#
#   bash tools/record_adlib_ref.sh [seconds]
#
# Six traps, every one of which produces a NUMBER rather than an error, which
# is why they survived so long in a usage block nobody ran:
#
#   1. LEM_OPL_TUNE selects nothing - it appears in no code. The old recipe
#      used it and the two sides then recorded DIFFERENT TUNES: 0 of 54.
#   2. --start is what reaches the music. Without it the run parks on the
#      title, its timer ISR never runs, and the capture is six card-detect
#      writes. The duration is not the problem: 240 seconds without --start
#      scores the same six as 90.
#   3. LEM_OPL_REQ is not optional. The driver plays what it is ASKED for, so
#      a register stream without the request log is an answer with the
#      question missing.
#   4. The slot5 lines are the DRIVER's own write at +0x047F, not the game's.
#      Fed back they pre-set the flag start_music tests at +0x0457 and the
#      tune is refused - 106 writes instead of 12,263. They are stripped here.
#   5. The tick budget has to match the capture's span, or the two streams are
#      different lengths and verify_adlib refuses to score them. Computed
#      below from the capture itself rather than guessed.
#   6. The request log must begin before the music does. This cannot be forced
#      from here - probe_opl.py finds the driver through the INT 61h vector,
#      which exists only once adlib.dat has loaded - so the script REPORTS the
#      gap rather than pretending there is none.
set -u
cd "$(dirname "$0")/.."
SECS="${1:-240}"

rm -f out/orig_opl.txt out/orig_req.txt out/port_opl.txt

echo "  recording the original for ${SECS}s (--start, or there is no music)..."
LEM_HOOK=tools/probe_opl.py LEM_OPL_OUT=out/orig_opl.txt \
    LEM_OPL_REQ=out/orig_req.txt \
    uv run python tools/play.py --auto --start --click-tries 4 \
    -- --run-seconds "$SECS" > out/adlib_ref.log 2>&1

if [ ! -s out/orig_opl.txt ] || [ ! -s out/orig_req.txt ]; then
    echo "  NOTHING RECORDED. That is a failure, not a pass - see out/adlib_ref.log"
    exit 1
fi

# THE CHANNELS LINE IS THE TELL. A capture with none is the card detect.
grep -E "\[opl\] (register writes|channels|PIT ticks)" out/adlib_ref.log | sed 's/^/  /'
if grep -q "channels ever addressed *: none" out/adlib_ref.log; then
    echo "  the capture addressed NO channels - it is the card detect, not a"
    echo "  reference. Nothing played; check that --start reached a level."
    exit 1
fi

grep -v slot5 out/orig_req.txt > out/orig_req.play.txt

# The span the port must cover: the original's last tick, less the tick the
# script is rebased on. Computed, because a guess here silently truncates.
LAST=$(awk '!/^#/ {t=$1} END {print t}' out/orig_opl.txt)
FIRST=$(awk '!/^#/ {print $1; exit}' out/orig_req.play.txt)
TICKS=$(( LAST - FIRST + 8 ))
echo "  original spans to tick $LAST, script starts at $FIRST -> --adlib-ticks $TICKS"

./reconstruct/lemdev --data lemming1.pc --adlib-script out/orig_req.play.txt \
    --adlib-ticks "$TICKS" --raw out/port_opl.txt | sed 's/^/  /'

echo
uv run python tools/verify_adlib.py out/orig_opl.txt out/port_opl.txt \
    2>&1 | grep -vE "^pygame" | sed 's/^/  /'
