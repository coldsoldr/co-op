# kennel

A loadable-plugin runtime for the original Teenage Engineering OP-1, based on R246 and pcppcp’s [op1r-src](https://github.com/pcppcp/op1r-src). It includes contributions from ultralazr’s Musica Obscura and Hundekraft.

Synth, effect and feature files register at boot. Settings are built into the core. Install or remove plugins through the workshop; the ordinary OP-1 disk-mode volume is not the plugin installation directory. Code replacement takes effect after restart.

## Plugin development

Read `../../PLUGIN_GUIDE.md`. Start with `synth_sdk/template/synth` or `synth_sdk/template/fx` and use `tools/kennel/pack.py`. A standalone `.knl` requires Python and a Blackfin compiler, but no original firmware or complete firmware build.

Match the folder, manifest ID and output filename. New synth/FX files use order 0; legacy orders are reserved. Check API declarations and `synth_sdk/kennel/kn_exports.c` against the target firmware.

## Layout

- `synth_sdk/`: dispatcher, firmware hooks, public API headers, screens and plugins.
- `synth_sdk/plugins/`: plugin implementations, metadata, artwork and component notices.
- `tools/kennel/`: packer, relocation tools and format validation.
- `docs/`: runtime and SDK references.
- `PROVENANCE.md`: source origins, authors and licensing.

For a complete firmware build, use `../../build.py` and the instructions in the archive README.

## License

GPL-3.0-or-later with the OP-1 firmware permission inherited from op1r-src. Read `LICENSE` and `NOTICE`; component-specific notices remain in their directories.
