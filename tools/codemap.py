# SPDX-License-Identifier: GPL-2.0-only
"""Map the code in the decrypted image, by recursive descent.

Linear disassembly desynchronises constantly in hand-written assembly: data
sits between routines and jumps land mid-instruction, so a linear pass produces
plausible-looking garbage and no way to tell which parts. Recursive descent
from the entry point only ever decodes bytes something actually reaches.

Two seeds, and the second is the one that makes this trustworthy:

  * image 0x0000, the entry point PKLITE restores.
  * **every offset recorded as having executed** (image/rec/*.rec). Those are
    not inferred - a real run put the instruction pointer there. Seeding with
    them reaches code behind indirect calls and handler tables that static
    descent cannot follow, and lets the report separate "proven to be code"
    from "believed to be code".

  uv run python tools/codemap.py                 # -> image/codemap.json + table
  uv run python tools/codemap.py --routine 0x376a

What comes out is a routine list, not a disassembly. A "routine" here is an
address something called - a `call` target, an interrupt vector, or a recorded
entry - together with how far descent got from it. That is the honest unit:
this binary has no prologues to delimit functions by (see docs/toolchain.md).
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lemmings
import capstone

# Instructions that end a straight-line run.
STOP = {"ret", "retf", "iret", "iretd", "jmp", "ljmp", "hlt"}
COND = {"je", "jne", "jz", "jnz", "ja", "jae", "jb", "jbe", "jg", "jge",
        "jl", "jle", "js", "jns", "jo", "jno", "jp", "jnp", "jc", "jnc",
        "jcxz", "loop", "loope", "loopne"}


def descend(img, seeds, limit):
    """Recursive descent. Returns (decoded, calls, targets_of, bad)."""
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
    md.detail = True
    decoded = {}                       # offset -> (size, mnemonic, op_str)
    calls = {}                         # call target -> set of call sites
    bad = set()                        # offsets that would not decode
    work = list(seeds)
    seen = set(seeds)
    while work:
        off = work.pop()
        while 0 <= off < limit:
            if off in decoded:
                break
            try:
                ins = next(md.disasm(img[off:off + 15], off))
            except StopIteration:
                bad.add(off)
                break
            decoded[off] = (ins.size, ins.mnemonic, ins.op_str)
            m, ops = ins.mnemonic, ins.op_str
            direct = None
            if ops and ops[0] == "0" and "x" in ops and "[" not in ops:
                try:
                    direct = int(ops, 0)
                except ValueError:
                    direct = None
            if m == "call" and direct is not None and 0 <= direct < limit:
                calls.setdefault(direct, set()).add(off)
                if direct not in seen:
                    seen.add(direct)
                    work.append(direct)
            elif m in COND and direct is not None and 0 <= direct < limit:
                if direct not in seen:
                    seen.add(direct)
                    work.append(direct)
            elif m == "jmp":
                if direct is not None and 0 <= direct < limit \
                        and direct not in seen:
                    seen.add(direct)
                    work.append(direct)
                break
            if m in STOP:
                break
            off += ins.size
    return decoded, calls, bad


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", default="image/vgalemmi.decrypted.img")
    ap.add_argument("--rec", default="image/rec")
    ap.add_argument("-o", default="image/codemap.json")
    ap.add_argument("--routine", type=lambda v: int(v, 0),
                    help="print what is known about one routine and exit")
    ap.add_argument("--limit", type=lambda v: int(v, 0),
                    default=lemmings.DGROUP,
                    help="descend only below this offset (default: DGROUP)")
    ap.add_argument("--no-exec-seeds", action="store_true",
                    help="descend from the entry point alone, for comparison")
    ap.add_argument("--tables", action="store_true",
                    help="find the handler tables the game dispatches through")
    ap.add_argument("--table-min", type=int, default=3,
                    help="shortest run of addresses to call a table")
    a = ap.parse_args()

    with open(a.image, "rb") as f:
        img = f.read()

    executed = {}
    import glob
    for r in sorted(glob.glob(os.path.join(a.rec, "*.rec"))):
        executed.update(lemmings.read_records(r))
    ran = {o for o in executed if o < a.limit}

    seeds = {0} | (set() if a.no_exec_seeds else ran)
    decoded, calls, bad = descend(img, seeds, a.limit)

    if a.routine is not None:
        off = a.routine
        print(f"routine {off:#07x}")
        print(f"  called from : "
              f"{sorted(hex(x) for x in calls.get(off, ())) or 'nothing static'}")
        print(f"  executed    : {'yes' if off in ran else 'no'}")
        n = 0
        while off in decoded and n < 40:
            size, m, ops = decoded[off]
            mark = "*" if off in ran else " "
            print(f"  {mark} {off:#07x}  {img[off:off+size].hex():<12s} {m} {ops}")
            if m in STOP:
                break
            off += size
            n += 1
        print("  ('*' = a run put the instruction pointer here)")
        return

    if a.tables:
        # A dispatch table is a run of consecutive words holding the addresses
        # of *routines*. The obvious filter - "every entry is an instruction
        # boundary" - does not work: descent decodes 36% of the code region, so
        # a run of four hits by chance thousands of times, and the first
        # version of this printed 156 tables that were almost all noise.
        #
        # Requiring "an address that executed" is not enough either. Recorded
        # offsets cover about 5% of the region, and ASCII text hits that at a
        # good rate: the second version of this reported a table at 0x0E6E4
        # whose four entries, read as bytes, spell " rescued".
        #
        # So require a **routine start**: a call target, or an address that
        # executed *and* that a decoded instruction ends at with a ret, iret or
        # jmp. Landing right after a control-transfer is what distinguishes an
        # entry point from an address merely inside a routine, and text cannot
        # satisfy it except by accident twice over.
        ends_run = {}
        for o, (size, m, _) in decoded.items():
            if m in STOP:
                ends_run[o + size] = m
        starts = (set(calls)
                  | {o for o in ran if o in ends_run}) - set(range(0x100))
        n = len(img) // 2
        runs, i = [], 0
        while i < n:
            j, ent = i, []
            while j < n:
                v = int.from_bytes(img[j * 2:j * 2 + 2], "little")
                if v in starts and v not in ent:
                    ent.append(v)
                    j += 1
                else:
                    break
            if j - i >= a.table_min:
                runs.append((i * 2, j - i))
                i = j
            else:
                i += 1
        # An entry that only a table reaches is the interesting kind: nothing
        # in the code calls it directly, so descent alone would never find it.
        print(f"handler tables in {a.image}\n"
              f"(runs of >= {a.table_min} consecutive, distinct routine "
              f"addresses >= 0x100)\n")
        print(f"  {len(starts)} routine starts known, so a word matches by "
              f"chance about\n  {100.0*len(starts)/65536:.2f}% of the time; a "
              f"run of {a.table_min} costs "
              f"{(len(starts)/65536.0)**a.table_min*len(img)/2:.3f} false "
              f"hits across the image.\n")
        for at, count in runs:
            ent = [int.from_bytes(img[at + 2 * k:at + 2 * k + 2], "little")
                   for k in range(count)]
            ran_n = sum(1 for e in ent if e in ran)
            indirect = sum(1 for e in ent if e not in calls)
            print(f"  {at:#07x}  {count:3d} entries   "
                  f"{ran_n:3d} executed   {indirect:3d} reached only here")
            print(f"      {' '.join(f'{e:#06x}' for e in ent[:12])}"
                  f"{' ...' if count > 12 else ''}")
        print(f"\n  {len(runs)} tables, "
              f"{sum(c for _, c in runs)} entries in total.")
        print("  'reached only here' is the count nothing in the code calls "
              "directly -\n  those are the entries static descent can never "
              "find on its own.")
        return

    # Bytes covered, and how much of that is proven rather than inferred.
    covered = sum(s for s, _, _ in decoded.values())
    proven = sum(decoded[o][0] for o in decoded if o in ran)
    only_static = sorted(o for o in calls if o not in ran)
    unreached = sorted(o for o in ran if o not in decoded)

    out = {
        "image": a.image,
        "code_region": [0, a.limit],
        "seeds": "entry + recorded executions"
                 if not a.no_exec_seeds else "entry only",
        "instructions_decoded": len(decoded),
        "bytes_decoded": covered,
        "bytes_proven_executed": proven,
        "code_region_bytes": a.limit,
        "call_targets": len(calls),
        "call_targets_never_executed": len(only_static),
        "undecodable_offsets": len(bad),
        "routines": [
            {"at": hex(o), "callers": len(calls[o]), "executed": o in ran}
            for o in sorted(calls)
        ],
    }
    os.makedirs(os.path.dirname(os.path.abspath(a.o)) or ".", exist_ok=True)
    with open(a.o, "w") as f:
        json.dump(out, f, indent=2)
        f.write("\n")

    pc = 100.0 * covered / a.limit
    pp = 100.0 * proven / a.limit
    print(f"wrote {a.o}\n")
    print(f"  code region                          : 0..{a.limit:#07x} "
          f"({a.limit} bytes)")
    print(f"  instructions decoded                 : {len(decoded)}")
    print(f"  bytes decoded                        : {covered}  ({pc:.2f}%)")
    print(f"  of those, PROVEN executed by a run   : {proven}  ({pp:.2f}%)")
    print(f"  call targets found                   : {len(calls)}")
    print(f"  call targets never executed          : {len(only_static)}")
    print(f"  offsets that would not decode        : {len(bad)}")
    if unreached:
        print(f"  recorded offsets descent never met   : {len(unreached)}"
              f"  (indirect entries; they were seeds)")
    print("\n  'decoded' is what descent reached. Only the proven figure is "
          "evidence;\n  the rest is a hypothesis that those bytes are code.")


if __name__ == "__main__":
    main()
