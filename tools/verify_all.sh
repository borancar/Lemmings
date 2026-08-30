#!/usr/bin/env bash
# Everything this port claims, checked in one command.
#
# The claims are scattered across five tools and a recorded run, and a claim
# nobody can re-run is a claim that goes stale. This runs them all and prints
# one summary, so "the port matches" is something a reader can reproduce
# rather than something they have to take from a commit message.
#
#   bash tools/verify_all.sh            # the screens, the proof, 120 frames
#   bash tools/verify_all.sh --units    # ...and the eight per-routine checks
#   bash tools/verify_all.sh --full     # ...and all 5,100 - the whole timer
#
# --full records the reference itself, which takes about twenty minutes of
# wall clock, and needs ~1.5 GB under out/.
#
# --units adds the checks that drive ONE routine of the original rather than a
# screen: the decompressor, the LVL header, the digit printer, the access
# code, the two sprite pre-passes, the data load and the AdLib driver. They
# were outside this script entirely, and STATUS.md's headline table quotes
# five numbers that only they produce - 179,618 bytes across 7 sections, the
# decompressor's 7 calls and 150,807 bytes, the LVL header's 15 fields, the
# pre-shift's 200 calls, the pre-builder's 106 - under a heading promising
# they were re-measured on the current build. Each drives an emulator, so
# they are a flag rather than the default; a claim nobody re-runs is how
# verify_dig's note came to say its own strongest half was broken, and how
# verify_adlib's usage line kept naming an env var nothing reads.
set -u
cd "$(dirname "$0")/.."
FULL=0
UNITS=0
for a in "$@"; do
    case "$a" in
        --full)  FULL=1; UNITS=1 ;;   # --full implies --units
        --units) UNITS=1 ;;
    esac
done
FRAMES=120
REF=out/ref_verify
[ $FULL = 1 ] && FRAMES=5100 && REF=out/ref5100

fail=0

# A PIPELINE'S STATUS IS THE LAST COMMAND'S, WHICH HERE IS ALWAYS `sed`.
#
# Four checks were written `if python ... | sed 's/^/  /'; then :; else
# fail=1; fi`, and that tests SED - which succeeds whatever the python did.
# tools/verify_triggers.py died on an AttributeError for as long as it took
# anyone to look, because LEVEL_ORDER had moved from game.c to data.c and it
# still named game.c, and this script printed the traceback and exited 0
# around it. A check that crashed looks exactly like a check with nothing to
# report; that is this repository's oldest lesson and the shell had a way to
# hide it that nobody had checked.
check_pipe () {
    local rc=${PIPESTATUS[0]}
    if [ "$rc" -ne 0 ]; then
        line "$1" "FAILED (exit $rc)"
        fail=1
    fi
}
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
# RE-RECORD WHEN THE REFERENCE LACKS WHAT THE COMPARISON NEEDS, not merely
# when the directory is absent. out/ref_verify was cached from a
# record_level.py that predated the pointer fields, so its manifest carried
# frame, game_frame, lit_dac and crtc_start and nothing else - and
# park_script.py could not aim the port's cross-hair at the guest's. That
# scored 39 of 119 with a residue that was entirely two cursors, and it had
# been doing so silently for as long as the directory had existed.
need_rec=0
[ $FULL = 1 ] && need_rec=1
[ -d "$REF" ] || need_rec=1
if [ -f "$REF/manifest.json" ] && ! grep -q '"mx"' "$REF/manifest.json"; then
    need_rec=1
fi
if [ $need_rec = 1 ]; then
    rm -rf "$REF"
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
    # AIM THE PORT'S POINTER AT THE ORIGINAL'S OWN, as the seven levels below
    # already do. Without a script the port's cross-hair sits where it
    # defaults while the guest's moves, which is 56 differing pixels - two
    # cross-hairs - on every frame after the guest first moves. It scored 39
    # of 119 that way and the residue was entirely cursor.
    uv run python tools/park_script.py "$REF" out/park_verify.txt \
        >/dev/null 2>&1
    SDL_VIDEODRIVER=dummy LEM_FRAMES="$n" LEM_SCREEN_DIR=out/verify_screen \
        LEM_INPUT_SCRIPT="$PWD/out/park_verify.txt" \
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
say "=== the usage blocks name flags that exist ==="
# Five recipes here specified something that cannot work, each in the header
# of a check the port rests on, and each failing as something OTHER than
# itself - a percentage of zero, a socket error, a usage message. This is
# cheap (no emulator) so it runs by default, and it carries its own control:
# --self-test plants a bad recipe and confirms it is caught.
uv run python tools/check_recipes.py 2>&1 | grep -vE "^pygame" | sed "s/^/  /"
check_pipe "tools/check_recipes.py"

say ""
say "=== the sprite regions tile the file ==="
uv run python tools/tile_sprites.py 2>&1 | tail -2 | sed 's/^/  /'
check_pipe "tools/tile_sprites.py"

