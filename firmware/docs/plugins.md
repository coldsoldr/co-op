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

## Milestone P2: FX plugins at the stock positions (2026-11-03)

Kennel 3.1's mechanism (dispatcher.c, kn_boot.c), ported:
- **The filter type is hijacked.** The stock filter FX object's vtables (mono 0x01910C94 for the synth and drum
  positions, the stereo master wrapper 0x01910CB4; entry 2 render, entry 3 init) point at `coop_fx_render_mono /
  init_mono / render_st / init_st` (plugin_host.cc). The selector `params[4] = 0x5300 + n` names plugin n; with no
  plugin the stock filter runs as before.
- **Names in the stock FX list.** At boot every loaded plugin gets a `fx_types` row in the factory database (its
  type name = the id with spaces, `default_params` = the four knob defaults mapped into the filter's ranges, then
  the selector). The stock list shows the row; the fx_types name compare at 0x019AAD8E is replaced (a JUMP.L to a
  region-C trampoline -> `coop_fx_name_eq`) so a plugin's name resolves to the filter type, as does a saved sound
  naming a plugin that is gone (it loads as the filter instead of asserting).
- **Numbers persist**: a name -> number map in the user database (table `coopplugins`), files take 32..63; a file
  that disappears keeps its number marked absent, so saved sounds stay valid.
- **Positions.** 0 synth and 1 drum are mono in stock: the plugin gets L = R = the instrument's mono sum, its
  output's mono goes on down the stock path and its difference is added to coop's side buses
  (`coop_stereo_side_rw`), so a stereo effect can make width, but it does not yet see the instrument's per-voice
  placement (fixed in P2b below). Position 2 (master) is true stereo in place. One plugin instance per
  file: selected at two positions at once it shares its state (fixed in P2b below).
- **Knobs**: the slot's four knob words as stock keeps them, kept inside the filter type's ranges every block
  (kennel's guard; the stock parameter reader asserts outside them). The core-owned coarse/fine model is P2b.
- P1's master-insert path is retired (plugins play only when selected).

Emulator (2026-11-03, `tests/sim/det_fx_select.txt` on the plugin NAND): the stock FX list opens with shift + key 3 on the synth page, the blue encoder moves it (8 steps per item), the plugin row is last; selected, the mono dispatch ran 15,083 blocks through the plugin (`plugins ... mono_calls 15083 plugin_blocks 15083`), the chord played, no fault; the plugin page shows the stock filter screen until P3.

## Milestone P2b: per-instrument side buses, an instance per position (2026-11-03)

- `stereo_bus.cc` keeps one difference pair per instrument (synth, drum): the voice hooks accumulate into their
  instrument's pair, each pair has its own decimator, DC block and tail, and the mix loops and the record tap take
  the sum (`coop_sat16(synth + drum)`), built once per block after the inserts ran. Spread 0 and no plugin give the
  same samples as before (the sum of one live pair and a zero pair).
- `coop_stereo_side_rw(instr, &l, &r)` hands an insert its instrument's decimated pair (zeroed when nothing is
  panned there). The host builds the plugin's input as `L = sat(mono + dL)`, `R = sat(mono + dR)` and writes the
  output back as the new mono (`(l + r) / 2`, down the stock path) and the new difference (`l - m`, `r - m`), which
  replaces the instrument's side: a stereo effect's image survives, including echoes that follow the placement.
- One relocated instance per position: `plugin_host.cc` relocates each file three times (synth, drum, master), so a
  plugin selected at two positions at once keeps two states; memory is three times the file's load size (the
  stereo delay: 3 x 129 KiB of heap 0).

Emulator (2026-11-03): with no plugin the chord scenario is sample-identical to P2 and the record scenario to T4e (the split is transparent); with the plugin at the synth position (`det_fx_select.txt`) the echo tail after the release keeps its side energy (mid/side 189/45, 84/14, 64/4 per 200 ms against P2's 193/30, 85/5, 64/1): the repeats follow the placement now.

## Stereo bus v2 step S1: stock effects in stereo at the instrument positions (2026-11-03)

The owner found the P2b flaw on the device: a stock effect at the synth position ran on the mono sum while the
per-voice side signal bypassed it (`docs/stereo-bus-v2.md`). S1 makes every effect at an instrument position, stock
or plugin, run on the whole stereo pair:
- all eight stock mono FX vtables (render, init) point at `coop_fx_render_mono` / `coop_fx_init_mono`; the stock
  mono render no longer runs on the mono bus;
- a stock type is run as its stereo class (the master's), constructed by coop in its own memory on the position's
  init (`st_make`), with its own line for delay and cwo; a plugin runs as in P2b;
- the stats `plugins` line adds `stock_st_blocks` (blocks a stock stereo twin rendered) and `stock_st_made`
  (constructions); the build tag is `s1`.

Limits: the stock delay sums L and R into its line, so its echoes are centred (as on the master); grid and spring
share one fixed tank between an instrument position and the master (`docs/stereo-bus-v2.md`). The FX list of R246
has no filter row (cwo, delay, grid, nitro, phone, punch, spring, then the plugin rows); the filter type only carries
plugins.

## S2: the second effect slot (2026-11-03)

Every plugin is relocated five times now: synth, drum and master for slot 1 (P2b), plus instrument slot 2 and master
slot 2 (`docs/stereo-bus-v2.md`, S2). A plugin in slot 2 gets `init(3)` / `init(4)` and the same `render` call as at
slot 1, with the four knob words in the filter ranges and `params[4] = 0x5300 + n`. Memory: five times the load size
(the stereo delay: 5 x 129 KiB of heap 0); a plugin that no longer fits fails to load with "plugin memory full".

## Next

- P1b: SysEx `FILE_PUT` / `FILE_DELETE` (kennel's devupd framing, 64-byte chunks) so plugins can be installed from
  `tools/coop_sysex.html` while the device runs; then the device test of P1.
- S3 (built): the instrument bus is a true stereo pair from the voice hook (`docs/stereo-bus-v2.md`). One
  instrument effect runs on both instruments, as in the stock.
- S2: a second FX slot per instrument and on the master.
- Then: the core-owned parameter store with coarse/fine, pages,
  the knob bindings; saved sounds with the `"coop"` JSON member.
- P3: engines (`coop_engine_t`, 128 samples at 88.2 kHz, stereo or mono-with-flag), LFO and sequencer objects,
  screens through the stock SVG renderer; the L1 and big-buffer leases (`kn_l1.c` model).
- The porting guide (kennel 3.1 and opus to coop) and the first ports from `te-op1-cfw/ports`.
