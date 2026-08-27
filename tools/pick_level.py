"""Open the ORIGINAL on a chosen level, in a window, to look at it.

tools/play.py reaches Fun 1 and nothing else. tools/record_level.py can poke
ds:0x1F6B - the index across the whole game's running order - but it is a
recorder and drives everything headless. This is the same poke as a plain
LEM_HOOK, so play.py --window can be pointed anywhere:

    LEM_LEVEL=77 uv run python tools/play.py --window --auto --start \
        -- --scale 3

77 is level004.dat section 3, "Tribute to M.C.Escher". The index is NOT the
file and section - LEVEL_ORDER is a genuine permutation - and the mapping is
`n = LEVEL_ORDER[i]`, file `(n-1)>>4`, section `((n-1)>>1)&7`, which is how 77
was arrived at rather than guessed.

Image 0x04401 is the menu, by which point the image is unpacked and DS is up.
"""
import os

DGROUP = 0x9A90
LEVEL_INDEX = 0x1F6B
MENU = 0x04401


def install(m):
    from unicorn import UC_HOOK_CODE

    want = os.environ.get("LEM_LEVEL")
    if want is None:
        print("  [pick] LEM_LEVEL not set - the game will start on Fun 1",
              flush=True)
        return
    base = m.load_seg * 16
    st = {"done": False}

    def pick(uc, address, size, user):
        if st["done"]:
            return
        st["done"] = True
        uc.mem_write(base + DGROUP + LEVEL_INDEX, bytes([int(want) & 0xFF]))
        print("  [pick] ds:0x1F6B poked to %s" % want, flush=True)

    m.uc.hook_add(UC_HOOK_CODE, pick, None, base + MENU, base + MENU)
