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


REQ_OUT = os.environ.get("LEM_OPL_REQ")


def _driver_segment(m):
    """Where DOS put adlib.dat, read out of the vector the game installed.

    The driver is called through INT 61h and its dispatcher is at offset 0 of
    its own segment, so the vector's segment is the driver's base and the
    three request slots - driver +0x00ED, +0x00EE, +0x00F0 - follow from it.
    Reading the vector rather than assuming a segment is the same rule this
    project already writes down for SS: ask the machine where a thing is.
    """
    import struct
    off, seg = struct.unpack("<HH", bytes(m.uc.mem_read(0x61 * 4, 4)))
    return seg, off


def install(m):
    """Log every OPL register write, and every SOUND THE GAME ASKS FOR.

    The requests are the port's inputs. Capturing the register stream alone
    would give something to compare against and no way to reproduce it: the
    transcribed driver plays what it is asked for, so a comparison needs the
    asking as well as the answer. `LEM_OPL_REQ` names where they go.

    The slots are watched with a THREE-BYTE memory hook rather than a code
    hook. A UC_HOOK_CODE callback costs so much throughput that the same run
    reached 40 register writes instead of 10,109 - the guest never got near
    the music, and the probe reported that as the music never starting.
    """
    state = {"armed": False, "reqs": []}

    def arm(mm):
        # The vector is only meaningful once the driver has answered, and the
        # cheapest moment to know that is the first register write it makes.
        if state["armed"]:
            return
        state["armed"] = True
        mm.opl_on_write = None            # one-shot: stop paying for it
        try:
            from unicorn import UC_HOOK_MEM_WRITE
            seg, off = _driver_segment(mm)
            base = (seg << 4)
            def on_slot(uc, access, address, size, value, user):
                slot = address - base
                kind = {0xED: "music", 0xEE: "slot5", 0xF0: "effect"}.get(slot)
                if kind and value:
                    state["reqs"].append((mm.timer_ticks,
                                          mm.pit0_div or 0x10000, kind, value))
            mm.uc.hook_add(UC_HOOK_MEM_WRITE, on_slot,
                           begin=base + 0xED, end=base + 0xF0)
            print("  [opl] driver at %04X:%04X; watching its request slots"
                  % (seg, off))
        except Exception as exc:
            # An exception raised inside a Unicorn hook is SWALLOWED and stops
            # emulation, which reads as a run that ended early and, with the
            # log capped at exactly the trigger, as a plausible capture. Two
            # runs were lost to that before the write count being exactly the
            # threshold gave it away.
            import traceback
            print("  [opl] COULD NOT WATCH THE REQUEST SLOTS: %r" % (exc,))
            traceback.print_exc()

    m.opl_on_write = arm

    def report():
        reqs = state["reqs"]
        print()
        print("  [opl] sounds the GAME asked for      : %d" % len(reqs))
        for t, d, kind, v in reqs[:12]:
            print("  [opl]     t%-6d %-6s %d" % (t, kind, v))
        if len(reqs) > 12:
            print("  [opl]     ... %d more" % (len(reqs) - 12))
        if REQ_OUT and reqs:
            with open(REQ_OUT, "w") as f:
                f.write("# tick divisor kind id\n")
                for t, d, kind, v in reqs:
                    f.write("%d %d %s %d\n" % (t, d, kind, v))
            print("  [opl] wrote %s" % REQ_OUT)
        elif REQ_OUT:
            print("  [opl] NO REQUESTS SEEN, so %s was not written - the port"
                  % REQ_OUT)
            print("  [opl] has nothing to be driven with and a comparison")
            print("  [opl] would be of two silences.")
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
