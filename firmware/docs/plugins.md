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
slot 1, with the four knob words in the filter ranges and `params[4] = 0x5300 + n`. Memory (until e8, which keeps an effect's state in a position buffer and relocates it once; old-model files still load this way): five times the load size
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
- 1,680 bytes packed (with the gain steps), load 1,356 (x5 = 6,780; e8: 1,860 packed, load 1,476 once, scratch 32 a position); simulator compute 5.3 k / 6.7 k cycles a block
  avg / max [C simulator]; op1emu gate passed 2026-11-11 (below); device pending.

## Port: drive (kennel effect, 2026-11-11) [C host + GNU simulator + op1emu; device pending]

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
- Size: 9,608 bytes packed, load 9,948 (x 5 = 49,740 bytes of heap 0); no imports; `.bss` 1,584. e8: 9,464 packed, load 8,480 once, `.bss` 20, scratch 1,600 a position.
- Tests (`make -C tests/host/drive all sim`): the plugin's render on the int32 bus against upstream's DSP behind
  kennel's stereo glue over 20,000 random blocks (knobs jumping, sweeping, out of range; gain word changing; re-inits),
  bit-identical on the host (C passes) and in bfin-elf-run with the device build (assembly passes) against upstream's
  C; mix 0 = the input bit for bit. Simulator cost: 5,539 a block still, 7,578 avg / 8,042 peak moving (upstream's own
  figure 4,651 / 5,009 stereo; kennel ran 3,235 / 3,605 mono at the instruments).
- op1emu gate passed 2026-11-11 (below): drive is row 2 of the alphabetical FX list (confirmed),
  `tests/sim/det_fx_drive_select.txt` picks it with `ccw 96` then `cw 16`. 9,616 bytes with the gain steps.

## ensemble (2026-11-11): the kennel ensemble on e6

`plugins/ensemble` (0.3): te-op1-cfw's kennel ensemble (`ports/knl/ensemble` 0.2) with its DSP copied into the plugin
(`src/`: the core of `ports/fx/src/ensemble`, Clouds' halfband path, `half_rate.h`), the upstream's constexpr double
tables turned into integers (`gen_tables.py`), C kernels host-only, the assembly kernels renamed `Ens*`. Controls: the
four turns as kennel (rate, depth, voices, mix; normalised from the filter ranges as `op1_fx_param_q15` did, the
ranges as constants), fn + blue the input gain (word 5) with shift + fn + blue its fine (`"fine": true`, `"random":
false`); no latch. Kennel's `"stereo": true` is left out (coop's loader reads it as kennel's stereo layout). True
stereo at every position; one exact saving: a block with L == R and equal decimator histories copies the left
decimator's result. File 10,820 bytes, load 19,380 x 5 (e8: 9,736 bytes, load 8,992 once, scratch 10,400 a position). `tests/host/ensemble`: bit-identical to the upstream on the
host (C) and in bfin-elf-run (assembly), 12.8 k mean / 14.8 k max cycles a block in the simulator. Gate plan and the
scenario `tests/sim/det_fx_ensemble.txt`: `plugins/ensemble/README.md`. Device test: `docs/device-tests.md`
"ensemble 0.3".

## Port: freq_shift (kennel freq_shift 0.2, 2026-11-11) [C op1emu; I until the device test]

`plugins/freq_shift`: te-op1-cfw's frequency shifter / ring modulator after Mutable Instruments Warps (MIT,
`ports/fx/src/shift` at 5aa314b, the kennel plugin `ports/knl/freq_shift`), self-contained (`src/` the DSP and its
assembly, `op1/support.cc`, `LICENSE-MIT`). Contract, stereo and cost: `plugins/freq_shift/README.md`.
- **Controls** (the owner's layout): the four turns are kennel's knobs (shift, feedback, spread, mix), normalised from
  the filter type's ranges as kennel's `op1_fx_param_q15` did; kennel's blue page-2 gain is `fn_turn` blue word 5
  with `"fine": true` (shift + fn + blue its fine, `"random": false`); kennel's green page-2 mode (`ClickMode(word, 2)`)
  is the green click, steps `shift` / `ring`, word 6, saved, wrapping, randomised.
- **Removed from the kennel glue** (`common/knl_fx.h`): the shift byte read at 0x01DD7F78 (`knl::Fine`, now the core's
  fine), the range table read at 0x01DE5C50 (`op1_fx_param_q15`, now the constant ranges once a block), the instance
  in `op1_fx_scratch` (`.bss`, one per relocation, until e8 put it back in a position buffer: `coop_fx_scratch`), `op1_fx_page` / `op1_fx_knobs` / the screen reads, the input
  meter, `memset` / `<cstring>` in the DSP. No `"stereo"` flag (kennel's layout; a `coop_fx_t` is stereo by ABI). The
  screen (`shift.svg`) is not carried: effect screens aren't built.
- **Stereo:** a Hilbert pair per channel instead of upstream's one pair on (L + R) / 2 (guidelines §2.2: no mono
  collapse); an L == R block runs one pair. Host test: bit-identical to two upstream mono instances (L at shift +
  spread, R at shift - spread), and with L == R to upstream's stereo path (`make -C tests/host/freq_shift`); the
  assembly equals the host C in the GNU simulator (`make ... sim`).
- **Cost** (GNU simulator, cycles a block): 9,762 avg / 11,013 max with L == R, 14,383 / 15,868 with L != R
  (upstream's stereo call: 6,585 / 7,187). op1emu: below; device pending. File 10,308 bytes, load 14,168 (x5 = 70,840; e8: 10,152 bytes, load 9,664 once, scratch 4,640 a position).
- Gated in op1emu 2026-11-11 (below); scenario `tests/sim/det_fx_freq_shift.txt`.

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
  9,061 cycles a block (upstream: 7,026 stereo, 4,128 mono). The op1emu gate passed 2026-11-11 (below); the device test is
  pending (`docs/device-tests.md` "tape_drive 0.4").

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
- 10,664 bytes packed, `.bss` 134,764; load size 143,652 x 5 = 718,260 bytes of heap 0 (e8: 10,160 bytes, load 9,132 once, `.bss` 20, the 134,784-byte state in each position buffer). GNU simulator 15,880 /
  16,637 cycles a block (avg / max); op1emu gate passed 2026-11-11 (below); device pending (`docs/device-tests.md` "tape_echo 0.1").

## wavefolder 0.1 (2026-11-11, a kennel effect port)

`plugins/wavefolder` (contract, cost and gate plan in its README): te-op1-cfw's FOLD (a sine wavefolder with
first-order ADAA, `ports/fx/src/fold` at 5aa314b) from the kennel plugin `wavefolder` 0.2. Turns fold, symmetry,
tone, mix as kennel; fn + blue the input gain (word 5, -6..+6 dB, `"fine": true` so shift + fn + blue is its fine,
`"random": false`). The upstream stereo path runs at every position; no `"stereo"` flag in plugin.json (a
`coop_fx_t` is stereo by ABI, and the loader would read the flag as kennel's `op1_fx_stereo_t`).
- Removed from the kennel wrapper: `knl::Fine` (the shift byte at 0x01DD7F78, rewriting stored words),
  `op1_fx_param_q15` (the range table at 0x01DE5C50; now the SDK's constant ranges, once a block and only when a
  word moved), `op1_fx_scratch` (one `Fold` in each relocation's `.bss`; e8: in the position buffer again, `coop_fx_scratch`), the page-2 press and the screen calls.
- coop additions: the int32 <-> int16 bus conversion in assembly (`src/bus_bfin.S`), a mix-0 bypass that leaves the
  bus words untouched, and an L == R shortcut (the mono kernels once while the channel states are equal:
  bit-identical to the stereo run).
- Host test `tests/host/wavefolder` (`make TE=<te-op1-cfw>/ports all sim`): bit-identical to the upstream driven as
  `fold_knl.cc` drove it, natively (C kernels) and in the GNU simulator (assembly). Simulator cost a block: 5,716
  avg / 6,962 max stereo (upstream `Process` alone 4,807); device estimate 9-15 k [I]. Packed 9,980 bytes, load size
  10,008 (x 5 = 50,040; e8: 9,716 bytes, load 9,116 once, scratch 960 a position). op1emu gate passed 2026-11-11 (below); device: pending. Packed 9,988 with the gain steps.

## cs_hall (2026-11-11): the hall reverb from te-op1-cfw (kennel cs_hall 0.1)

`plugins/cs_hall` (README: contract, glue, memory, cost plan, gate plan). The DSP is te-op1-cfw's `ports/fx/src/hall`
with its Blackfin kernels and Clouds' halfband path, copied into the folder; its `constexpr double` tables became the
integer literals they evaluated to. True stereo at every position (a `coop_fx_t` is stereo by ABI; kennel's
`"stereo": true` is not carried over: coop's loader would read it as kennel's `op1_fx_stereo_t` layout and refuse
the file). Controls: the four turns size / decay / damp / mix; fn + blue predelay, fn + green early, fn + white width,
each with its fine on shift + fn (words 5-7, saved); freeze on the orange click (latch bit 0). The 84 KB tank is
cleared in render, 4 KB a block (21 blocks dry), not in init (guidelines §2.3). Host test `tests/host/cs_hall`:
bit-identical to the upstream through kennel's glue, host C and the Blackfin build in the simulator (18-22 k cycles a
block, 26 k peak there). Load size 102,932 bytes (~515 KB for five relocations; e8: 17,060 bytes, load 16,216 once, the 86,752-byte state in each position buffer). No `format` field in the controls
schema yet, so predelay can't be shown in ms. op1emu gate passed 2026-11-11 (below); device test pending (`docs/device-tests.md` "cs_hall").

## cs_plate (2026-11-11): the plate reverb, a kennel effect port on e6

`plugins/cs_plate` (the contract and port notes are in its `README.md`). It is te-op1-cfw's Dattorro plate
(`ports/fx/src/plate`, 9ced415), true stereo at every position, from its unreleased kennel build
(`ports/knl/cs_plate`). The four turns are kennel's four knobs. fn + blue is size as 8 `steps`: the plate's 8 tank
lengths were quantised in kennel's glue, so guidelines §3.1 make them steps. It has 4 detents a step, no wrap and no
fine. fn + green is diffusion and fn + white is motion, both continuous with their fine. Freeze is a latch on the
orange click. Notes for the other effect ports:
- kennel's `"stereo": true` is not carried into plugin.json (the loader would take it for `op1_fx_stereo_t`).
- `constexpr double` table builders become integer literal tables, checked against the upstream build by the host test.
- A large state (here 76 KB) is not cleared in `init`. `init` arms a counter, and the renders clear 8 KB a block
  with the dry passing (2.2 k cycles a block in the GNU simulator, 10 blocks), then reset. Kennel's half-rate
  restart on a 5 ms pause (`op1::Cycles`) goes, since coop calls `init` at every restart.
- The turns' range units map to Q15 with kennel's `op1_fx_param_q15` formula and the SDK's constant ranges.
- Host test: `tests/host/cs_plate` (host: C both sides; GNU simulator: the plugin's assembly against upstream's C),
  bit-identical. Cost in the simulator: 17,456 cycles a block, of which the plate is 7,956 and the half-rate path
  ~7,500. The op1emu gate passed 2026-11-11 (below); the device run is pending.

## The nine effect ports merged and gated (2026-11-11)

bitcrusher, drive, ensemble, freq_shift, tape_drive, tape_echo, wavefolder, cs_hall and cs_plate are merged on dev
(each port's own section above; device tests pending in `docs/device-tests.md`, one entry each).

**The owner's decisions applied after the merge (2026-11-11):**
- Input gain in kennel's steps on every port with a gain (bitcrusher, drive, ensemble, freq_shift, tape_drive,
  wavefolder; tape_echo had it): `"step": 1365, "fine_step": 273` (0.5 dB / 0.1 dB a detent over the +-6 dB word).
  The four files grew by 8 bytes each (the two fields in the controls extension).
- freq_shift: a Hilbert pair per channel, approved (no L + R sum).
- The dry pass-through for 15-30 ms after a restart (cs_hall, cs_plate, tape_echo: the tank or tape clear) is accepted
  as it is; the device tests say to listen for it.
- cs_hall: width at every position, default full width, accepted. cs_hall / cs_plate: the tank's L + R input is the
  reverb's design (the wet is stereo from the tank taps, the dry stays stereo), a §2.2 exception, accepted.
- cs_hall predelay is to show in ms once the controls schema has a `format` field (noted in its README).

**Parked: cs_clouds and cs_clouds_delay** (branch `worktree-agent-a125e10b2d0409be0`, not merged). The owner parked
them for their cost: GNU simulator average 30 k / 37 k cycles a block, peaks 69 k / 80 k (over the 60 k a plugin may
cost).

**Sizes before e8** (packed / load / load x 5 relocations, bytes; e8 relocates each once and keeps the state in the position buffers: "e8" below has the new figures):

| Plugin | Packed | Load | x 5 |
|---|---|---|---|
| bitcrusher | 1,680 | 1,356 | 6,780 |
| drive | 9,616 | 9,948 | 49,740 |
| ensemble | 10,828 | 19,380 | 96,900 |
| freq_shift | 10,308 | 14,168 | 70,840 |
| tape_drive | 11,072 | 13,912 | 69,560 |
| tape_echo | 10,664 | 143,652 | 718,260 |
| wavefolder | 9,988 | 10,008 | 50,040 |
| cs_hall | 17,712 | 102,932 | 514,660 |
| cs_plate | 11,460 | 87,534 | 437,670 |

**op1emu gate** (e6 image from dev after the merge, release core; each port alone on a NAND; one run at a time on an
idle machine). Every port: `plugins seen 1 loaded 1 failed 0`, no fault, the chord through it at the synth FX 1
(`det_fx_select.txt`), the five positions with their declared controls (`det_fx_controls.txt`, chord and drum keys
through all five), the words kept and the latches cleared after a power cycle (`det_fx_reload.txt`), and each port's
own scenario where it has one. The words and figures per port are in its `docs/device-tests.md` entry. Cost, cycles a
block, average / peak, **emulator figures (the device is typically 10-35 % higher)**; from the `det_fx_controls.txt`
run (all five positions live), the synth-only `det_fx_select.txt` in brackets:

| Plugin | instrument FX 1 (`fx1_plugin`) | instrument FX 2 (`s2_instr`) | master FX 2 (`s2_master`) |
|---|---|---|---|
| bitcrusher | 6,446 / 7,415 (6,467 / 8,095) | 5,586 / 6,254 | 5,440 / 5,486 |
| drive | 7,615 / 10,484 (7,617 / 10,484) | 6,348 / 8,837 | 6,395 / 8,884 |
| ensemble | 13,725 / 15,176 (13,522 / 14,588) | 12,493 / 13,871 | 12,161 / 13,370 |
| freq_shift | 11,159 / 16,151 (10,561 / 13,428) | 9,776 / 14,129 | 8,859 / 12,211 |
| tape_drive | 10,931 / 12,207 (10,931 / 11,837) | 9,666 / 10,530 | 9,712 / 9,966 |
| tape_echo | 16,821 / 18,176 (16,778 / 18,312) | 15,632 / 16,447 | 15,581 / 16,140 |
| wavefolder | 5,786 / 7,305 (6,047 / 7,292) | 4,532 / 6,041 | 5,735 / 6,088 |
| cs_hall | 20,010 / 26,893 (20,016 / 21,780) | 18,763 / 25,644 | 18,800 / 20,252 |
| cs_plate | 19,181 / 21,187 (19,176 / 20,585) | 17,922 / 19,934 | 17,966 / 18,469 |

None is near 60 k; cs_hall's peak (26.9 k, size crossfades and randomised jumps) is the highest. Master FX 1 has no
cost slot of its own (the master runs the same render) [I].

**Found by the gate:**
- **The stock FX list is alphabetical with the plugins sorted in among the stock types** ("bitcrusher", "cs hall",
  "cs plate" before cwo; "drive", "ensemble", "freq shift" at row 2; "tape ...", "wavefolder" after spring). The e6
  scenarios pick the last row (`turn enc1 cw 96`), which only lands on a plugin named after "spring". The gate picked
  the first row with `ccw 96` and row 2 with `ccw 96` then `cw 16`; `det_fx_drive_select.txt`, `det_fx_ensemble.txt`,
  `det_fx_freq_shift.txt`, `det_fx_cs_hall.txt` and `det_fx_cs_plate.txt` now do that. The FX 2 list (coop's) lists
  plugins in number order, so `det_fx_controls.txt`'s FX 2 picks worked unchanged with one plugin on the NAND.
- Three port scenarios ended before their own events (their final `wait`, wall time, was too short):
  `det_fx_ensemble.txt` and `det_fx_freq_shift.txt` now wait 75 s, `det_fx_cs_plate.txt` 80 s.
- e6 loaded at most 8 plugin files and hung at boot above ~2.1 MB of plugin memory; both are fixed in e7 (next
  section).
- `det_chord.txt` is not exact run to run any more, for e5 and e6 alike (`docs/device-tests.md` "e6").

## e7 (2026-11-11): 32 plugin files, every file not loaded named, a plugin memory budget

The owner's decisions after the e6 gate: raise the file limit to 32 and report every file that does not load; find
why large plugin sets hang at boot, then add a load budget so the OP-1 always boots.

**The boot hang, found** [C op1emu]. rings + tape_echo + cs_hall + cs_plate (2.34 MB) on a fresh NAND, e6 image, with
op1emu's new `--dump-at-exit LO+LEN:PATH` (guest memory to a file and the PC / SP / FP / RETS logged at exit;
`tools/op1emu-patches/0004-dump-at-exit.patch`, applied by `tools/setup_op1emu.sh` after 0001-0003):
- The CPU sits at `0x0191995A`, `IDLE; JUMP.S self`, called from `0x01919B08`: the stock fatal handler (`0x01919AA8`).
  Its message buffer (`0x01BA138C`) reads `..\src\dsp\tape_c.cpp:62 data` with the backtrace `0x1a5425e 0x19d7bc8
  0x19f040a 0x19f0f08 0x1a9c290`. Nothing reaches the screen, so the last frame (the first-boot "side_b.aif 94 %")
  stays.
- `0x01A5425E` is in the tape cache constructor (`0x01A54168`, docs/tape-stereo.md): 24 nodes, each
  `MALLOC(0x80000)` (heap 0; 512 KiB with the stereo tape) whose null result is that assertion.
- heap 0 at the hang (descriptor at `0x100`, from `[0x01DFFC40]`): break `0x00FC9780`, 223,356 bytes above it, 7 free
  blocks of 1,019,808 bytes, the largest 524,288: a node takes a 0x80008-byte block. So heap 0 ran out.
- The break over the boot (`--watch-writes 0x10c`, the descriptor's break word): 0xE3BA0 when coop's plugin scan starts
  (2.64 s), 0x449740 after it (the plugins and the 1 MiB staging block), **0xC49748 after e4's `heap_probe`** (+8 MiB),
  then the tape's nodes: 15 fit in the 8 MiB block the probe left free, 7 from the break, one from the staging hole,
  the 24th fails at 2.81 s.
- **Why `heap_largest` said 8,388,608 every time**: the probe's binary search asks for 16 MiB (fails), then 8 MiB, which
  succeeds from the break; heap 0 never moves its break back, so the freed 8 MiB becomes a free block and every larger
  try fails. Any heap with 8 to 16 MiB left reports exactly 8 MiB, and the probe cost the tape a node of room (8 MiB
  holds 15 nodes, not 16).
- The same mechanism on every boot: the cache is built after the plugin scan on a fresh NAND (1.23 s) and on a booted
  one (0.81 s). The pristine NAND's first boot ends with 2,729,052 bytes of heap 0 free, the second boot with 3,053,204
  (op1emu, no plugin files, e6): the first boot makes the tape files and is the worst case. After the cache, the stock
  takes another ~0.3 MB of heap 0 at boot (both boots) and nothing more through a minute of playing.

**What e7 does** (`core/sdram/plugin_host.cc`):
- `MAX_FILES 32` (numbers 32..63). The scan reads the whole directory: a 33rd and later `.coop` file is counted
  (`skipped`), the first 8 of them named, and a `.coop` name of 40 characters or more is counted too (`badname`).
  `seen` = loaded + failed + disabled + skipped. The settings menu's disabled list holds 32 (`DIS_MAX`, was 16); the
  SysEx inventory lists up to 48 files (8 KB of text, was 16 files / 2 KB).
- **The budget: 1,536 KiB (1,572,864 bytes) of heap 0 for all plugins**, counted as the heap takes it: each block
  `(n + 15) & ~7` with its header; an effect = 5 relocations, an engine = its relocation + its per-voice state. Files
  load in directory order; one that would take the total over the budget is refused before anything is allocated:
  `over the plugin memory budget`. Why 1.5 MiB: the first boot leaves 2.6 MiB free with no plugins; 1.5 MiB keeps
  ~1.1 MiB free after the boot, for what coop allocates later (an FX 2 delay / cwo line 173 KiB each, spring tanks
  57 KiB, the master's grid tank 264 KiB: ~0.75 MiB at most), the USB disk heap (96 KiB) and unknown stock use. A booted
  NAND has 0.3 MiB more. [I: the device's heap use is assumed to match op1emu's.]
- **A second check, against the heap itself**, before a plugin's blocks are taken: heap 0 (read in place) must still
  hold the tape's 24 nodes **plus one spare** (node size read from the stock instruction at `0x01A54172`) and 512 KiB
  more, counting the plugin's blocks as taken off the break (an engine with the largest voice state, 48 KiB), or the
  plugin is refused: `not enough memory left for the tape`. The first version checked after allocating and freed the
  refused plugin: op1emu still hung, because a freed block never goes back above the break and the 24th node did not
  fit (gate below). With the budget it does not trigger in op1emu; it is there for a device whose stock uses more heap
  before the scan (many files on the NAND) [I].
- `heap_probe` is gone. The stats read heap 0 in place: `heap_largest`, `heap_free`, `heap_top` (above the break), on
  the `plugins` line, at the time the stats are written.
- Test switch `--set-word g_coop_plug_budget=N` (another budget; 0 = 1,536 KiB) to exercise the heap check.

**Stats** (after the `plugins` line, which gains `skipped` and `over_budget`):
```
plugmem budget 1572864 used <bytes> over_budget N heap_short N skipped N badname N tape_node 524288 heap_free_boot <bytes>
plugfile <file> <failed|disabled|skipped> needs <bytes>: <reason>      one per file not loaded
plugfile unnamed_skipped N                                             files past the 8 named ones
```
**Inventory** (docs/sysex.md): a refused or skipped file is `failed` (the librarian's word for not loaded) with the
reason, e.g. `over the plugin memory budget (needs 702 KB, 457 KB of 1536 KB left)`, `more than 32 plugin files`.

**Gate** (op1emu, one run at a time; figures in `docs/device-tests.md` "e7") [C op1emu]:
- The 2.34 MB set that hung on e6 boots: rings and cs_plate load, tape_echo and cs_hall `over the plugin memory budget`.
- The 11-file set (nine ports, rings, saw) boots on a fresh NAND and again on its second boot: 9 load (saw,
  tape_drive, bitcrusher, ensemble, rings, cs_plate, drive, wavefolder, freq_shift, 1,451,544 bytes), tape_echo and
  cs_hall are over budget; ~1.26 MB of heap 0 is left at the end of the first boot, ~1.59 MB on the second.
- With the budget switched off (`g_coop_plug_budget=16777216`) the heap check alone keeps it booting: 8 load,
  wavefolder, cs_hall and freq_shift `not enough memory left for the tape`, 0.67 MB left.
- Without e4's probe, the 2.34 MB set itself would fit (all four load with the budget off, 0.39 MB left), and the e6
  image boots it on the NAND's second boot (the first boot has 0.3 MB less): the probe's 8 MiB and the first-boot
  tape files were what tipped that set over.
- Regression: pristine `det_chord.txt` e7 vs e6 within the known run-to-run spread (one build identical over 3.2 M
  frames), `det_fx_controls.txt` / `det_fx_reload.txt` and saw `det_eng_controls.txt` as e6.

## e8 (2026-11-11): the position buffers, an effect relocated once

The owner's decision after e7 (the budget refused tape_echo and cs_hall, the e6 hang was the stock tape cache running
out of heap 0): **kennel's model**. The firmware owns one working buffer per FX position; the effect active at a
position works in it; an effect file is relocated once; an effect restarts (re-initialises, nothing preserved)
whenever it is selected at a position. Kennel's reference: te-op1-cfw `ports/knl/common/knl_fx.h` (a 176,460-byte
`op1_fx_scratch(pos)` per position, "re-initialised in init and never assumed preserved"). The nine ports came from that
pattern and had moved their state into `.bss` (one copy per relocation, five relocations); e8 puts it back.

**ABI** (`sdk/coop_plugin.h` "e8", `docs/fx-abi.md` "Memory"):
- plugin.json `"scratch": N` (bytes, 1 .. `COOP_FX_SCRATCH_MAX` = 180,224). pack.py sets `KN_FLAG_SCRATCH` (0x20, a new
  header flag) and extension 7 (u32 N), refuses N out of range, a non-effect, and a file that imports `coop_fx_scratch`
  without declaring it; it compiles the sources with `-DCOOP_FX_SCRATCH_BYTES=N` so a port static_asserts its state
  fits. `tools/plugin/reloc.py` and `core/sdram/plugin_load.cc` check flag and extension together.
- `void *coop_fx_scratch(int slot, uint32_t *bytes)`: a core export (`sdk/coop_exports.cc`, entry 14), valid from init
  and render: the slot's buffer (32-byte aligned; `*bytes` = its size, at least N), 0 for a slot outside 0..4 or
  before the buffers exist. The address is fixed for the session; the ports keep it per slot from init.
- `init(slot)` is the restart: the core already calls it whenever the effect is selected at a position (positions 0-2 on
  the first block after a selection or sound change, slots 3-4 from the main loop when the FX 2 choice changes), so
  nothing new is called. It resets every field of the state (the buffer holds the previous effect's bytes) and stays
  cheap: tape_echo, cs_hall and cs_plate still clear their tanks over the next renders while the dry signal passes.
  cs_hall now clears on every (re)start (before e8 it skipped a tank the loader had zeroed): the first selection
  also passes ~30 ms dry.
- **Backward compatible**: a file without the flag (stereo_delay, test_fx, the probes) loads as before: five
  relocations, its state in `.bss`, the e7 budget at five times its load size. The engines (saw, rings) are not
  touched. **A core before e8 refuses a scratch file** at `kn_parse`: "unknown plugin flags" (as e6 did with
  `KN_FLAG_PAGE2` for cores before it) [C: the e7 flag mask in `tests/tools/test_pack_scratch.py`; I on a device].

**Core** (`core/sdram/plugin_host.cc`):
- A scratch file is relocated once; `g_plug[n].inst[0..4]` are the same object. It waits in `pend` (not selectable)
  until the buffers exist.
- **The buffers**: one heap-0 block of 5 x E (+ 32 for the alignment), zeroed, taken at the end of the boot scan, before
  the staging block is freed (so it comes from the break and does not split the staging hole the tape's nodes use) and
  so before the stock tape's cache; only when a scratch file loaded. E = the largest declaration among the scratch
  files that loaded, 32-byte rounded: with the nine ports, tape_echo's 134,784 (5 x 134,784 = 673,920; the block
  673,960). Sized at boot rather than fixed, because a set of small effects then costs almost nothing (bitcrusher
  alone: 5 x 32 bytes) and the full set fits the budget with rings (kennel's 176,460 x 5 = 882 KB plus rings' 666 KB
  would not). The ceiling `COOP_FX_SCRATCH_MAX` (176 KiB) keeps any kennel effect core portable. Then every waiting
  file is published. If heap 0 cannot give the block after all (the scan's heap check said it could: a heap read gone
  wrong), every scratch file is refused with "no memory for the effect buffers" and its image freed.
- **The budget counts the buffers once**: a scratch file costs its image block plus what it grows the buffer block by
  (`fxbuf_cost(new max) - fxbuf_cost(old max)`); the e7 heap check (`heap_room_ok`, before allocating, blocks counted off the break) adds the buffers still to be taken
  to every file's check, scratch or not, so the tape's 24 nodes and 512 KiB stay free. `needs` in `plugfile` lines and
  the inventory is that charge.
- Same effect at two positions: two buffers, two states (positions 0, 1, 2, 3, 4 each have their own). Switching effects
  at a position: the new effect's init resets the buffer for itself; selecting the old one again inits it again. A
  stock effect at a position does not use the buffer. Slot 2's init runs on the main loop while the audio interrupt
  renders the other positions; the ports keep nothing shared that init writes (block buffers are in the state too).
- Checked and unchanged: the latches (`g_fx_latch[pos]`, cleared at every init), the per-position controls (e6), the
  stats `fxctl` and cost lines (per position, not per instance), `g_cur[]` (which plugin each position initialised
  last: it is what triggers the restart), the screens (the stock filter page), `g_plug[n].fx = 0` when an init fails
  (one image: the effect is dropped at every position, as before when any instance's init failed).
- Refusals: "effect buffer larger than this core gives (repack)" (N over `COOP_FX_SCRATCH_MAX`), "calls coop_fx_scratch
  without the scratch flag (repack)", "no memory for the effect buffers", as e7's in the stats and the inventory.
- Build tag `e8`.

**Stats**: the `plugmem` line gains `scratch_files S fxbuf E fxbuf_mem M fxbuf_short K` (scratch effects loaded, each
position buffer's bytes, the buffers' heap block, 1 when heap 0 could not give it).

**The nine ports** (each README's memory line): the state struct moved into the slot's buffer (`Instance* g_inst[5]` in
`.bss`, 20 bytes; cs_plate a `Slot` with the clear counter outside the cleared plate); init binds the slot and resets
the state; render works on the slot's state. Host tests (`tests/host/<id>`) link `tests/host/fx_scratch_host.cc`: five
buffers of the declared size filled with random bytes, so an init that relied on zeroed memory fails the bit-exact
comparison (checked: bitcrusher without its `Init()` mismatches 853,221 samples), and `coop_fx_scratch_check` (the
effect at slots 0 and 3 interleaved on different signals must give slot 0 the output it gives alone; a restart on a
dirtied buffer must give it again). Harness changes: freq_shift renders at slot 0 only and initialises slot 4 before its
mix-0 check (it had rendered slots 0 / 2 / 4 from one state); ensemble's random re-init uses the rendered slot;
cs_hall's reference passes its 21 clear blocks after every restart (the plugin clears always now) and initialises the
slot it renders (2).

| Plugin | Packed | Load (once) | `.bss` | Image block | `"scratch"` | Before e8: load x 5 |
|---|---|---|---|---|---|---|
| bitcrusher | 1,860 | 1,476 | 20 | 1,536 | 32 | 6,780 |
| drive | 9,464 | 8,480 | 20 | 8,536 | 1,600 | 49,740 |
| ensemble | 9,736 | 8,992 | 20 | 9,048 | 10,400 | 96,900 |
| freq_shift | 10,152 | 9,664 | 20 | 9,720 | 4,640 | 70,840 |
| tape_drive | 11,084 | 10,628 | 20 | 10,688 | 3,392 | 69,560 |
| tape_echo | 10,160 | 9,132 | 20 | 9,192 | 134,784 | 718,260 |
| wavefolder | 9,716 | 9,116 | 20 | 9,176 | 960 | 50,040 |
| cs_hall | 17,060 | 16,216 | 20 | 16,272 | 86,752 | 514,660 |
| cs_plate | 10,940 | 10,064 | 20 | 10,120 | 77,664 | 437,670 |
| **nine** | | | | **84,288** | buffers **673,960** | **2,014,450** |

All nine with the buffers: **758,248 bytes** of heap 0 (was 2,014,450 plus block headers, which the 1,536 KiB budget
could not hold). With rings (666,064) and saw (2,320 + its voice state) the owner's 11-file set takes 1,426,632 bytes of the
1,572,864 budget [C op1emu: `plugmem ... used 1426632`, all 11 loaded, `heap_short 0`; on e7 the same set loaded 9 (1,451,544) and with the budget off the heap check refused three]. Gate results: `docs/device-tests.md` "e8".

New sim scenarios: `tests/sim/det_fx_two_pos.txt` (one plugin at synth FX 1, instrument FX 2 and master FX 2 at once;
`det_fx_two_pos_first.txt` for a plugin on the first row of the FX list) and `tests/sim/det_fx_switch.txt` (cs_hall /
tape_echo switched at synth FX 1 and at instrument FX 2: one init per selection). Gate: `docs/device-tests.md` "e8".
