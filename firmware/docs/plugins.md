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

Limits: the stock delay sums L and R into its line, so its echoes are centred (as on the master). Grid and spring
shared one fixed tank between an instrument position and the master until the clean-ups after S2
(`docs/stereo-bus-v2.md`). The FX list of R246
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
- P3: engines: started 2026-11-09, `docs/engines.md` (E1 the host and the reference engine `saw`, E2 plugin screens,
  E3 rings). Later: LFO and sequencer objects; the L1 and big-buffer leases (`kn_l1.c` model).
- The porting guide (kennel 3.1 and opus to coop) and the first ports from `te-op1-cfw/ports`.
- Updates without reflashing (the owner, 2026-11-04; planned after the P3 screens). Plugins already install over
  SysEx. The coop core itself is linked into the firmware image, so today every core change is a flash. Plan: a small
  stable loader flashed once, which loads the core from a file on the OP-1's disk at boot (integrity check, and a key
  held at boot skips the file: `docs/brick-safety.md` first). The web app then installs the core as it installs
  plugins; only a change to the stock hook sites still needs a flash. Opus 2 works this way (a patched core, then
  plugin files), and its patcher builds the package in the browser from the owner's own stock file. [I]
  hundekraft 3.1.4 does it another way: its USB update (kennel `devupd/du_fw.c`) sends a binary patch made against
  the firmware already installed (`/yaffs2/OP1_vdk.ldr`, checked by size and CRC32); the OP-1 applies it, checks the
  result is an R246 OP1_vdk, writes it to flash itself with a journal and rollback, and only then reboots. So the
  owner needs the `.op1` once, then only a cable. coop already has a SysEx file channel, so either route fits; the
  loader keeps flash writes rare, the patch route keeps one image. [I]
