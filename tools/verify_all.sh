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

# WHAT A RECORDED RUN CANNOT SHOW. No recorded run ever assigns a skill or
# moves the pointer, so everything the player does is invisible to every
# comparison above. These two drive the ORIGINAL into doing it and compare
# against what it did.
say ""
say "  the pointer and the digger, against the original doing them:"
if [ -f out/hit.json ]; then
    uv run python tools/verify_hit.py --check out/hit.json 2>/dev/null | tail -4 || fail=1
else
    say "    no out/hit.json - run the LEM_HOOK line in tools/verify_hit.py"
fi
if [ -f out/digtrace.json ]; then
    uv run python tools/verify_dig.py --check out/digtrace.json 2>/dev/null | tail -6 || fail=1
else
    say "    no out/digtrace.json - run the LEM_HOOK line in tools/verify_dig.py"
fi

# PACE. Every check above compares pictures; none of them can see how long a
# frame lasts, and a port at four times speed matches all 5,100 of them.
say ""
say "  pace, which no picture comparison can see:"
if uv run python tools/verify_pace.py 2>/dev/null | tail -3; then
    :
else
    fail=1
fi


# THE RESULTS SCREEN, driven rather than played to. Image 0x046EB is a plain
# routine and the screen is a pure function of six bytes, so this costs ninety
# seconds where reaching it by playing costs fifteen minutes - and it covers
# BOTH endings, which a playthrough of one level cannot.
# EVERY SKILL'S RECORD, which is a different claim from any screen. See
# tools/verify_skills.py: it compares the lemming ARRAY, for every lemming
# rather than only the one that was clicked.
say ""
say "  the skills (tools/verify_skills.py):"
uv run python tools/verify_skills.py 2>/dev/null | sed -n 's/^    /    /p' || fail=1

say ""
say "  the results screen (tools/verify_results.py, driven):"
# Three cases, because they take three different paths through the screen and
# a playthrough of one level can only ever show one of them: won on time, won
# after the clock ran out, and lost with NOTHING saved - which is the else
# branch at image 0x04778, verdict rung 0, no access code and a retry prompt
# instead of a next-level one.
for case in "won 100 10 0" "timeup 30 10 1" "lost 0 10 1"; do
    set -- $case
    name=$1 saved=$2 needed=$3 timed=$4
    rm -rf "out/vr_$name"
    LEM_HOOK=tools/verify_results.py LEM_RESULTS_OUT="out/vr_$name" \
        LEM_RES_SAVED=$saved LEM_RES_NEEDED=$needed LEM_RES_LEVEL=1 \
        ${timed:+LEM_RES_TIMEUP=$timed} \
        uv run python tools/play.py --auto -- --run-seconds 90 \
        >"out/vr_$name.log" 2>&1 || true
    if [ -f "out/vr_$name/results.scrn" ]; then
        say "    $name: captured"
    else
        # KEEP THE LOG. This reported NOTHING CAPTURED for all three cases
        # once and the reason was gone with the output: both ran perfectly
        # when tried by hand a minute later, so the failure was the runs being
        # starved rather than anything about the port. A check that can only
        # say "it did not work" sends the next reader looking in the wrong
        # place - see the note in CLAUDE.md about truncated runs.
        say "    $name: NOTHING CAPTURED - that is a failure, not a pass"
        say "      (why: the tail of out/vr_$name.log)"
        tail -3 "out/vr_$name.log" 2>/dev/null | sed 's/^/      /'
        fail=1
    fi
done

say ""
if [ $fail = 0 ]; then
    say "  everything above ran. Read the numbers, not this line."
else
    say "  SOMETHING FAILED above. That is the result, not the numbers."
fi
exit $fail
