# SPDX-License-Identifier: GPL-2.0-only
"""Produce a permanently decrypted program image.

The protection keeps exactly one instruction in plaintext at a time and
re-encrypts behind itself (docs/protection.md), so no memory dump ever holds
the protected stretch in the clear - which is why a raw snapshot disassembles
as garbage around image 0x0D00-0x0E40.

But every instruction *is* plaintext for the moment it executes. So: record
the bytes of each instruction as the CPU runs it, then write those bytes back
into the dumped image. What comes out is an image where the protected code is
readable too, and it never has to be produced again.

Recording rides along with an ordinary run rather than having a driver of its
own: set LEM_DECRYPT_OUT and tools/play.py records while it plays. That
matters, because only code that actually executes can be recovered, and
play.py is what gets past the menus, the protection and the briefing into the
level. A hand-rolled driver that stops at the title screen recovers a small
fraction of the code and gives no sign that it did.

  # rebuild the image from the records committed in image/rec (a second)
  uv run python tools/decrypt_image.py --merge image/rec/*.rec

  # re-record from the game itself (minutes; drives play.py into level one)
  uv run python tools/decrypt_image.py --record

  # merge records from runs captured separately
  uv run python tools/decrypt_image.py --merge image/rec/*.rec

  uv run python tools/disasm.py image/vgalemmi.decrypted.img 0xdff -n 40

Each run is merged into the same image, so coverage only grows: reach a new
screen, record it, merge it in. The report says how many bytes were recovered
and how many of them differed from the raw dump - that difference is exactly
what the protection was hiding.
"""
import argparse
import glob
import hashlib
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lemmings

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def record_a_run(rec_path, seconds, extra):
    """Drive tools/play.py into the first level with recording turned on."""
    os.makedirs(os.path.dirname(os.path.abspath(rec_path)) or ".",
                exist_ok=True)
    env = dict(os.environ, LEM_DECRYPT_OUT=os.path.abspath(rec_path))
    cmd = [sys.executable, os.path.join(HERE, "play.py"),
           "--auto", "--start", "--start-delay", "4", "--start-tries", "3",
           "--click-tries", "4", "--",
           "--run-seconds", str(seconds)] + list(extra)
    print(f"recording a run into {rec_path} ({seconds}s of guest time) ...")
    r = subprocess.run(cmd, cwd=ROOT, env=env)
    if not os.path.exists(rec_path):
        raise SystemExit(f"play.py exited {r.returncode} and wrote no records")
    return rec_path


