#!/usr/bin/env python3
"""What the original's AdLib driver writes to the chip, in order.

This is the sound equivalent of `prove.sh`: it compares no pictures and
listens to nothing.  It captures the OPL2 register stream the ORIGINAL emits,
and that stream is the reference a transcribed driver has to reproduce.

The game only reaches its sound code when a card answers, and until now none
did - the shared emulator returned 0x00 from port 0x388, so the driver's
detect failed at image 0x0059F and every run took the INT 60h path instead.
`LemmingsVga` now answers the AdLib manual's detect, so `adlib.dat` installs
and runs.

Ride it along with play.py, which is the only driver that gets past the menu:

    LEM_HOOK=tools/probe_opl.py LEM_OPL_OUT=out/opl_menu.txt \
        uv run python tools/play.py --auto -- --run-seconds 40

A run that captures NOTHING says so and fails.  Nothing captured and a silent
game look identical in a log, and only one of them is a result.
"""
import atexit
import collections
import os

OUT = os.environ.get("LEM_OPL_OUT", "out/opl.txt")


def reg_name(r):
    """The OPL2's register map, enough to read a stream by eye.

    Operator registers are indexed by SLOT and the slots are NOT contiguous -
    0x06, 0x07, 0x0E and 0x0F are gaps.  That is exactly why the driver's own
    reset table at +0x006D skips 0x86, 0x87, 0x8E and 0x8F, and a name table
    that assumed 18 consecutive operators would mislabel two thirds of them.
    """
    if r == 0x01: return "test / waveform-select enable"
    if r == 0x02: return "timer 1 preset"
    if r == 0x03: return "timer 2 preset"
    if r == 0x04: return "timer control"
    if r == 0x08: return "CSM / note-select"
    if r == 0xBD: return "rhythm mode, AM+vib depth"
    for base, what in ((0x20, "tremolo/vib/sustain/KSR/multiple"),
                       (0x40, "key-scale level + total level"),
                       (0x60, "attack + decay"),
                       (0x80, "sustain + release"),
                       (0xE0, "waveform")):
        if base <= r <= base + 0x15:
            off = r - base
            if (off & 7) <= 5:
                return "op %2d  %s" % ((off >> 3) * 6 + (off & 7), what)
            return "(gap slot %02X)" % off
    if 0xA0 <= r <= 0xA8: return "ch %d  f-number low" % (r - 0xA0)
    if 0xB0 <= r <= 0xB8: return "ch %d  key-on + block + f-number high" % (r - 0xB0)
    if 0xC0 <= r <= 0xC8: return "ch %d  feedback + connection" % (r - 0xC0)
    return "?"


def install(m):
    def report():
        log = getattr(m, "opl_log", [])
        print()
        print("  [opl] the detect at driver +0x00A3 : %s" %
              ("PASSED - adlib.dat believes it has a card"
               if getattr(m, "opl_detected", False) else "did NOT pass"))
        print("  [opl] register writes captured      : %d" % len(log))
        if not log:
            print("  [opl] NOTHING WAS CAPTURED.  That is a fact about this")
            print("  [opl] run, not about the game - check the line above")
            print("  [opl] before reading it as silence.")
            return
        hist = collections.Counter(r for _, _, r, _ in log)
        keyed = sorted(r - 0xB0 for r in hist if 0xB0 <= r <= 0xB8)
        divs = collections.Counter(d for _, d, _, _ in log)
        span = log[-1][0] - log[0][0]
        print("  [opl] distinct registers touched    : %d" % len(hist))
        print("  [opl] channels ever addressed       : %s" % (keyed or "none"))
        print("  [opl] rhythm mode (0xBD) written    : %s"
              % ("yes" if 0xBD in hist else "no"))
        print("  [opl] PIT ticks spanned             : %d" % span)
        for d, n in divs.most_common():
            print("  [opl]   divisor %-6d = %7.2f Hz : %d writes"
                  % (d, 1193182.0 / d, n))
        os.makedirs(os.path.dirname(os.path.abspath(OUT)) or ".",
                    exist_ok=True)
        with open(OUT, "w") as f:
            f.write("# tick divisor reg val   (PIT_HZ = 1193182)\n")
            for t, d, r, v in log:
                f.write("%d %d %02X %02X  %s\n" % (t, d, r, v, reg_name(r)))
        print("  [opl] wrote %s" % OUT)

    atexit.register(report)
    print("  [opl] watching ports 0x388/0x389")
