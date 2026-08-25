#!/usr/bin/env bash
# Reproduce the port's terrain proof, end to end.
#
# Drives the ORIGINAL into its first level under the emulator, stops it at the
# exact instruction its terrain composition finishes, dumps the level it built
# in video memory, renders the same level with the C port, and compares them as
# 4-bit indices over the whole 1584x160 canvas.
#
# The comparison is against what the original COMPOSES, not what it displays: a
# screen carries sprites, a cursor, a palette fade and a viewport, and none of
# those is the thing being checked.
#
#   tools/prove.sh [OUTDIR]
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$REPO/out}"
mkdir -p "$OUT"
cd "$REPO"

# Image offset 0x376A is the instruction after the terrain loop - the moment
# composition is complete and before anything else touches that memory. A dump
# taken by wall clock instead catches it mid-flight and looks exactly like a
# port that draws terrain the original does not.
# The capture is timing-dependent, so it is CHECKED rather than assumed. The
# harness arms a breakpoint and then sleeps; if the guest has not actually
# stopped there when the dump is taken, the dump is of whatever happened to be
# in video memory - and that reads as a port that is 83% right rather than as a
# failed capture. That happened, and this is the guard for it: `where` reports
# where the guest is stopped, and the run is only believed if it says 0x0376a.
capture() {
    uv run python tools/play.py --auto --start --dump-vga --dump-when 0x10 \
        --dump-delay 2 --click-tries 0 \
        --cmd "break i+0x376a" --cmd "sleep 25" \
        --cmd "click down 0 160 100" --cmd "sleep 1" --cmd "click up 0" \
        --cmd "sleep 25" --cmd "where" --cmd "planes $OUT/composed.bin" \
        --cmd "delete" --cmd "cont" \
        --start-delay 4 --start-tries 3 \
        -- --shots 1 --shot-every 900 --shot-dir "$OUT/shots" \
           --run-seconds 210 > "$OUT/prove.log" 2>&1 || true
    grep -q -E "planes .*composed.bin" "$OUT/prove.log" || return 1
    # The line after the `where` command is its answer.
    grep -A1 '^\[play\] > where$' "$OUT/prove.log" | grep -q "image 0x0376a"
}

echo "== driving the original to the end of its terrain composition =="
rm -f "$OUT/composed.bin"
if ! capture; then
    echo "   the guest was not stopped at image 0x0376a - capture discarded," >&2
    echo "   retrying once (this is timing-dependent, not a port failure)" >&2
    rm -f "$OUT/composed.bin"
    capture || {
        echo "!! could not stop the original at its terrain composition." >&2
        echo "!! That is a FAILED CAPTURE, not a comparison result." >&2
        echo "!! See $OUT/prove.log" >&2
        exit 1
    }
fi

echo "== rendering the same level with the port =="
make -C reconstruct >/dev/null
# --no-objects because the original composes terrain only: it draws the
# entrance and exit as animated sprites at display time, never baking them in.
reconstruct/lemmings --data lemming1.pc --no-objects \
    --out /dev/null --raw "$OUT/port.raw" | sed 's/^/   /'

echo "== comparing =="
uv run python tools/compare_composed.py "$OUT/composed.bin" "$OUT/port.raw"
