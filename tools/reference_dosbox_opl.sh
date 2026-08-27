#!/usr/bin/env bash
# Capture DOSBox's OPL REGISTER STREAM - not its audio.
#
# Three audio captures in a row were spoiled by the instrument (a float format
# read as s16, a master compressor flattening the envelope, a disk driver
# draining the mixer faster than the guest filled it), and each produced a
# plausible file. The register stream has none of those failure modes: it is
# what the game asked the chip to do, and it compares against
# tools/probe_opl.py's capture write for write.
#
# DOSBox's `caprawopl` writes a .dro. The binding is Ctrl+Alt+F7.
#
#   tools/reference_dosbox_opl.sh OUTDIR [record-seconds]
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:?usage: reference_dosbox_opl.sh OUTDIR [seconds]}"
SECS="${2:-100}"
mkdir -p "$OUT"

# DOSBox has no read-only guarantee and the game rewrites RUSSELL.DAT, so it
# gets a COPY seeded from the shipped RUSSELL.VAR - see CLAUDE.md.
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cp -r "$REPO/lemming1.pc" "$WORK/game"
chmod -R u+w "$WORK/game"
cp "$WORK/game/russell.var" "$WORK/game/russell.dat"
mkdir -p "$WORK/capture"

cat > "$WORK/dosbox.conf" <<CONF
[sdl]
fullscreen=false
output=texture
[dosbox]
machine=vgaonly
[capture]
capture_dir=$WORK/capture
[cpu]
core=normal
# NOT 8000 cycles. That is roughly an 8 MHz 8086 and the game cannot keep up
# on it - its own clock ran 25 game-seconds in 30 real ones - and a reference
# that is CPU-starved measures the emulator's throughput rather than the
# game's intent. This project already records that trap for its own emulator:
# the rate has to be read out of what the loop waits ON.
#
# NOTE FOR ANYONE EDITING THIS FILE: the heredoc below is UNQUOTED, because
# the config needs \$WORK expanded. A backtick in a comment inside it is a
# command substitution, and one here made the shell try to run 'fixed 8000'
# and silently drop the whole [cpu] section.
cycles=fixed 25000
[mixer]
rate=48000
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
# No audio device is needed: only the register writes are wanted.
export SDL_AUDIODRIVER=dummy

xvfb-run -a -s "-screen 0 1024x768x24" bash -c '
  set -e
  dosbox -conf "'"$WORK"'/dosbox.conf" >"'"$OUT"'/dosbox.log" 2>&1 &
  DP=$!
  sleep 8
  xdotool key --clearmodifiers 1; sleep 3
  xdotool key --clearmodifiers 2; sleep 22
  for i in 1 2 3; do xdotool key --clearmodifiers F1; sleep 4; done
  for i in 1 2 3; do
    xdotool mousemove 320 240 mousedown 1; sleep 1; xdotool mouseup 1; sleep 5
  done
  import -window root "'"$OUT"'/level.png" || true
  # Ctrl+F7. dosbox-staging registers the handler with PRIMARY_MOD, which is
  # Ctrl on Linux. The first attempt swept four candidates and one of them
  # started a VIDEO capture instead, which is its own toggle and got in the
  # way; and the run was killed before the .dro could be flushed.
  xdotool key --clearmodifiers ctrl+F7
  sleep '"$SECS"'
  xdotool key --clearmodifiers ctrl+F7
  # The file is written when the capture CLOSES. Give it room before the
  # process goes away.
  sleep 6
  import -window root "'"$OUT"'/end.png" || true
  kill $DP 2>/dev/null || true
  sleep 5
' || true

found=$(find "$WORK/capture" -iname '*.dro' | head -1 || true)
if [ -z "$found" ]; then
    echo "NO .dro WAS WRITTEN. That is a fact about this run, not about" >&2
    echo "DOSBox - the Ctrl+Alt+F7 binding may differ in this build." >&2
    find "$WORK/capture" -type f -printf '  left behind: %p\n' >&2 || true
    exit 1
fi
cp "$found" "$OUT/dosbox.dro"
ls -la "$OUT/dosbox.dro"
