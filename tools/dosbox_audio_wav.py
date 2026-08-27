#!/usr/bin/env python3
"""Turn SDL's disk-driver capture of DOSBox into a WAV.

dosbox-staging's mixer works in FLOAT and asks SDL for AUDIO_F32SYS, so the
raw is 32-bit float stereo - not the signed 16-bit a DOS emulator suggests.

Reading it as s16 does not fail, which is the trap. It produces a file whose
every five-second RMS is 18,900, flat to four digits for four minutes, and a
flat RMS is not music. One channel comes out as a slow waveform and the other
as noise, because that is what the two halves of a float look like.
"""
import sys, struct, array

RATE = 48000


def main():
    raw, wav = sys.argv[1], sys.argv[2]
    f = array.array("f")
    with open(raw, "rb") as r:
        b = r.read()
    f.frombytes(b[:len(b) // 4 * 4])
    if not f:
        sys.exit("no samples in %s" % raw)

    mono = array.array("h")
    for i in range(0, len(f) - 1, 2):
        v = (f[i] + f[i + 1]) * 0.5
        mono.append(int(max(-1.0, min(1.0, v)) * 32767))

    # Trim the run-up: the menus, the fade and the briefing are silent, and
    # the music does not start until the level is 55 frames old.
    start, step = 0, RATE // 10
    for i in range(0, len(mono), step):
        c = mono[i:i + step]
        if c and max(abs(x) for x in c[::7]) > 600:
            start = max(0, i - 2 * RATE)
            break
    mono = mono[start:]

    nb = len(mono) * 2
    with open(wav, "wb") as w:
        w.write(b"RIFF" + struct.pack("<I", 36 + nb) + b"WAVE")
        w.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, RATE,
                                      RATE * 2, 2, 16))
        w.write(b"data" + struct.pack("<I", nb))
        w.write(mono.tobytes())
    print("  %s  %.1f s, %.1f s of silence trimmed"
          % (wav, len(mono) / RATE, start / RATE))
    if len(mono) < RATE * 5:
        sys.exit("LESS THAN FIVE SECONDS OF SOUND - the run probably never "
                 "reached a level. Check dosbox.log before reading this as "
                 "a result.")


if __name__ == "__main__":
    main()
