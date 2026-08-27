#!/usr/bin/env bash
# Play the game in DOSBox, with sound, WITHOUT touching the originals.
#
# This exists because of a hazard CLAUDE.md already records: RUSSELL.DAT is a
# working file the game rewrites, and a drifted one sends the game down its
# floppy path at image 0x0062B - "Lemmings Disk 1 Not found" - before it draws
# anything. `LemmingsVga` seeds it from RUSSELL.VAR on every run, so our
# emulator is safe; DOSBox has no such guarantee and would rewrite the file in
# place, breaking every later emulator run in a way `git checkout` does not
# reliably fix, because the committed copy is itself a used one.
#
# So DOSBox gets a COPY, seeded clean, in a temporary directory.
#
#   tools/play_dosbox.sh            play it
#   tools/play_dosbox.sh --keep     and keep the directory afterwards
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d -t lemmings-dosbox-XXXXXX)"
if [ "${1:-}" != "--keep" ]; then
    trap 'rm -rf "$WORK"' EXIT
else
    echo "keeping $WORK"
fi

cp -r "$REPO/lemming1.pc" "$WORK/game"
chmod -R u+w "$WORK/game"
cp "$WORK/game/russell.var" "$WORK/game/russell.dat"

cat > "$WORK/dosbox.conf" <<CONF
[sdl]
fullscreen=false
output=texture
[dosbox]
machine=vgaonly
[render]
aspect=false
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
# Off, so what you hear is what the chip produced - see docs/sound.md, where
# it flattened a comparison's loudness envelope to a constant.
compressor=off
[sblaster]
sbtype=none
oplmode=opl2
[autoexec]
mount c "$WORK/game"
c:
vgalemmi.exe
CONF

echo "  1 for VGA, then 2 for High Performance PCs."
exec dosbox -conf "$WORK/dosbox.conf"
