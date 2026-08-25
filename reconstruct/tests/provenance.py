"""Every function must say where it came from.

CLAUDE.md: "Every function carries where it came from - the image offset if it
is transcribed, and an explicit 'not transcribed, address unknown' if it is
not. This is the convention that decays first: each omission is trivial and
the loss only shows up later, when a verifier disagrees and there is no way to
find the routine it disagreed about."

It decayed. This checks it, by looking at the comment block IMMEDIATELY above
each definition - not a window of lines, which silently inherits the address
of whatever came before and reports a function as transcribed when it is not.

  uv run python reconstruct/tests/provenance.py [--list]
"""
import pathlib
import re
import sys

SRC = pathlib.Path(__file__).resolve().parent.parent / "src"
DEF = re.compile(r'^(?:static\s+)?[A-Za-z_][A-Za-z0-9_]*\s+\**([a-z_][a-z0-9_]*)\s*\(')
ADDR = re.compile(r'\bimage\s+0x[0-9A-Fa-f]{4,5}|\b0x[0-9A-Fa-f]{4,5}\b')
# Three outcomes, and the third is the one that matters. "The port's own" and
# "not transcribed yet" were one bucket until it was noticed they are opposite
# claims: the first says the original has no such routine, the second says it
# has one and nobody has read it. Collapsing them hides every debt.
# Anchored to the start of a line, because a comment that MENTIONS the words
# in passing is not a classification. Matching them anywhere flagged dat_load,
# level_parse, render_terrain and render_objects - all of which carry an
# address and merely discuss what was and was not transcribed nearby. Same
# mistake as searching a window instead of the block: a loose pattern makes
# the file report something it does not say.
# Allow the marker on the line that OPENS the block, not only on a
# continuation line: `/* The port's own...` is the natural way to write a
# short one and the first version silently refused it.
LINE = r"^[ \t]*(?:/\*|\*)?[ \t]*"
OWN = re.compile(LINE + r"(?:The p|P)ort's own\b|" + LINE + r"THE PORT'S OWN",
                 re.M)
OWED = re.compile(LINE + r"NOT TRANSCRIBED\b", re.M)


def block_above(lines, i):
    """The contiguous comment directly above line i, and nothing else.

    A RUN of one-line definitions shares the comment above the run - four
    trivial accessors in a row want one explanation, not four copies of it, and
    demanding four is how a convention turns into ritual and then gets ignored.
    So walk back over immediately-adjacent definition lines first.
    """
    out = []
    j = i - 1
    while j >= 0 and DEF.match(lines[j]) and lines[j].rstrip().endswith("}"):
        j -= 1
    while j >= 0 and lines[j].strip() == "":
        j -= 1
    while j >= 0:
        t = lines[j].strip()
        if t.endswith("*/") or t.startswith("*") or t.startswith("/*") \
           or t.startswith("//"):
            out.append(lines[j])
            if t.startswith("/*"):
                break
            j -= 1
        else:
            break
    return "\n".join(reversed(out))


FILE_OWN = re.compile(r"THIS FILE IS THE PORT'S OWN")


def main():
    show = "--list" in sys.argv
    trans = own = bare = 0
    owed = []
    missing = []
    for f in sorted(SRC.glob("*.c")):
        text = f.read_text()
        lines = text.split("\n")
        # A file may declare itself the port's own once, at the top, instead of
        # repeating it on every function. sdl_io.c and main.c are boundaries
        # the PORT chose, not ones the binary proves, and there are no
        # addresses to carry; sixteen identical comments saying so would be
        # ritual, and ritual is what a convention decays into just before it
        # decays out.
        if FILE_OWN.search(text[:2500]):
            n = sum(1 for i, l in enumerate(lines)
                    if not l.startswith(" ") and not l.rstrip().endswith(";")
                    and DEF.match(l)
                    and DEF.match(l).group(1) not in ("if", "for", "while",
                                                      "switch", "return",
                                                      "sizeof"))
            own += n
            if show:
                print(f"  port's own   {f.name}: whole file, {n} functions")
            continue
        for i, l in enumerate(lines):
            if l.startswith(" ") or l.rstrip().endswith(";"):
                continue
            m = DEF.match(l)
            if not m or m.group(1) in ("if", "for", "while", "switch",
                                       "return", "sizeof"):
                continue
            ctx = block_above(lines, i)
            if OWED.search(ctx):
                owed.append((f.name, i + 1, m.group(1)))
            elif ADDR.search(ctx):
                trans += 1
                if show:
                    a = ADDR.search(ctx).group(0)
                    print(f"  transcribed  {f.name}:{i+1:5d}  "
                          f"{m.group(1):26s} {a}")
            elif OWN.search(ctx):
                own += 1
                if show:
                    print(f"  port's own   {f.name}:{i+1:5d}  {m.group(1)}")
            else:
                bare += 1
                missing.append((f.name, i + 1, m.group(1)))

    print(f"\n  transcribed, with an address : {trans}")
    print(f"  the port's own, said so      : {own}")
    print(f"  not transcribed YET          : {len(owed)}")
    print(f"  NEITHER                      : {bare}")
    if owed:
        print("\n  game logic the original has a routine for, not yet read:")
        for fn, ln, nm in owed:
            print(f"    {fn}:{ln:5d}  {nm}")
    if missing:
        print("\n  these say nothing about where they came from:")
        for fn, ln, nm in missing:
            print(f"    {fn}:{ln:5d}  {nm}")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