- Lessons from the other two firmwares for P3 (from their sources and docs, not measured here) [I]:
  - hundekraft (kennel 3.1): hand-written Blackfin assembly for the heavy inner loops (31 kernels), and plugins that
    use the iter voice objects and the L1 scratch beside them directly.
  - Opus 2: an L1 data allocator (about 25 KB for all plugins); an audio pool the display does not share (ordinary
    pool memory reads about twice as slow while a frame goes out); about 107 cycles per 32-byte line an audio block
    touches outside L1; stock effect inits off the audio thread (grid's takes 1.65 M cycles); every plugin call
    timed, a measured budget per plugin, and a plugin over it switched off.
  - Neither has a stereo voice path or 32-bit working values; coop keeps those.

## C++ ports and the packer (2026-11-10, the rings port)

- `tools/plugin/pack.py` links with `ld -r --force-group-allocation`, as te-op1-cfw's `knl_pack.py` does: C++ inline
  variables, out-of-line inline functions and template instances sit in COMDAT groups, which a relocatable link
  otherwise keeps as sections of their own (`.text._ZN...`), and the packer refuses those. saw and stereo_delay pack
  byte-identical with and without the flag.
- Globals whose type has a constructor (kennel's `FFloat` members, any class with default member values) give the
  plugin a `.ctors` section, which the packer refuses (no static constructors, guidelines §2.1): keep such objects
  as raw `.bss` storage and set them up in `init`, as rings does for its voice states and reverb.
- The packer runs the compiler quietly: compile a port's sources once by hand with the packer's flags to see its
  warnings (`plugins/rings/README.md` lists the flags through `plugin.json`).
- Port notes for rings: `plugins/rings/README.md`, `docs/engines.md` E3.

## Performance rules for plugins

Every plugin follows `guide/plugin-guidelines.md` (the contract: controls, parameters, screens, stereo, porting) and `docs/plugin-performance.md` (the SDK guide: the block budget, the per-sample loop rules, how a
plugin is measured with the stats file's cost line). A plugin over 60 k cycles a block is a bug to fix before release.

## p6k (2026-11-08): the built-in stereo delay is gone

Milestone B's demo slap echo in the master suite (`g_coop_master_flags` bit 0, the line at 0x02d00000) was removed
at the owner's word; bit 1 (the master suite) remains. The P1 gate above ("bit-identity with the built-in delay")
is history: `plugins/stereo_delay` is now the reference plugin on its own, gated by its op1emu WAV. The master's
second slot (S2) is the way to an effect on the master bus. In the same package the SDK header describes the
`params` the host really passes (range units, `sdk/coop_plugin.h`), and `HELLO` reports the package tag plus a
capabilities byte (`docs/sysex.md`).

## Effect controls (e6, 2026-11-11): fn + turn, its fine, clicks and latches at every position

Effects get the declared controls engines have had since e4 (`docs/engines.md` "Controls"), with two additions the
eleven kennel effect ports need: a **fine** pairing for fn + turn, and **latches**. The owner's decisions are in
`docs/ui-features.md` §14, the rules in `guide/plugin-guidelines.md` §3, §4, §4.1. Built and gated in op1emu; the
device test is pending (`docs/device-tests.md` "e6").

**Declaration** (`plugin.json` "controls", up to eight; `tools/plugin/pack.py` checks it and packs extension tag 6):

| Field | Saved control (`steps`, `continuous`) | Latch (`latch`, effects only) |
|---|---|---|
| `id`, `name` | as e4: id `[a-z0-9_]`, name 1-8 stock glyphs, lower case | same |
| `action` | `click`, `shift_click`, `fn_turn`, `fn_click`, `shift_fn_turn`, `shift_fn_click` (a continuous one: turns only) | a click action |
| `encoder` | `blue`, `green`, `white`, `orange` | same |
| `word` / `bit` | `word`: 5, 6 or 7, one control each | `bit`: 0..15 of `params[8]`, one latch each; no `word` |
| `default` | step index / Q15 (0..32767) | none (a latch starts off) |
| `steps` | `steps`: 2..16 names (1-8 glyphs) | none |
| `detents`, `wrap` | as e4 / e5 (steps) | none |
| `fine` | `true` on a continuous `fn_turn`: shift + fn + turn on that encoder is its fine; nothing else may sit there | none |
| `step` | continuous only: Q15 a detent on the control's own action, 1..32767 (default 328 = 1 %) | none |
| `fine_step` | with `fine` only: Q15 a detent on shift + fn + turn, 1..`step` (default `step` / 16, remainder carried) | none |
| `random` | default true: randomise draws it (steps within the count, continuous 0..32767); `false` keeps it (input gain) | never drawn (`random: true` is refused) |

Worked examples (what the ports declare):

```json
{"id": "gain", "name": "gain", "action": "fn_turn", "encoder": "blue", "word": 5, "kind": "continuous",
 "default": 16384, "fine": true, "step": 1365, "fine_step": 273, "random": false}
{"id": "mode", "name": "mode", "action": "click", "encoder": "green", "word": 6, "kind": "steps",
 "steps": ["shift", "ring"], "default": 0}
{"id": "hold", "name": "hold", "action": "click", "encoder": "green", "kind": "latch", "bit": 0}
```

**What render receives.** An effect with controls gets the core's params block, at least 16 words
(`COOP_FX_PARAMS_MIN`): `params[0..4]` as before (the four turns in the filter type's range units, the selector);
`params[5..7]` the three words, clamped by the core to each control (step index 0..n-1; Q15 0..32767 for continuous;
0 where nothing is declared); `params[8]` (`COOP_FX_LATCH`) the latch bits (bit b = the latch with `"bit": b`, 1 =
on); `params[9..15]` 0. A continuous control moves `step` Q15 a detent (default 328, 1 %) and its fine `fine_step`
(default `step` / 16 with the remainder carried: 20.5 as 20 and 21 for the default; 1,600 detents across the range);
both clamp at 0 and 32767. A steps control moves one step per `detents` detents and a click steps once, wrapping.

**Input gain, the owner's steps (2026-11-11):** kennel's ~0.5 dB a detent on fn + turn and ~0.1 dB on shift + fn +
turn. The ports' gain word is Q15 across +-6 dB (GainQ14: -6 dB at 0, 0 dB at 16384; tape_drive's (w - 16384) x 4 Q16
octaves, also +-6 dB), 2730.7 Q15 a dB, so declare `"step": 1365` (0.4999 dB) and `"fine_step": 273` (0.09998 dB):
88 coarse detents or 120 fine ones from the bottom to the top. `step` and `fine_step` work on engines too. A file that
declares neither packs byte-identically to e6 before them and turns by 328 / (328 / 16) as before. They pack as a 12th
and 13th field after the flags (`... name <empty> flags step fine_step`, 0 = the default); a core that predates them
(an earlier e6 dev build) ignores the rest of the line.

**What a port must do:**
- Read `params[5..8]` once at the block start and use those values for the whole block (a turn or click lands between
  blocks; a latch flips at a block boundary).
- Clamp what it reads, even though the core clamps: a stored word is whatever a sound holds.
- Treat a latch as state that may be cleared under it: the core clears a position's latches whenever the effect's
  `init` runs there (selected, a sound change, and the stock switching between synth and drum: it restarts the
  instrument effect, so a freeze at the synth or drum position is off after a visit to the other instrument; the owner
  accepted this on 2026-11-11, `docs/ui-features.md` §14) [C op1emu].
- Declare `"random": false` on a control a random draw must not move (input gain: a level jump); everything else
  saved is drawn by randomise.
- Not keep its own copy of a saved word across `init`; not write `params`; not read past `params[15]`.
- Declare `defaults` for the four turns as before; the saved words' defaults come from the controls (the packer sets
  the page-2 defaults from them; never write `page2_defaults` next to `controls`).

**Where the words live.**
- Slots 0 (synth) and 1 (drum), FX 1: the sound's stock FX words 5..7 (`0xFF801444 + 16 * inst`, `0xFF8015C4 +
  16 * kit`), written live and then through `Patch::SetFxParams` (as the randomiser does), so they are saved in the
  sound's `fx_params[5..7]`. The filter type's words 4..7 accept 0..32767 at the boot check [C op1emu: the e6 probe
  `filter_ranges` printed `[0,32767,0]` for each].
- Slot 2 (master), FX 1: `0xFF8016C4` words 5..7, then op1.db's master row through the stock's own writer
  (`master_db_save` 0x019D9B94, `symbols/sdram.sym`), from the main loop once the turns stop for ~0.5 s. Without it
  the words were lost at a power cycle [C op1emu].
- Slots 3 and 4 (FX 2): coop's slot record (`plugin_host.cc` g_s2 ext), saved in `/yaffs2/user/coop_slot2.bin` after
  the v3 record (`'X', 1`, then 6 int16 words). The version byte stays 3, so a core before e6 still reads the file.
  Choosing another effect in FX 2 starts its words at the plugin's defaults.
- The render takes slot 0-2 words from the shown sound's stored words when their selector equals the one the stock
  passed, else from the stock's params (`from_params` counts those; 0 in every e6 run).

**Compatibility.** pack.py sets the file's page-2 flag (`KN_FLAG_PAGE2`) for an effect with controls; a core before
e6 refuses such a file ("plugin flags not supported yet"), so a port never runs with zero words and no latch word.
A file without "controls" packs byte-identically to e5 (stereo_delay, saw, rings, probe_alu checked) and gets exactly
the params it got before (the stock's pointer at slots 0-2, the 8-word block at 3-4).

**Randomise** (the owner, 2026-11-11; `core/sdram/coop_random.cc`, `engine_ui.cc coop_fx_ui_randomise`): synth or drum
key + right arrow on the effect page draws the saved controls of the effect in focus, per control, as for engines:
steps within their count, continuous over 0..32767, skipped with `"random": false`; latches never. FX 1 (synth, drum):
with the four turns, as before, all written through `Patch::SetFxParams`. FX 2 of the instrument holding a plugin
(refused before e6): its four q15 knobs and its words, back with the left arrow as the other draws. The master has no
randomise (the key combination is the synth / drum key's). The stats count draws in `fxctl random`.

**Display.** On FX 1 the stock shows the filter type's page; on FX 2 coop's page. The core draws the control's name
and value top left in the encoder's colour for ~1.2 s over either (latches show `on` / `off`). A plugin screen for
effects is not built (the stock page is the filter's): showing the values permanently would take an effect screen
hook like the engines' case-14 swap (E2), which needs the filter page's screen slot found and swapped per position.

**Found and fixed on the way.** An FX 2 holding the stock filter (what an empty FX 2 takes when FX 1 is a plugin) is a
coop twin of the stereo filter class, whose init and render the package points at `coop_fx_init_st` /
`coop_fx_render_st`. Its construction reset the master plugin (`g_cur[2] = -1`: the master effect started again and,
with e6, lost its latches) and its render would have restarted the master plugin every block while it ran. The twin
now passes straight to the stock filter (`plugin_host.cc st_twin`).

**Stats** (`coop_stats.txt`, line `fxctl`): `fx` effects with controls, `turns`, `fine`, `clicks`, `latches`,
`ignored` (an undeclared layer), `cancelled` (a click that became a combo), `popup_frames`, `from_params`,
`master_saves`, and per position `pN=blocks i inits : w5/w6/w7/latch` (what the render was last given).

**Test plugin and scenarios.** `tests/plugins/test_fx` (gain on fn + blue with fine, polarity on the green click,
depth on fn + white, mute latch on the orange click; it multiplies by the gain, inverts, mutes).
`tests/sim/det_fx_controls.txt` walks the five positions; `tests/sim/det_fx_reload.txt` power-cycles the NAND it
leaves. Packer checks: `tests/tools/test_pack_controls.py`.

## Port: bitcrusher (2026-11-11, e6 controls)

te-op1-cfw's kennel bitcrusher (`ports/knl/bitcrusher`, 5aa314b; DSP `ports/fx/src/crush/crush.h`, coldsoldr) as
`plugins/bitcrusher/` (self-contained: `src/crush.h`, `op1/support.cc`). Notes in `plugins/bitcrusher/README.md`.
- Turns rate / bits / jitter / mix as kennel (`params[0..3]` normalised from the filter ranges in the render, rate
  and bits mirrored: clockwise = more). Kennel's blue page word 5 (input gain, -6..+6 dB) is the declared `fn_turn`
  `gain` on blue, `"fine": true` (shift + fn + blue), `"random": false`. No other presses upstream; no latch.
- The stereo core runs at every position (kennel: stereo at the master only). Q31 buffers: the crusher reads the top
  16 bits, the dry side keeps the low bits; bit-identical to upstream << 16 (`tests/host/bitcrusher`, 40,000 blocks,
  0 mismatches) [C host].
- Do not set plugin.json `"stereo"` on a coop effect: the loader then checks kennel's `op1_fx_stereo_t` layout
  (callbacks at +0, +4, +12) and a `coop_fx_t` fails it ("stereo callback outside plugin text") [I: read in
  `plugin_load.cc`, not run].
- 1,672 bytes packed, load 1,356 (x5 = 6,780); simulator compute 5.3 k / 6.7 k cycles a block avg / max [C simulator];
  device pending. op1emu gate pending (plan in the README).

## Port: drive (kennel effect, 2026-11-11) [C host + GNU simulator; op1emu gate and device pending]

`plugins/drive/` (README there): te-op1-cfw's oversampled soft-clip overdrive (`ports/fx/src/drive`, commit 9ced415;
the kennel plugin `ports/knl/drive` 0.2), self-contained: `src/drive_dsp.{h,cc}`, `src/drive_bfin.S` (six passes,
`tools/asm_lint.py`: 0 flagged), `src/drive_tables.h` (generated by `gen_drive_tables.py`), `op1/support.cc`,
`LICENSE-MIT` (Clouds' halfband coefficients).
- Controls: the four turns as kennel's (drive, tone, bias, mix; `params[0..3]` normalised from the filter type's range
  units as `op1_fx_param_q15` did); kennel's blue page-2 word 5 (input gain, `knl::GainQ14`) is fn + blue with
  `"fine": true`, `"random": false`. Nothing else upstream had a press. No latch. No `"stereo"` flag (a `coop_fx_t` is
  stereo by ABI; the flag means kennel's `op1_fx_stereo_t` layout and the loader would refuse the file).
- What went: the constexpr double maths (tables generated as integers, checked entry for entry against upstream's
  constexpr ones), `knl::Fine` (the stock shift byte 0x01DD7F78; the core's fine replaces it), the filter range table
  read at 0x01DE5C50 (a constant row), the mono split and the fused mono wet kernel (coop has no mono position), the
  tests-only no-oversampling path, the DriveParams default member initialiser (a static constructor otherwise).
- Stereo: upstream's packed path (L low halves, R high, one dual MAC) at every position; no L == R shortcut.
- Size: 9,608 bytes packed, load 9,948 (x 5 = 49,740 bytes of heap 0); no imports; `.bss` 1,584.
- Tests (`make -C tests/host/drive all sim`): the plugin's render on the int32 bus against upstream's DSP behind
  kennel's stereo glue over 20,000 random blocks (knobs jumping, sweeping, out of range; gain word changing; re-inits),
  bit-identical on the host (C passes) and in bfin-elf-run with the device build (assembly passes) against upstream's
  C; mix 0 = the input bit for bit. Simulator cost: 5,539 a block still, 7,578 avg / 8,042 peak moving (upstream's own
  figure 4,651 / 5,009 stereo; kennel ran 3,235 / 3,605 mono at the instruments).
- op1emu gate: not run (one gate at a time). Plan in `plugins/drive/README.md`: `tests/sim/det_fx_drive_select.txt`
  (new: drive is row 2 of the alphabetical FX list, not the last row), `det_fx_controls.txt` with its list turns
  adjusted, `det_fx_reload.txt`, a pristine `det_chord.txt`.

## ensemble (2026-11-11): the kennel ensemble on e6

`plugins/ensemble` (0.3): te-op1-cfw's kennel ensemble (`ports/knl/ensemble` 0.2) with its DSP copied into the plugin
(`src/`: the core of `ports/fx/src/ensemble`, Clouds' halfband path, `half_rate.h`), the upstream's constexpr double
tables turned into integers (`gen_tables.py`), C kernels host-only, the assembly kernels renamed `Ens*`. Controls: the
four turns as kennel (rate, depth, voices, mix; normalised from the filter ranges as `op1_fx_param_q15` did, the
ranges as constants), fn + blue the input gain (word 5) with shift + fn + blue its fine (`"fine": true`, `"random":
false`); no latch. Kennel's `"stereo": true` is left out (coop's loader reads it as kennel's stereo layout). True
stereo at every position; one exact saving: a block with L == R and equal decimator histories copies the left
decimator's result. File 10,820 bytes, load 19,380 x 5. `tests/host/ensemble`: bit-identical to the upstream on the
host (C) and in bfin-elf-run (assembly), 12.8 k mean / 14.8 k max cycles a block in the simulator. Gate plan and the
scenario `tests/sim/det_fx_ensemble.txt`: `plugins/ensemble/README.md`. Device test: `docs/device-tests.md`
"ensemble 0.3".

## Port: freq_shift (kennel freq_shift 0.2, 2026-11-11) [I until the gate and the device test]

`plugins/freq_shift`: te-op1-cfw's frequency shifter / ring modulator after Mutable Instruments Warps (MIT,
`ports/fx/src/shift` at 5aa314b, the kennel plugin `ports/knl/freq_shift`), self-contained (`src/` the DSP and its
assembly, `op1/support.cc`, `LICENSE-MIT`). Contract, stereo and cost: `plugins/freq_shift/README.md`.
- **Controls** (the owner's layout): the four turns are kennel's knobs (shift, feedback, spread, mix), normalised from
  the filter type's ranges as kennel's `op1_fx_param_q15` did; kennel's blue page-2 gain is `fn_turn` blue word 5
  with `"fine": true` (shift + fn + blue its fine, `"random": false`); kennel's green page-2 mode (`ClickMode(word, 2)`)
  is the green click, steps `shift` / `ring`, word 6, saved, wrapping, randomised.
- **Removed from the kennel glue** (`common/knl_fx.h`): the shift byte read at 0x01DD7F78 (`knl::Fine`, now the core's
  fine), the range table read at 0x01DE5C50 (`op1_fx_param_q15`, now the constant ranges once a block), the instance
  in `op1_fx_scratch` (now `.bss`, one per relocation), `op1_fx_page` / `op1_fx_knobs` / the screen reads, the input
  meter, `memset` / `<cstring>` in the DSP. No `"stereo"` flag (kennel's layout; a `coop_fx_t` is stereo by ABI). The
  screen (`shift.svg`) is not carried: effect screens aren't built.
- **Stereo:** a Hilbert pair per channel instead of upstream's one pair on (L + R) / 2 (guidelines §2.2: no mono
  collapse); an L == R block runs one pair. Host test: bit-identical to two upstream mono instances (L at shift +
  spread, R at shift - spread), and with L == R to upstream's stereo path (`make -C tests/host/freq_shift`); the
  assembly equals the host C in the GNU simulator (`make ... sim`).
- **Cost** (GNU simulator, cycles a block): 9,762 avg / 11,013 max with L == R, 14,383 / 15,868 with L != R
  (upstream's stereo call: 6,585 / 7,187). op1emu and device pending. File 10,300 bytes, load 14,168 (x5 = 70,840).
- Gate plan (not run in the port): `plugins/freq_shift/README.md` "Gate plan", scenario
  `tests/sim/det_fx_freq_shift.txt`.

## tape_drive (2026-11-11): the kennel tape saturation, stereo at every position

te-op1-cfw's kennel effect tape_drive 0.3, ported as `plugins/tape_drive` 0.4. `plugins/tape_drive/README.md` has
the contract, the cost plan and the gate plan.
- **Controls.** The turns are saturate, wobble, age (mirrored) and mix. fn + blue is the input gain (word 5,
  `"fine": true`, not randomised), with its fine on shift + fn + blue.
- **The DSP** (`src/`: te-op1-cfw `ports/fx/src/tape` with its assembly kernels) is copied unchanged, except for
  the include paths and one default member initialiser (it would have given the static instance a constructor).

Notes for other ports:
- **No `"stereo": true`.** A `coop_fx_t` is stereo by ABI, and the loader reads that kennel flag as kennel's
  `op1_fx_stereo_t` layout.
- **int16 DSP on the int32 bus.** The DSP runs on the bus's high halves, and the output keeps the input's low halves
  (`wet16 << 16 | in & 0xffff`). So the high half is bit-identical to upstream, and at mix 0 the output is the int32
  input exactly.
- **Knob ranges.** `op1_fx_param_q15` becomes the SDK's constant ranges with kennel's formula: four divisions a
  block.
- **Status.** Host test `tests/host/tape_drive`: bit-identical, on the host and in the simulator. Simulator cost:
  9,061 cycles a block (upstream: 7,026 stereo, 4,128 mono). The op1emu gate and the device test are pending
  (`docs/device-tests.md` "tape_drive 0.4").

## tape_echo (2026-11-11): the kennel tape echo, an e6 effect port

`plugins/tape_echo` is te-op1-cfw's fixed-point TAPE ECHO as the kennel plugin `ports/knl/tape_echo` 0.2 ran it
(5aa314b): a three-head tape delay at 22.05 kHz behind Clouds' halfband filters, wow and flutter after Mutable
Instruments Beads (MIT, `plugins/tape_echo/LICENSE-MIT`). Contract, mapping, cost: `plugins/tape_echo/README.md`.
- Controls: the four turns as kennel (time, feedback, age, mix); fn + blue gain (word 5, `"fine": true`, `"step"`
  1365 / `"fine_step"` 273: 0.5 / 0.1 dB a detent), fn + green sync (word 6, steps: `off` and kennel's tempo lock's
  11 divisions, 1/16T .. 1/2; the owner moved it off shift + blue, which is the core's fine step now), green click
  hold (latch bit 0), white click heads (steps, word 7). Sync runs at 120 BPM until the core exports the tempo
  (`docs/request-tempo-export-tape-echo.md`).
- The DSP is copied into the plugin (as rings): the stereo path only, its constexpr double tables as integer literals
  (`src/tape_echo_tables.h`, generated by `tests/host/tape_echo/gen_tables.cc`), the C kernels for host builds
  only. Host and simulator tests against the original are bit-identical (`tests/host/tape_echo`).
- Things other effect ports will meet: (1) a delay line's clear does not go in `init` (the audio interrupt at slots
  0-2): tape_echo clears 2,048 pairs a block for 17 blocks after a (re)start, the dry passing through; (2) GCC turns a
  struct zeroing (`k_ = Kernel()`) into a `memset` call, which the packer lists as an import (`imports _memset`)
  even with `-fno-tree-loop-distribute-patterns`: zero it with a loop of words; (3) two sources with the same stem
  (`tape_echo.cc`, `src/tape_echo.cc`) give one object name and a link error: rename one.
- 10,664 bytes packed, `.bss` 134,764; load size 143,652 x 5 = 718,260 bytes of heap 0. GNU simulator 15,880 /
  16,637 cycles a block (avg / max); op1emu and device pending (`docs/device-tests.md` "tape_echo 0.1").

## wavefolder 0.1 (2026-11-11, a kennel effect port)

`plugins/wavefolder` (contract, cost and gate plan in its README): te-op1-cfw's FOLD (a sine wavefolder with
first-order ADAA, `ports/fx/src/fold` at 5aa314b) from the kennel plugin `wavefolder` 0.2. Turns fold, symmetry,
tone, mix as kennel; fn + blue the input gain (word 5, -6..+6 dB, `"fine": true` so shift + fn + blue is its fine,
`"random": false`). The upstream stereo path runs at every position; no `"stereo"` flag in plugin.json (a
`coop_fx_t` is stereo by ABI, and the loader would read the flag as kennel's `op1_fx_stereo_t`).
- Removed from the kennel wrapper: `knl::Fine` (the shift byte at 0x01DD7F78, rewriting stored words),
  `op1_fx_param_q15` (the range table at 0x01DE5C50; now the SDK's constant ranges, once a block and only when a
  word moved), `op1_fx_scratch` (one `Fold` in each relocation's `.bss`), the page-2 press and the screen calls.
- coop additions: the int32 <-> int16 bus conversion in assembly (`src/bus_bfin.S`), a mix-0 bypass that leaves the
  bus words untouched, and an L == R shortcut (the mono kernels once while the channel states are equal:
  bit-identical to the stereo run).
- Host test `tests/host/wavefolder` (`make TE=<te-op1-cfw>/ports all sim`): bit-identical to the upstream driven as
  `fold_knl.cc` drove it, natively (C kernels) and in the GNU simulator (assembly). Simulator cost a block: 5,716
  avg / 6,962 max stereo (upstream `Process` alone 4,807); device estimate 9-15 k [I]. Packed 9,980 bytes, load size
  10,008 (x 5 = 50,040). op1emu gate: not run yet (plan in the README); device: pending.
