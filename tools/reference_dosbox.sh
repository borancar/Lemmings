#!/usr/bin/env bash
# Capture what DOSBox makes of the original, as a reference to check against.
#
# DOSBox has no read-only guarantee and the game rewrites RUSSELL.DAT, so this
# mounts a COPY of the game directory. The originals are never exposed to it.
#
#   tools/reference_dosbox.sh OUTDIR [seconds-before-shot]
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:?usage: reference_dosbox.sh OUTDIR [settle-seconds]}"
SETTLE="${2:-20}"
mkdir -p "$OUT"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cp -r "$REPO/lemming1.pc" "$WORK/game"
chmod -R u+w "$WORK/game"

cat > "$WORK/dosbox.conf" <<CONF
[sdl]
fullscreen=false
output=surface
windowresolution=640x400
[dosbox]
machine=vgaonly
[cpu]
core=normal
cycles=fixed 8000
[autoexec]
mount c "$WORK/game"
c:
vgalemmi.exe
CONF

export SDL_VIDEODRIVER=x11
xvfb-run -a -s "-screen 0 1024x768x24" bash -c '
  set -e
  dosbox -conf "'"$WORK"'/dosbox.conf" >"'"$OUT"'/dosbox.log" 2>&1 &
  DP=$!
  sleep 6
  WID=$(xdotool search --name -- "DOSBox" | head -1 || true)
  echo "window id: ${WID:-none}"
  import -window root "'"$OUT"'/00-adapter-menu.png" || true

  # 1 = VGA, then 2 = High Performance PCs. Digits confirm outright.
  xdotool key --clearmodifiers 1; sleep 3
  import -window root "'"$OUT"'/01-machine-menu.png" || true
  xdotool key --clearmodifiers 2; sleep '"$SETTLE"'
  import -window root "'"$OUT"'/02-after-machine.png" || true
  sleep 10
  import -window root "'"$OUT"'/03-later.png" || true

  kill $DP 2>/dev/null || true
  wait $DP 2>/dev/null || true
'
echo "captures in $OUT:"
ls -la "$OUT"
