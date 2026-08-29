#!/usr/bin/env python3
"""Every flag a tool's usage block hands to the tool it RUNS, checked.

Five usage blocks in this repository specified something that cannot work, and
each sat in the header of a check the port's claims rest on - see CLAUDE.md,
"A recipe is prose that looks executable, so nobody runs it". They are cheap
to find and impossible to notice by reading, because each fails as something
OTHER than itself: a percentage of zero, a FileNotFoundError naming a socket,
a usage message that reads as the reader mistyping.

    uv run python tools/check_recipes.py
    uv run python tools/check_recipes.py --self-test   # the control

It reads tool docstrings, the `.sh` headers, CLAUDE.md, STATUS.md and docs/.
The shell scripts were missed on the first pass and the gap widened the moment
another `.sh` with a usage block was added - a recipe is no safer for being in
`#` than in a docstring.

TWO RULES MAKE THIS USABLE and without them it is noise. The first version
reported 71 findings and about 69 were its own fault:

  - stop at `--`. Everything after it goes to the EMULATOR, not the script,
    and `--run-seconds`, `--shots`, `--scale` are all its.
  - only attribute flags to a tool named after `python`. A line beginning
    `LEM_HOOK=tools/probe_opl.py ... python tools/play.py --auto` runs
    play.py; probe_opl.py is a hook and owns none of those flags.

It cannot see everything, and says so rather than implying otherwise: it
checks that a flag EXISTS, not that its value is sensible. `--shots 1` parses
and ends the run after 1.6 seconds; `--run-seconds 90` parses and stops short
of the music. Those cost a day between them and no parser would have caught
either.
"""
import argparse
import ast
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def flags_of(src):
    """The --options a source file's argparse actually defines."""
    return set(re.findall(r'add_argument\(\s*"(--[a-z0-9-]+)"', src))


def findings(docs):
    """(file, tool, flag) for every flag given to a tool that lacks it."""
    out = []
    for path, doc in docs:
        for line in doc.splitlines():
            m = re.search(r"python\s+tools/([a-z0-9_]+)\.py(.*)$", line)
            if not m:
                continue
            target, rest = m.group(1), m.group(2)
            rest = rest.split(" -- ")[0]          # the rest is the emulator's
            tp = os.path.join(HERE, target + ".py")
            if not os.path.exists(tp):
                continue
            with open(tp) as f:
                tf = flags_of(f.read())
            for flag in re.findall(r"(--[a-z0-9-]+)", rest):
                if flag not in tf:
                    out.append((os.path.basename(path), target, flag))
    return out


def read_docs():
    """Every place a recipe lives: tool docstrings AND the markdown.

    CLAUDE.md is the most-read file here and carries commands of its own -
    it is where the `--scale` separator trap is written down, having once
    been the thing that file told you to do. STATUS.md and docs/ quote
    commands too. A recipe is no less copied for being in prose.
    """
    docs = []
    for path in sorted(glob.glob(os.path.join(HERE, "*.py"))):
        with open(path) as f:
            src = f.read()
        try:
            doc = ast.get_docstring(ast.parse(src))
        except SyntaxError:
            continue
        if doc:
            docs.append((path, doc))
    root = os.path.dirname(HERE)
    md = (glob.glob(os.path.join(root, "*.md"))
          + glob.glob(os.path.join(root, "docs", "*.md")))
    for path in sorted(md):
        with open(path) as f:
            docs.append((path, f.read()))
    # AND THE SHELL SCRIPTS. tools/verify_all.sh, prove.sh, record_skills.sh
    # and record_adlib_ref.sh all carry usage blocks in their comment headers,
    # and a recipe is no safer for being in `#` than in a docstring. This was
    # missed on the first pass and the gap widened the moment another .sh with
    # a usage block was added.
    for path in sorted(glob.glob(os.path.join(HERE, "*.sh"))):
        with open(path) as f:
            docs.append((path, f.read()))
    return docs


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true",
                    help="check that a planted bad recipe IS caught, which is "
                         "the only thing that makes a clean run mean anything")
    a = ap.parse_args()

    docs = read_docs()
    if not docs:
        print("  no sources read at all - that is a failure, not a pass")
        return 2

    if a.self_test:
        # THE CONTROL. A checker that finds nothing and has never been shown
        # to find anything is indistinguishable from one that cannot.
        planted = [("PLANTED", "uv run python tools/play.py --auto "
                               "--no-such-flag")]
        bad = findings(planted)
        ok = any(f == "--no-such-flag" for _, _, f in bad)
        print(f"  planted recipe caught : {'yes' if ok else 'NO'}")
        # and a correct line must NOT be reported
        clean = [("PLANTED", "uv run python tools/play.py --auto -- --shots 6")]
        quiet = not findings(clean)
        print(f"  correct line ignored  : {'yes' if quiet else 'NO'}")
        print(f"  ({len(docs)} sources would be scanned)")
        return 0 if (ok and quiet) else 1

    bad = findings(docs)
    print(f"  {len(docs)} sources scanned (tool docstrings, the .sh\n  headers, CLAUDE.md, STATUS.md and docs/)")
    print(f"  flags handed to a tool that does not define them: {len(bad)}")
    for f, t, flag in sorted(set(bad)):
        print(f"    {f:24s} runs {t}.py {flag}")
    if bad:
        print("\n  A usage block that cannot run is worse than none: it is\n"
              "  copied. See CLAUDE.md on recipes.\n")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
