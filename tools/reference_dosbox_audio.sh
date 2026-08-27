#!/usr/bin/env bash
# Record what DOSBox's OWN OPL makes of the game - an independent chip.
#
# Every audio comparison in this project so far has gone through ymfm at both
# ends: our emulator has no synthesiser, it logs register writes, so "the
# emulator's WAV" and "the port's WAV" are the same chip fed two streams.
# That settles the DRIVER and says nothing about the CHIP.
#
# DOSBox brings its own OPL emulation and its own copy of the game, so it
# agrees with neither by construction. If its rendering has voices ours does
# not, the difference is in the chip; if it sounds the same, it is not.
#
# The audio is captured with SDL2's `disk` driver, which writes exactly what
# the program sends to the device - no key presses, no capture toggle.
#
#   tools/reference_dosbox_audio.sh OUTDIR [record-seconds]
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:?usage: reference_dosbox_audio.sh OUTDIR [seconds]}"
SECS="${2:-110}"
mkdir -p "$OUT"

# DOSBox has no read-only guarantee and the game rewrites RUSSELL.DAT, so it
# gets a COPY. The originals are never exposed to it - see the note in
# tools/reference_dosbox.sh, and CLAUDE.md on RUSSELL.VAR.
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cp -r "$REPO/lemming1.pc" "$WORK/game"
chmod -R u+w "$WORK/game"
cp "$WORK/game/russell.var" "$WORK/game/russell.dat"

RAW="$WORK/audio.raw"

cat > "$WORK/dosbox.conf" <<CONF
[sdl]
fullscreen=false
output=texture
[dosbox]
machine=vgaonly
[cpu]
core=normal
cycles=fixed 8000
[mixer]
rate=48000
blocksize=1024
prebuffer=25
# The master compressor is ON by default and it FLATTENS THE DYNAMICS. The
# first recording had a five-second RMS of 1870..2112 for the whole two
# minutes, which is not what music does, and every comparison against it by
# loudness envelope was measuring the compressor. Off, so this is what the
# chip produced.
compressor=off
[sblaster]
sbtype=none
oplmode=opl2
[autoexec]
mount c "$WORK/game"
c:
vgalemmi.exe
CONF

export SDL_VIDEODRIVER=x11
export SDL_AUDIODRIVER=disk
export SDL_DISKAUDIOFILE="$RAW"
# NO SDL_DISKAUDIODELAY. Unset, SDL's disk driver waits one buffer's worth
# between writes and so records in REAL TIME. Setting it to 0 makes it consume
# as fast as it can: the first run here produced 350 seconds of audio in about
# 180 of wall clock, which is not a recording of anything - the mixer is
# drained faster than the emulated machine fills it, and what comes out is
# time-mangled. It still looks like music, and its spectrum still looks like
# an OPL, which is why it took three rounds to catch.

xvfb-run -a -s "-screen 0 1024x768x24" bash -c '
  set -e
  dosbox -conf "'"$WORK"'/dosbox.conf" >"'"$OUT"'/dosbox.log" 2>&1 &
  DP=$!
  sleep 8
  # SCREENSHOTS AT EVERY STAGE. Without them a capture is a WAV with no way
  # to say what was on screen when it was made - and the first run of this
  # produced audio that correlated 0.046 with the port, which could equally
  # have been a different tune, a different screen or a different level. A
  # recording nobody can place is not a reference.
  import -window root "'"$OUT"'/00-adapter-menu.png" || true
  # 1 = VGA, then 2 = High Performance PCs.
  xdotool key --clearmodifiers 1; sleep 3
  import -window root "'"$OUT"'/01-machine-menu.png" || true
  xdotool key --clearmodifiers 2; sleep 22
  import -window root "'"$OUT"'/02-title.png" || true
  # F1 starts a game; the title drops keys during its fade, so press more
  # than once rather than betting on one moment.
  for i in 1 2 3; do xdotool key --clearmodifiers F1; sleep 4; done
  import -window root "'"$OUT"'/03-briefing.png" || true
  # The briefing wants a real button, HELD - a tap between two of its polls
  # is invisible to it.
  for i in 1 2 3; do
    xdotool mousemove 320 240 mousedown 1; sleep 1; xdotool mouseup 1; sleep 5
  done
  import -window root "'"$OUT"'/04-level.png" || true
  sleep '"$SECS"'
  import -window root "'"$OUT"'/05-end-of-recording.png" || true
  kill $DP 2>/dev/null || true
  sleep 2
' || true

if [ ! -s "$RAW" ]; then
    echo "NO AUDIO WAS CAPTURED. That is a fact about this run, not about" >&2
    echo "DOSBox - check $OUT/dosbox.log before reading it as silence." >&2
    exit 1
fi

python3 "$REPO/tools/dosbox_audio_wav.py" "$RAW" "$OUT/dosbox.wav"