say ""
say "=== the release rate, against image 0x04032 ==="
uv run python tools/verify_rate.py 2>&1 | grep -vE "^pygame|^ *\[vga\]" | sed 's/^/  /'
check_pipe "tools/verify_rate.py"

say ""
say "=== the composed level WITH objects (the gap prove.sh leaves) ==="
uv run python tools/verify_objects.py 2>&1 | sed 's/^/  /'
check_pipe "tools/verify_objects.py"

say ""
say "=== the TRIGGER MAP, which no picture can show ==="
# The map at ss:0x12 is the only thing that answers "what is under this
# lemming", and the port built half of it: the steel pass at image 0x03839 was
# missing entirely, so a bomb went through a steel floor. A frame comparison
# cannot reach this and neither can a screenshot.
#
# level004/3 because it has all three kinds of cell in it - twelve one-way
# walls, five steel areas and an exit, and because it is the level that found
# the effect-0 fault. It drives the emulator to a level, so it is minutes
# rather than seconds, which is why only one is run here.
#
# The other one worth running BY HAND is --level 63, level002/6, the trap
# level: it is what exercises the INDEX nibble, since level004/3 has only two
# cells that are neither steel nor one-way wall. Both scored 16,040 of 16,040
# on the whole byte when this was written.
uv run python tools/verify_triggers.py --level 77 2>&1 | sed 's/^/  /'
check_pipe "tools/verify_triggers.py --level 77"

if [ $UNITS = 1 ]; then
    say ""
    say "=== the per-routine checks (--units) ==="
    # Each of these drives ONE routine of the ORIGINAL and compares its own
    # output with the port's on the same call - the differential shape
    # tools/verify_dat.py documents. They are the only source of five figures
    # in STATUS.md's headline table, and until now nothing re-ran them.
    #
    # verify_adlib.py needs a reference pair it does not record itself, so it
    # is listed here with its inputs rather than run blind: a run that scores
    # 0 of 54 is the two sides being different tunes, not a broken driver.
    # Checked with both controls before this went in, by stubbing `uv`:
    # every tool succeeding gives fail=0 with the two argument-taking checks
    # SKIPPED, and every tool exiting 3 gives fail=1 with each one named. The
    # risk in this block was never the tools - it is PIPESTATUS surviving a
    # pipeline inside a loop inside an if, and that is what the stubs test.
    #
    # AND THE STUBS DID NOT CATCH THE REAL FAULT, which is worth knowing
    # before trusting them again. Stubs return the exit codes I choose;
    # tools return the ones they have. verify_code.py returned 1 for the one
    # code in the shipped lemmings.cod that image 0x05042 does not reproduce
    # - a 1992 typo the ORIGINAL refuses too, which that tool prints nine
    # lines explaining - and inside a sweep only the status survives, so it
    # read as FAILED. Found by running the whole thing, after the stub test
    # and the per-tool runs had both passed. Run it for real once.
    for u in verify_dat verify_level verify_digits verify_code \
             verify_shift verify_preshift; do
        uv run python "tools/$u.py" 2>&1 \
            | grep -vE "^pygame|^ *\[vga\]" | sed "s/^/  /"
        check_pipe "tools/$u.py"
    done
    # verify_data.py takes a MEMORY DUMP, not nothing: it compares the bytes
    # the original has in RAM after loading against what the port makes of the
    # same file. Run bare it exits 2 on a missing argument, and check_pipe
    # would report that as a failed sweep - a false alarm of my own making,
    # caught by running it. Its input comes from
    #   play.py --auto --start --dump-mem "0x10000 0xA0000 out/mem.bin"
    if [ -s out/mem.bin ]; then
        uv run python tools/verify_data.py out/mem.bin 2>&1 \
            | grep -vE "^pygame|^ *\[vga\]" | sed "s/^/  /"
        check_pipe "tools/verify_data.py"
    else
        line "tools/verify_data.py" "SKIPPED - no out/mem.bin"
        say "    record one with play.py --dump-mem; see that file's header"
    fi
    if [ -s out/orig_opl.txt ] && [ -s out/port_opl.txt ]; then
        uv run python tools/verify_adlib.py out/orig_opl.txt out/port_opl.txt \
            2>&1 | sed "s/^/  /"
        check_pipe "tools/verify_adlib.py"
    else
        line "tools/verify_adlib.py" "SKIPPED - no out/orig_opl.txt pair"
        say "    record one with the recipe in that file's header. It has six"
        say "    traps and every one scores a number rather than erroring:"
        say "    LEM_OPL_TUNE reads as nothing, --start is what reaches the"
        say "    music, the tick budget must match the capture, the log must"
        say "    begin before the music, and the slot5 lines are the DRIVER's"
        say "    own write - fed back they make start_music refuse to start."
    fi
fi

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
            "level007.dat 1 ref_i101 1100 -1" \
            "level009.dat 1 ref_i119 1100 -1"; do
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
