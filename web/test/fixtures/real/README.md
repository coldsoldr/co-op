# Real OP-1 device files

These files were copied unchanged from an OP-1 in disk mode and are used by
`web/src/core/codec/patch.test.ts` for byte-for-byte round-trip tests.

| File | Kind | Name in metadata | Sample rate | Container |
| --- | --- | --- | --- | --- |
| `synth.aif` | synth (sampler) | realistic-cello-4545 | 22050 Hz | AIFC, sowt |
| `drum.aif` | drum | rlnd_tr808-7 | 44100 Hz | AIFC, sowt |

Provenance: recorded and contributed by the repository owner (Byron Jacobs),
who confirms they are their own recordings and may be redistributed under
the repository's MIT licence.

Treat this folder as append-only. Replacing a file leaves the old bytes in
git history, so pick small files if more are ever added.
