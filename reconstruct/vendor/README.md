# Vendored third-party code

Nothing in this directory is reconstructed from `vgalemmi.exe`, and nothing in
it should be read as part of the port. It is here so the build has no network
dependency and so the exact version can be named in a measurement.

The rest of the reconstruction carries a provenance header naming the binary
and **no** licence header, because a file transcribed out of someone else's
program is not ours to licence. That reasoning does not apply here: this code
has a known author and a known licence, and both are kept intact.

## ymfm

    upstream  https://github.com/aaronsgiles/ymfm
    revision  81aec25ccbb98f4873a255f7551ac4dadac59b4a
    licence   BSD 3-Clause, (c) 2021 Aaron Giles - see LICENSE
    author    Aaron Giles

Yamaha FM chip emulation. The port uses exactly one chip from it, the
**YM3812 (OPL2)**, which is what an AdLib card carries - see `docs/sound.md`
for how that was established from `adlib.dat`'s own bytes rather than assumed.

Only the files the OPL family needs are vendored:

    ymfm.h ymfm_fm.h ymfm_fm.ipp        the core and the FM engine
    ymfm_opl.h ymfm_opl.cpp             the OPL chips, YM3812 among them
    ymfm_adpcm.* ymfm_pcm.* ymfm_ssg.*  units ymfm_opl.cpp refers to for the
                                        Y8950 and YMF278B, which this port
                                        does not use but cannot compile without

**Why this is not transcribed.** An OPL2 is *hardware*. It is not a routine in
the game, so there is nothing in the image to read it out of, and the rule
that the port prefers transcribing to writing has nothing to apply to. It sits
on the same side of the line as SDL: something the port needs because the
machine the original ran on is gone.

The line it must not cross is into the game. The **driver** - `adlib.dat`'s
sequencer, which decides which registers get written and when - IS
transcribable, is checked against the original's own register stream, and
belongs in the port proper. ymfm's job stops at turning register writes into
samples.

To update: re-copy the files above from a newer upstream revision, change the
revision line here, and re-run the OPL checks. Do not edit the sources in
place - a local change to vendored code is invisible to the next reader and
survives no update.