def record_startup(rec_path, rounds=400, media=None):
    """Record the start-up path in-process, without a window or a driver.

    play.py reaches the level but takes a route through start-up that never
    executes the protected stub - measured, not assumed: a 250-second recorded
    run has a clean gap at image 0x0634..0x0E66. Neither run is a superset of
    the other, which is the whole reason records merge rather than one run
    producing the image.

    `media` overrides the byte INT 21h AH=1Bh reports for the default drive,
    and it is what decides whether the protection runs at all:

        media 0xF8 (fixed disk) -> INT 01h fires 0 times
        media 0xF9 (floppy)     -> INT 01h fires 2945 times

    So the trap-flag layer is the *floppy* path. On a hard disk the game never
    installs the handler, which is why an ordinary run recovers none of it.
    Recording once with 0xF9 is the only way to read that code, and the
    plaintext it yields is the binary's own either way.
    """
    import lemmings as L
    from dos_emulator import set_game_dir, KEYMAP
    from dos_emulator.emulator import MEDIA_ID_ADDR
    from unicorn.x86_const import UC_X86_REG_CS, UC_X86_REG_IP
    import pygame

    cls = L.RecordingLemmingsVga
    if media is not None:
        class cls(L.RecordingLemmingsVga):           # noqa: F811
            def _dos(self):
                r = super()._dos()
                self.uc.mem_write(MEDIA_ID_ADDR, bytes([media]))
                return r

    set_game_dir(L.GAME_DIR)
    pygame.init()
    m = cls(L.EXE, max_insns=1 << 62, psp_seg=L.PSP_SEG)
    keys = [getattr(pygame, f"K_{k}") for k in L.STARTUP_KEYS]
    addr, sent = m.start, 0
    print(f"recording the start-up path into {rec_path} ...")
    for _ in range(rounds):
        m.blocked_on_input = False
        try:
            m.uc.emu_start(addr, 0, count=200_000)
        except Exception as e:
            print("  cpu:", e)
            break
        addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
        m.service_keyboard()
        if m.finished:
            print(f"  program exited: {m.finished}")
            break
        if m.blocked_on_input and sent < len(keys):
            sc, asc = KEYMAP[keys[sent]]
            m.press_key(sc, asc, True)
            m.press_key(sc, asc, False)
            sent += 1
    os.makedirs(os.path.dirname(os.path.abspath(rec_path)) or ".",
                exist_ok=True)
    L.write_records(rec_path, m.seen)
    print(f"  recorded {len(m.seen)} distinct instructions")
    return rec_path


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", default="image/vgalemmi.decrypted.img")
    ap.add_argument("--base", default="image/vgalemmi.img",
                    help="raw dumped image to patch the recovered code into")
    ap.add_argument("--merge", nargs="*", metavar="REC",
                    help="merge these .rec files instead of recording a run")
    ap.add_argument("--record", action="store_true",
                    help="re-record from the game instead of using image/rec")
    ap.add_argument("--seconds", type=int, default=200,
                    help="guest seconds to record for (default: 200)")
    ap.add_argument("--keep", default="image/rec",
                    help="directory to keep recorded .rec files in")
    ap.add_argument("--startup-only", action="store_true",
                    help="record only the start-up path (no level run)")
    a, extra = ap.parse_known_args()

    recs = []
    if a.merge is not None:
        for pat in a.merge:
            recs.extend(sorted(glob.glob(pat)) or [pat])
    elif not a.record:
        recs = sorted(glob.glob(os.path.join(a.keep, "*.rec")))
        if not recs:
            raise SystemExit(f"no records in {a.keep} - pass --record")
    else:
        recs = sorted(glob.glob(os.path.join(a.keep, "*.rec")))
        fresh = [record_startup(os.path.join(a.keep, "startup.rec")),
                 record_startup(os.path.join(a.keep, "protection.rec"),
                                media=0xF9)]
        if not a.startup_only:
            fresh.append(record_a_run(
                os.path.join(a.keep, f"level-{a.seconds}s.rec"),
                a.seconds, extra))
        for f_ in fresh:
            if f_ not in recs:
                recs.append(f_)
    if not recs:
        raise SystemExit("nothing to merge")

    # Merge every run's records. Where two runs disagree about an offset the
    # protection decrypts to different bytes on different passes, which would
    # be worth knowing about - so count it rather than silently take one.
    seen, conflicts = {}, 0
    per_run = []
    for r in recs:
        one = lemmings.read_records(r)
        per_run.append((r, len(one)))
        for off, b in one.items():
            if off in seen and seen[off] != b:
                conflicts += 1
            seen[off] = b

    if not os.path.exists(a.base):
        raise SystemExit(
            f"{a.base} missing - run tools/extract_image.py first")
    with open(a.base, "rb") as f:
        patched = bytearray(f.read())

    # Two very different things make a recorded instruction differ from the
    # dumped image, and reporting them together reads as a much bigger
    # protection than there is:
    #   - the image held other non-zero bytes there -> it was encrypted
    #   - the image held zeros there                -> BSS, and the bytes are
    #     code the program loaded at runtime (the AdLib driver, for one)
    recovered = differed = 0
    enc, bss = [], []
    for off in sorted(seen):
        b = seen[off]
        if off + len(b) > len(patched):
            continue
        recovered += len(b)
        was = bytes(patched[off:off + len(b)])
        if was != b:
            differed += len(b)
            into = bss if was == bytes(len(b)) else enc
            if into and off <= into[-1][1] + 16:
                into[-1][1] = off + len(b) - 1
            else:
                into.append([off, off + len(b) - 1])
        patched[off:off + len(b)] = b
    spans = enc

    os.makedirs(os.path.dirname(os.path.abspath(a.o)) or ".", exist_ok=True)
    with open(a.o, "wb") as f:
        f.write(patched)

    meta = {
        "source": os.path.basename(lemmings.EXE),
        "base_image": a.base,
        "image_sha256": hashlib.sha256(bytes(patched)).hexdigest(),
        "length": len(patched),
        "psp_seg": lemmings.PSP_SEG,
        "dgroup_image_offset": lemmings.DGROUP,
        "runs_merged": [{"file": r, "instructions": n} for r, n in per_run],
        "instructions_recovered": len(seen),
        "bytes_recovered": recovered,
        "bytes_that_differed_from_the_dump": differed,
        "offsets_where_runs_disagreed": conflicts,
        "encrypted_spans": [[hex(x), hex(y)] for x, y in enc],
        "runtime_loaded_spans": [[hex(x), hex(y)] for x, y in bss],
        "note": ("Only code that executed is recovered; the rest is the raw "
                 "dumped image. encrypted_spans is where the image held "
                 "different non-zero bytes - that is what the protection was "
                 "hiding. runtime_loaded_spans is BSS the program filled in "
                 "at run time (the AdLib driver, for one), which is not "
                 "protection at all. Merge more runs to grow coverage."),
    }
    with open(os.path.splitext(a.o)[0] + ".json", "w") as f:
        json.dump(meta, f, indent=2)
        f.write("\n")

    print(f"\nwrote {a.o}")
    for r, n in per_run:
        print(f"  {n:6d} instructions from {r}")
    print(f"  distinct instructions executed in the image : {len(seen)}")
    print(f"  bytes recovered                             : {recovered}")
    print(f"  of those, differing from the raw dump       : {differed}")
    print(f"  offsets where two runs disagreed            : {conflicts}")
    print(f"  encrypted spans (image offsets)             : "
          f"{[f'{x:#07x}..{y:#07x}' for x, y in enc]}")
    print(f"  runtime-loaded into BSS (not protection)    : "
          f"{[f'{x:#07x}..{y:#07x}' for x, y in bss]}")


if __name__ == "__main__":
    main()
