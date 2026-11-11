# Source provenance and credits

Component notices and copyright lines remain in their original source files. Unless a component says otherwise, kennel is GPL-3.0-or-later with the OP-1 firmware permission in `NOTICE`. Source-file hashes are in the archive’s `PROVENANCE.json`; upstream revisions below identify imported components, not an included Git history.

| Component | Origin | Revision / license |
|---|---|---|
| Runtime, dispatcher, engine/FX infrastructure, sequencers and screens | pcppcp and contributors, [op1r-src](https://github.com/pcppcp/op1r-src) | Foundation `175ca8c60ccb9de3daa40a1e7fd2042b25af95fd`; op1r core/Musica Obscura merge `df30df0`; GPL-3.0-or-later with OP-1 permission |
| Firmware patcher | [pcppcp/op1r](https://github.com/pcppcp/op1r) | `6b77bf3fe2a0a006dc79750b34c03c9ba8d8fe01`; original LICENSE/NOTICE retained |
| Mixtur, Arc, Palme, In a room, Oracle and Peal | ultralazr, Musica Obscura | Imported through op1r; original source notices apply |
| Chord, Scale, utility implementations, loader, update integration and core services | Hundekraft | GPL-3.0-or-later with OP-1 permission |
| EVA | Hundekraft synthesis and integration; astronaut artwork supplied by the project maintainer | Artwork supplied as `astro-1.svg`; ownership recorded by the maintainer. Generator converts the supplied vector into the plugin screen |
| HAIL | Mutable Instruments Beads by Émilie Gillet; fixed-point C port, screen and OP-1 integration by Hundekraft | eurorack `f4cfa7697095a28d292bff758beb51deb49b5a86`; Copyright 2017 Émilie Gillet, MIT, `LICENSE.beads`. Shipped under the distinct name HAIL |
| Clouds / Reverb DSP | Mutable Instruments Clouds / Elements by Émilie Gillet, via op1r | Original MIT notices retained; Hundekraft processing/integration changes use kennel’s terms |
| Reverb and Aether artwork | DAN76; DAN76/Rackes for Aether | Imported with op1r source and its notices |
| Wavetable, Taffy, Pew, Elg, Washer, Snowglobe and MBIRA | Hundekraft | Original engines/effects and SDK adapters; GPL-3.0-or-later with OP-1 permission |
| Oxbow | zvenson’s [OberVave](https://github.com/zvenson/obervave), adapted by Hundekraft | Upstream `a2fb3d0`, GPL-3.0-only. The project maintainer reported zvenson’s permission on 2026-10-15 to use the code under kennel’s GPL-3.0-or-later + OP-1 permission terms; no written copy is linked in this bundle |
| SID | Hermit (Mihaly Horvath), cRSID 1.58 light renderer; OP-1 integration by Hundekraft | Original permissive notice retained. Source: `https://hermit.cucumalac.hu/cRSID-1.58-with-source-code.tar.gz` |
| C64arp | Hundekraft; musical reference to C64 relative pitch tables | Original implementation; references in the plugin README |
| Crush, Warble and Plate | Reconstructions of Maiko Pruett’s #250 custom algorithms/artwork | Reconstruction authorized by the project maintainer on 2026-10-16. Source headers and individual plugin READMEs retain the attribution; no TE generic screen/font is included |
| Father | xylograph’s Father concept/room demonstration; separate original reverb DSP and vector reconstruction by Hundekraft | No xylograph or Teenage Engineering DSP source is included. Reference: `https://www.reddit.com/r/OP1users/comments/1x422ip/father_a_stereo_algorithmic_reverb_running_on_the/` |
| op1p loader | pcppcp/op1r-src | `a04e3bcb23a6f5a81f1fe56468cf976327cc7093`; original notices retained |
| MIDI Tape / CC automation | pcppcp/op1r-src, integrated through kennel services | Initial recorder `5c90efb4e6f15260bdf672a0a7bb6dde9f5625d4`; subsequent import `7cf11afc2bade31f0fb08d3af877b9bb04f970bd`; GPL-3.0-or-later with OP-1 permission |
| Aether, Sampler+, sound repair and other op1r additions | pcppcp/op1r-src | `7cf11afc2bade31f0fb08d3af877b9bb04f970bd`; packaged documentation `pcppcp/op1r` `bd87d3653979f74929ec14b3b1b11efae56b3224`; original copyright lines retained |
| Randomiser | Lewis-Clayton / pcppcp; kennel integration by Hundekraft | Imported from op1r, then expressed as a native kennel feature |
| Gin | SquareWave Studio; libpo32 by Eric Lewis; kits adapted from CNTR RNDM | `joseph-holland/op1r-src` `ec5d6f722df05b77f80706db239352992937f4d7`, linked by Gin 1.0 release `1a6385a4a2ba76bab3758eecf1bb289c9c09bd94`. Native host/editor GPL-3.0-or-later + OP-1 permission; synthesis/libpo32 MIT. All vendor notices, GCC runtime notices, preset credits and permission records remain in `plugins/gin/vendor/` |
| Amp | Hundekraft; workflow inspired by Teenage Engineering’s OP-1 Field | Original input-strip DSP, tuner integration and artwork; no TE Amp DSP/artwork is copied |

## Utility and firmware boundaries

Maiko’s #249 utility code is not imported. Scale, naming, FILE, flash check, undo and tape tools are Hundekraft implementations informed by documented behavior; see `docs/maiko-rebuild.md`.

The source bundle contains no complete Teenage Engineering firmware, TE fonts, device keys, NAND images or extracted stock asset files. `synth_sdk/l1_patches.json` contains checked differences for encrypted R246 code, as used by op1r; these differences are not covered by the GPL. Read `NOTICE`.

Gin’s vendored standalone source includes its original integration documents and source receipts. Its standalone build layout differs from the kennel integration. Kennel-side integration, provider-scoped saves, cache invalidation and guarded native drum callbacks are Hundekraft changes unless their source header credits upstream.
