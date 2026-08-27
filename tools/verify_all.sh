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
        LEM_RES_TIMEUP=$timed \
        uv run python tools/play.py --auto -- --run-seconds 90 \
        >"out/vr_$name.log" 2>&1 || true
    if [ -f "out/vr_$name/results.scrn" ]; then
        say "    $name: captured"
    else
        # KEEP THE LOG, and this is the second time it earned its keep. The
        # three cases reported NOTHING CAPTURED again, and the log's last line
        # was `LEM_RES_TIMEUP=0: command not found` - the assignment was
        # written `${timed:+LEM_RES_TIMEUP=$timed}`, and a shell decides what
        # is an assignment BEFORE it expands anything, so what came out was a
        # command name. Without the log this reads as three broken screens.
        #
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

# THE THREE FURTHER LEVELS, which until now were only ever checked by hand.
#
# Each carries what a comparison of it needs and cannot work out for itself:
# the reference's ds:0x1F6B index is a permutation away from the file and
# section lemdev takes, and half the plates play their level under ODDTABLE's
# numbers instead of the file's. The park script aims the port's cursor at the
# guest's own; two cross-hairs in different places is 56 differing pixels on
# every frame and compare_screen.py excuses 28, which is one.
# THE COMPOSED LEVEL WITH ITS OBJECTS, which prove.sh above cannot see: it
# breaks at image 0x0376A, and the objects are drawn later by image 0x03981
# in a second composition path. Nothing compared the bitmap afterwards, and
# that gap let "the original never composes objects into the level bitmap"
# stand as a claim - true at prove.sh's break, false a few routines on.
# THE RELEASE RATE, driven through the original's own handler. Nothing else
# here presses those buttons: no recorded run contains one, which is why they
# did nothing at all for as long as they did.
# THE SPRITE REGIONS, laid out from game.c's own constants. Sixteen of them
# tile MAIN.DAT's first section exactly, so a wrong base or frame count shows
# up as a gap or an overlap. It found the shrug facing one way before any
# player did.
# WHAT THE NUMBERS ABOVE COVER. Every level compared here scores exact, and
# that is narrower than it sounds: five of six real bugs found in one session
# were in paths no reference touches. This is informational - a feature nobody
# measures is not a bug - but it is the list of places one could sit unseen.
say ""
say "=== what these comparisons exercise ==="
uv run python tools/coverage.py 2>&1 | tail -6 | sed 's/^/  /'

say ""
say "=== the sprite regions tile the file ==="
if uv run python tools/tile_sprites.py 2>&1 | tail -2 | sed 's/^/  /'; then :; else fail=1; fi

say ""
say "=== the release rate, against image 0x04032 ==="
if uv run python tools/verify_rate.py 2>&1 | grep -vE "^pygame|^ *\[vga\]" | sed 's/^/  /'; then :; else fail=1; fi

say ""
say "=== the composed level WITH objects (the gap prove.sh leaves) ==="
if uv run python tools/verify_objects.py 2>&1 | sed 's/^/  /'; then :; else fail=1; fi

say ""
say "=== the TRIGGER MAP, which no picture can show ==="
# The map at ss:0x12 is the only thing that answers "what is under this
# lemming", and the port built half of it: the steel pass at image 0x03839 was
# missing entirely, so a bomb went through a steel floor. A frame comparison
# cannot reach this and neither can a screenshot.
#
# level004/3 because it has all three kinds of cell in it - twelve one-way
# walls, five steel areas and an exit. It drives the emulator to a level, so
# it is minutes rather than seconds.
if uv run python tools/verify_triggers.py --level 77 2>&1 | sed 's/^/  /'; then :; else fail=1; fi

say ""
say "=== three further levels, frame by frame ==="
# level004.dat 3 is here because tools/coverage.py said the other three carry
# no one-way wall between them: object ids 3 and 4 and the only_on_terrain
# flag were measured by NOTHING. This one has twelve, and it found a
# difference on its first run.
#
# level006.dat 3 was chosen the same way, for the LAST object id nothing
# measured - id 4, the mirrored one-way wall, 21 of them in the game - and it
# found a difference on its first run too, in TERRAIN rather than objects: a
# piece carrying both the erase and the no-overwrite flag, which the original
# draws and the port was erasing. 116 such pieces across 9 of the 80 levels.
# Two levels chosen by a coverage gap, two bugs; that is the argument for
# choosing them that way rather than by which level looks interesting.
#
# level007.dat 1 closes the last of them - the two TERRAIN flag combinations
# nothing exercised, upside_down + erase and all three together. It was exact
# on its first run, which is the other outcome a coverage gap can have and
# worth having either way: it turns "we have never looked" into "we looked".
for case in "level001.dat 3 ref_i8 530 10" \
            "level002.dat 4 ref_i61 1100 -1" \
            "level002.dat 6 ref_i63 1100 -1" \
            "level004.dat 3 ref_i77 1100 -1" \
            "level006.dat 3 ref_i95 1100 -1" \
            "level007.dat 1 ref_i101 1100 -1"; do
    set -- $case
    lfile=$1 lsec=$2 lref=$3 lframes=$4 lodd=$5
    if [ ! -f "out/$lref/manifest.json" ]; then
        line "$lfile level $lsec" "NO REFERENCE at out/$lref - not a pass"
        fail=1
        continue
    fi
    rm -rf out/va_shot; mkdir -p out/va_shot
    oddarg=""
    [ "$lodd" != "-1" ] && oddarg="--odd $lodd"
    # AIM THE PORT'S POINTER AT THE ORIGINAL'S OWN, frame by frame. A constant
    # park was right for level002's two sections, whose guest settles on frame
    # 1, and wrong for level001 section 3, whose guest does not move until
    # frame 42 - 42 frames of exactly 56 differing pixels, all of them the
    # cursor. Generated per reference, so it cannot go stale against one.
    uv run python tools/park_script.py "out/$lref" "out/park_$lref.txt" \
        >/dev/null 2>&1
    SDL_VIDEODRIVER=dummy LEM_FRAMES=$lframes LEM_SCREEN_DIR="$PWD/out/va_shot" \
        LEM_INPUT_SCRIPT="$PWD/out/park_$lref.txt" \
        ./reconstruct/lemdev --data lemming1.pc --file "$lfile" --level "$lsec" \
        $oddarg --show >/dev/null 2>&1 || true
    got=$(uv run python tools/compare_screen.py --ref "out/$lref" \
          --dir out/va_shot 2>/dev/null | grep -E "play area|panel" \
          | sed 's/^ *//' | tr '\n' ' ')
    if [ -z "$got" ]; then
        line "$lfile level $lsec" "NOTHING SCORED - that is a failure"
        fail=1
    else
        say "  $lfile level $lsec:"
        say "    $got"
    fi
done

say ""
if [ $fail = 0 ]; then
    say "  everything above ran. Read the numbers, not this line."
else
    say "  SOMETHING FAILED above. That is the result, not the numbers."
fi
exit $fail
