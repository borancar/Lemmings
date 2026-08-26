"""An input script that assigns ONE skill, for the side-by-side.

Image 0x03F07 takes the panel button from (mouse_x - 8) >> 4, so button k
sits at screen x = 8 + k * 16, and image 0x03180 dispatches 2..9 to the eight
skills. The digger is 9; the other seven are what "Just dig!" never stocks,
which is why both sides get the --skills poke.

  uv run python tools/make_skill_script.py --button 5 --out out/s5.txt

The lemming's position comes from a plain recording's manifest, so the frame
numbering is the one the comparison uses - working it out from a tool with
its own frame origin is how a scripted click lands on empty sky.
"""
import argparse, json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--button", type=int, required=True,
                    help="2..9; 9 is the digger")
    ap.add_argument("--at", type=int, default=110, help="the frame to click on")
    ap.add_argument("--pos", default=os.path.join(ROOT, "out", "playpos"),
                    help="a recording whose manifest carries lem0")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    man = json.load(open(os.path.join(a.pos, "manifest.json")))
    row = next((r for r in man if r["frame"] >= a.at and r.get("lem0")), None)
    if row is None:
        sys.exit(f"  no lemming was out at frame {a.at}")
    o = row["lem0"]
    # Aim at where the lemming is DRAWN - the sprite goes down at canvas
    # (x + hot_x - 16, y + hot_y - 16) and the canvas is shown from
    # scroll - 16, so its top-left is (x + hot_x - scroll, y + hot_y - 16).
    sx = o["x"] + o["hot_x"] - row["scroll"] + 8
    sy = o["y"] + o["hot_y"] - 16 + 7
    bx = 8 + a.button * 16          # image 0x03F07
    at = row["frame"]
    open(a.out, "w").write("\n".join([
        f"# button {a.button} on lemming 0 at frame {at}, written by",
        "# tools/make_skill_script.py - read by record_level.py --script and",
        "# by the port through LEM_INPUT_SCRIPT.",
        f"20 move {bx} 190", "22 down", "34 up", "",
        f"{at} move {sx} {sy}", f"{at + 2} down", f"{at + 14} up", "",
    ]))
    print(f"  button {a.button} at screen x {bx}; lemming 0 at ({sx}, {sy}) "
          f"on frame {at} -> {a.out}")


if __name__ == "__main__":
    main()
