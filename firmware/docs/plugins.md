# coop plugins (Phase 4)

The plugin system of the plan of record: coop's own stereo ABI, kennel 3.1's file format and loader, a porting guide
for kennel and opus plugins. This document is the working record; `sdk/` holds the public headers.

## Milestone P1: loader + FX host (2026-11-03) [I until the device test]

**What exists.**
- `sdk/coop_plugin.h`: ABI version 1, effects only. An FX plugin exports one `coop_fx_t` (`magic` "COFX", `api`,
  `flags`, `init(slot)`, `render(slot, l, r, params, n)`): the master bus as two int32 planar buffers (the int16
  bus widened by 16 bits), 64 samples in place, four Q15 knobs in `params`.
- File format: kennel's plugin file (`third_party/kennel/docs/plugin-format.md`) with the magic `COOP` and
  `api_version` = `COOP_PLUGIN_API`; everything else byte-compatible, so kennel's packer and relocation checks
  apply unchanged. `tools/plugin/pack.py` + `reloc.py` + `merge.ld` are kennel's tools adapted (magic, the coop
  SDK and `core/include` on the include path, C++ sources through `bfin-elf-g++`, `<id>.coop` output).
  `plugins/Makefile` packs every `plugins/<id>/plugin.json` to `build/plugins/<id>.coop`.
- Loader: `core/sdram/plugin_load.cc/.h` are kennel's `kn_load.c/.h` with the magic swapped, compiled as C++.
  `core/sdram/plugin_host.cc` lists `/yaffs2/plugins` at boot (from the tape mount hook, main thread), reads each
  `.coop` whole into a 1 MiB staging block from heap 0, parses and checks it, relocates it into its own heap-0
  block (aligned 32, never freed), checks the entry object (magic, api, callbacks inside the plugin's code), calls
  `init`, and then publishes it to the audio thread. Any failure skips the file with a reason; the stats file's
  `plugins` line carries `seen / loaded / failed` and the last refused file's name and reason.
- Host: the master insert slot (`master_suite.cc`, image flag bit 0) runs the loaded plugin instead of the built-in
  stereo delay. P1 hosts one FX plugin (the first loaded); P2 brings slots, the parameter model and pages.
- Exports: `sdk/coop_exports.cc` is the append-only table of core symbols a plugin may import (empty in P1).
- First plugin: `plugins/stereo_delay` (the built-in delay's DSP from `core/include/coop_fx.h`, lines in its own
  .bss, 984 bytes packed, no imports). Its gate is bit-identity with the built-in delay.

**Building and testing.**

```sh
make -C core/sdram combined                     # the core (loader + host inside)
make -C plugins                                 # build/plugins/stereo_delay.coop
# an op1emu NAND with the plugin at /yaffs2/plugins (tools/make_nand.py uses op1emu's provision_nand.py):
mkdir -p build/nand_overlay/plugins && cp build/plugins/stereo_delay.coop build/nand_overlay/plugins/
python3 -I tools/make_nand.py --provision ~/op1emu-work/provision_nand.py --firmware ~/Downloads/op1_246.op1 \
    --mkyaffs2 ~/op1emu-work/yaffs2utils/mkyaffs2 --overlay build/nand_overlay --output ~/op1emu-work/plug_pristine.bin
```

Gate: `tests/sim/det_chord.txt` on an image with `g_coop_master_flags=1`, once on the pristine NAND (built-in
delay) and once on the plugin NAND (the file): the two dumps must be sample-identical, and the stats text must say
`plugins seen 1 loaded 1 failed 0`. **Passed 2026-11-03:** `plugins seen 1 loaded 1 failed 0`; the two dumps have the
same level and width (0.2155) and match within 10 LSB once aligned on the onset (the plugin NAND boots on a different
block boundary, docs/emulator.md); no fault. With no image flag the loaded plugin alone drives the slot, and the
pristine NAND stays transparent (`loaded 0`).

**On the device.** The SysEx channel writes files now (P1b, docs/sysex.md: PUT_BEGIN / PUT_DATA / PUT_END /
DELETE, the host page's Install button): `stereo_delay.coop` goes to `/yaffs2/plugins`, a power cycle loads it,
and a loaded plugin drives the master slot without any image flag, so the stock-transparent master suite stays
transparent until a plugin is installed. Package `op1_246_coop_p1_plugins.op1`.

## Next

- P1b: SysEx `FILE_PUT` / `FILE_DELETE` (kennel's devupd framing, 64-byte chunks) so plugins can be installed from
  `tools/coop_sysex.html` while the device runs; then the device test of P1.
- P2: FX slots (synth 0, drum 1, master 2 as positions), the core-owned parameter store with coarse/fine, pages,
  the knob bindings; saved sounds with the `"coop"` JSON member.
- P3: engines (`coop_engine_t`, 128 samples at 88.2 kHz, stereo or mono-with-flag), LFO and sequencer objects,
  screens through the stock SVG renderer; the L1 and big-buffer leases (`kn_l1.c` model).
- The porting guide (kennel 3.1 and opus to coop) and the first ports from `te-op1-cfw/ports`.
