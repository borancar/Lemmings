#!/usr/bin/env bash
# Everything this port claims, checked in one command.
#
# The claims are scattered across five tools and a recorded run, and a claim
# nobody can re-run is a claim that goes stale. This runs them all and prints
# one summary, so "the port matches" is something a reader can reproduce
# rather than something they have to take from a commit message.
#
#   bash tools/verify_all.sh            # the screens, the proof, 120 frames
#   bash tools/verify_all.sh --full     # ...and all 5,100 - the whole timer
#
# --full records the reference itself, which takes about twenty minutes of
# wall clock, and needs ~1.5 GB under out/.
set -u
cd "$(dirname "$0")/.."
FULL=0
[ "${1:-}" = "--full" ] && FULL=1
FRAMES=120
REF=out/ref_verify
[ $FULL = 1 ] && FRAMES=5100 && REF=out/ref5100

fail=0
say() { printf '%s\n' "$*"; }
line() { printf '  %-46s %s\n' "$1" "$2"; }

say ""
say "=== the port's own tests ==="
if make -C reconstruct test >/dev/null 2>&1; then
    line "make test" "ok"
else
    line "make test" "FAILED"; fail=1
fi

say ""
say "=== the composed level, against the original's own buffer ==="
out=$(bash tools/prove.sh 2>&1 | grep -E "offset .*%" | tail -1)
case "$out" in
    *100.00%*) line "tools/prove.sh" "100.00%" ;;
    *)         line "tools/prove.sh" "FAILED: ${out:-no result}"; fail=1 ;;
esac

say ""
say "=== the screens on the way in ==="
for s in menu code input briefing; do
    raw=$(timeout 900 uv run python tools/verify_menu.py --screen "$s" 2>&1)
    got=$(printf '%s' "$raw" | grep -E "whole screen" | tail -1 | sed 's/.*: *//')
    [ -n "$got" ] || { got="FAILED - no result"; fail=1; }
    line "$s" "$got"
    # The menu's banner scrolls, so its score depends on the phase the port is
    # rendered at. That phase is derived from ds:0x21 and checked against a
    # fit; if the two ever disagree the score is not the port's fault and the
    # summary has to say so rather than showing a number nobody can explain.
    printf '%s' "$raw" | grep -E "the banner:" | sed 's/^/      /'
    printf '%s' "$raw" | grep -E "palette entries differing : [1-9]" \
        | sed 's/^ */      /'
done

say ""
say "=== the level, frame by frame ==="
if [ $FULL = 1 ] || [ ! -d "$REF" ]; then
    uv run python tools/record_level.py --frames "$FRAMES" --out "$REF" \
        >/dev/null 2>&1
fi
n=$(ls "$REF"/*.scrn 2>/dev/null | wc -l)
if [ "$n" -lt 1 ]; then
    line "recording the original" "FAILED - no frames"; fail=1
else
    line "frames recorded from the original" "$n"
    rm -rf out/verify_port out/verify_panel
    mkdir -p out/verify_port out/verify_panel
    i=0
    while [ "$i" -lt "$n" ]; do
        f=$(printf 'f%05d' "$i")
        ./reconstruct/lemdev --data lemming1.pc --rating fun --number 1 \
            --obj-frame "$i" --raw "out/verify_port/$f.raw" \
            --panel "out/verify_panel/$f.raw" >/dev/null 2>&1
        i=$((i + 1))
    done
    uv run python tools/compare_run.py --ref "$REF" \
        --port-dir out/verify_port --panel-dir out/verify_panel --every 1 \
        2>/dev/null | tail -5

    # And THE GAME'S OWN SCREEN, which is a different claim: the above scores
    # a canvas and a panel composed separately by lemdev, and for a long time
    # that passed while ./lemmings showed an empty cave. This runs the window
    # path headlessly and scores the 320x200 it actually displays.
    say ""
    say "  the game's own screen (the window path, not the harness):"
    rm -rf out/verify_screen; mkdir -p out/verify_screen
    SDL_VIDEODRIVER=dummy LEM_FRAMES="$n" LEM_SCREEN_DIR=out/verify_screen \
        ./reconstruct/lemdev --data lemming1.pc --rating fun --number 1 \
        --show >/dev/null 2>&1
    uv run python tools/compare_screen.py --ref "$REF" \
        --dir out/verify_screen 2>/dev/null | tail -4
fi

say ""
if [ $fail = 0 ]; then
    say "  everything above ran. Read the numbers, not this line."
else
    say "  SOMETHING FAILED above. That is the result, not the numbers."
fi
exit $fail
