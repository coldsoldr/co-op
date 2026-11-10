---
name: port
description: Port a hundekraft (kennel .knl) or OPUS (.opmod) plugin into a coop plugin under plugins/<id>/, following guide/plugin-guidelines.md, with the stereo dual-mono wrap, the contract, the pack and the op1emu gate. Use when the user says /port <source>, "port <plugin>", or asks to bring a kennel or opus plugin to coop.
---

# /port: bring a kennel or OPUS plugin to coop

**Where you work:** the coop firmware is the `firmware/` folder of the co-op repository. Every path in this skill
(`plugins/`, `docs/`, `guide/`, `tools/`, `tests/`, `sdk/`, `third_party/`, `incoming/`) is relative to `firmware/`;
run commands from there (`cd firmware`), and read `firmware/CLAUDE.md` first. The rest of the repository is the co-op
web app: a port doesn't touch it (publishing a finished plugin in its catalogue is a separate step, `plugins/<id>/`
at the repository root, `docs/contributing-plugins.md`).

Argument: a plugin name or a source directory. Sources:
- kennel: `incoming/hk/hundekraft-3.1.4-source/ext/kennel/synth_sdk/plugins/<name>/` (SDK header
  `synth_sdk/op1_plugin.h`, vendored in `third_party/kennel/`);
- OPUS: `incoming/opus/plugin-kit/examples/opus.<name>/` (API in `plugin-kit/docs/api.md`, `rules.md`);
- te-op1-cfw: `../../te-op1-cfw/ports/<name>/` (its own `docs/<name>-port.md`).

Read `guide/plugin-guidelines.md` (the rules) and `docs/plugin-performance.md` (the loop rules) first; cite the
section when you apply a rule. **A synth engine** (kennel `op1_engine_t`, kind synth) follows "Engines" below for
steps 3-6 and 9, and reads `docs/engines.md` (E1, E2, e4) and `plugins/saw/` (the reference engine that uses every
part of the engine ABI) first. Sections 7.1 and 7.2 of the guidelines are the translation tables; use them, don't
invent mappings.

## Hard rules (from CLAUDE.md)
- Never commit Teenage Engineering firmware bytes or `.op1` files. Upstream plugin source is GPL (kennel) or its own
  licence (OPUS): check the licence before copying a line, record it in `plugin.json` `description`.
- Fixed point only, no float, no libc, no static constructors, no firmware addresses in a plugin.
- Nothing is done without a device result in `docs/device-tests.md`. You build; the owner flashes.

## Steps

1. **Read the upstream plugin whole**: its DSP, its `plugin.json` / manifest, its screen, and how it uses the
   params (kennel: `params[0..7]`, press modes, page words; OPUS: `limits`, `shift_knob`, `turn`). List every
   control and what it does, in a table, before writing code.

2. **Scan for what coop forbids**, and fix each hit:
   ```sh
   grep -nE '\b(float|double)\b' <src>            # none allowed, not even outside audio
   grep -nE '\b(malloc|memcpy|memset|printf|sqrt|sin|cos|pow|exp|log)[f]?\s*\(' <src>   # libc
   grep -nE '0x0?1[89A-F][0-9A-Fa-f]{5}|0x[Ff][Ff][AaBb]0' <src>   # R246 SDRAM / L1 addresses
   grep -nE 'op1_(find_node|eng_page|fx_page|synth_knobs_live|fx_knobs_live|voice_vel|l1_claim|fx_big|alloc)' <src>
   grep -nE '\b(opus_on_key|opus_on_knob|opus_fn_held|opus_engine_knob_set)\b' <src>
   ```
   A division inside the per-sample loop is a bug too (`docs/plugin-performance.md` rule 1).

3. **Stereo (guidelines §2.2).** coop's `render(slot, l, r, params, n)` is stereo at every position.
   - Mono-only upstream: wrap **dual-mono** with separate per-channel state (two instances of every delay line,
     filter history and envelope). Kennel keys state by position only; add the channel.
   - Upstream stereo (`render_stereo`, `stereo()`): port the stereo path and run it at every position.
   - Never sum L + R and re-spread.
   - **Stereo doubles the cost.** A kennel effect that fit at the master as mono now runs twice at every position.
     Budget: under 30 k cycles a block, over 60 k does not ship. Plan the optimisation pass now, not after the gate:
     fuse the two channels in the dual MAC where the chains are identical (`docs/plugin-performance.md`, rule 7), run
     the mono half once when L == R, keep state in registers, hoist per-block work. Tell the user up front what the
     upstream cost was (kennel `cost`, OPUS `cpu.txt`) and what doubling it means.

4. **Parameters (guidelines §3).** Write the parameter table in `plugins/<id>/plugin.json`: `id`, `name` (8 chars,
   stock glyphs), `action` (turn / click / shift_turn / shift_click / deep_turn / deep_click + encoder), `kind`,
   `min` / `max` / `default`, `steps`, `format`, `lfo`, `saved`. Translate with §7.1 / §7.2:
   - kennel push pages → `shift_turn` (continuous) or `click` with `steps`; latches → `latch` on a click;
     `PAGE_SEMI` → semitone turn with cents on fine; `coarse` → `steps: 8`; `params[4]` is never touched;
     the FX range table conversion (`op1_fx_param_q15`) goes away: the core hands Q15 of the declared range.
   - OPUS `limits {min,max,step}` → the table; `shift_knob` returning 1 → `shift_turn` parameters.
   - Enumerations quantised in the DSP (`word*n>>15`) become `steps`.
   - Apply the common meanings (§4.3: mix on orange, freeze on the orange click) and write down what moved and why.
   Until ABI v2 lands the core hands four Q15 knobs only; put the extra parameters in the table marked *proposed* and
   map the four turn parameters to `params[0..3]`.

5. **Write the plugin**: `plugins/<id>/<id>.cc` exporting one `coop_fx_t` (`sdk/coop_plugin.h`), state in `.bss`,
   `init(slot)` cheap (reset and bind only; a slow table build is noted as needing the main-thread hook), and
   `plugins/<id>/README.md` with the 4 x 6 action table (rows: turn, click, shift + turn, shift + click,
   shift + fn + turn, shift + fn + click; columns: blue, green, white, orange), the upstream mapping, and the
   memory / L1 / cost lines (cost left as "device: pending" until measured).

6. **Screen**: don't carry upstream art across unchecked. Until P3 the stock filter screen shows; write the screen
   brief against the manifesto (`guide/plugin-guidelines.md` §5) in the README, or mock it in the playground
   (`tools/playground/coop-playground.html`) first.

7. **Build and lint** (WSL): `wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/co-op/firmware && make -C plugins'` →
   `build/plugins/<id>.coop`; any `.S` goes through `tools/asm_lint.py`. Check the packed size and the load size x 5.

8. **Host test where a C reference exists**: a `tests/sim` harness against the upstream DSP over random blocks
   (bit-identical for a straight port; levels within tolerance for a restructured one), as `docs/plugin-performance.md`
   "How a rewrite is accepted".

9. **op1emu gate** (`docs/plugins.md` "Building and testing"): overlay NAND with the `.coop` via `tools/make_nand.py`,
   run `tests/sim/det_fx_select.txt`-style scenarios with the plugin at the synth, drum and master positions and at
   slot 2; a chord through it, no fault, the stats `plugins` line showing it loaded and rendering; a pristine-NAND run
   unchanged. **Gate runs go one at a time**: never in parallel with another run or a build (wall-clock waits).
   Read the stats cost line, and say plainly that it's an emulator figure (device is 10-35 % higher).

10. **Record**: add the pending device test to `docs/device-tests.md` (what to flash, what to listen for, the cost
    line to copy from `/yaffs2/user/coop_stats.txt`), and the port notes to `docs/plugins.md`. Commit as coldsoldr
    (see memory), no TE bytes, upstream licence named.

## Engines (kennel synth engines, e4 onward)

A port never changes the firmware. If the engine needs something the core doesn't give (an address it used, a control
layer, memory), stop and write a request in `docs/request-<topic>.md` for the firmware work, as the rings port did
(`docs/request-engine-controls-l1.md`, answered).

- **3e. The ABI** (`sdk/coop_plugin.h`): export one `coop_engine_t` (`COOP_ENGINE_MAGIC`, `voice_bytes`, `init`,
  `note_on(voice, state, velocity, params)`, `render(voice, state, out[128], pitch[128], params)`, `note_off`,
  `screen`). Mono int16 at 88.2 kHz, 128 samples; the stock envelope, LFO and coop's stereo placement follow.
  Kennel → coop: `op1_voice_index(self)` → `voice`; `OP1_PITCH_INC` → `pitch[]`; `OP1_VOICE_VEL` →
  `velocity` (Q15) at note on; `op1_synth_knobs` → `params[0..7]`; kennel's own voice-object state → `voice_bytes`
  host state or arrays in `.bss` indexed by voice (when voices hand state over on a steal, keep one `.bss` array).
- **4e. Controls**: the four turns are `params[0..3]` (0..32767, the stock owns them, shift is fine). Kennel push pages
  and page-2 words become declared controls in `plugin.json` "controls" (`docs/engines.md` "e4"): `click`,
  `shift_click`, `fn_turn`, `fn_click`, `shift_fn_turn`, `shift_fn_click`, in words 5..7, `steps` (named, up to 16)
  or `continuous`. The engine reads `params[5..7]`, clamps them and latches modes at note on. The core shows the name
  and value; randomise draws them unless `"random": false`.
- **5e. Memory and L1**: big state in `.bss` (relocated once for engines; ~600 KB loads, `heap_largest` in the stats
  says what's left). Per-call scratch: `coop_l1_scratch(&bytes)` (1312 bytes, inside note_on / render only). Hot
  per-voice data: `coop_l1_lease(bytes)` from `init` only (up to 4 KB for all engines), always with an SDRAM
  fallback. SDRAM lines cost a bus wait whenever they aren't cached; what the per-sample loop touches
  belongs in L1.
- **6e. Screen**: an SVG at the front of the file (`plugin.json` "screen": `svg`, `symbol`) and a `coop_screen_t`;
  draw through `sdk/coop_screen.h` (`coop_scr_find`, `_transform`, `_visible`, `_line_rgba` / `_fill_rgba`, `_draw`,
  `coop_scr_words` for all eight words, `coop_scr_mods` for the held layer). Kennel's `op1_find_node`,
  `op1_eng_page`, `op1_synth_knobs` map to these. `tools/plugin/pack.py` checks the SVG against R246's parser.
- **9e. op1emu gate**: a NAND with the `.coop` (`tools/make_nand.py`), `tests/sim/det_eng_select.txt` /
  `det_eng_note.txt` (select it: alphabetical in the list, 8 encoder steps a row; adjust the turn), `det_eng_controls.txt`
  for its declared controls, `det_eng_reload.txt` after a power cycle; the stats `engines` line
  (`eng_<id>=` per voice render, `eng_<id>_block=` per block: the budget is 580,499 a block for everything), `l1`
  (`leased`), `plugins` (`heap_largest`, `ctl_*`). Emulator figures; the device is 10-35 % higher.

## Report back
The control table before and after, every forbidden construct found and how it was replaced, the stereo approach
(dual-mono or true stereo) and the cost plan, the gate results with the exact stats lines, and what the owner must
do on the device. Mark anything unproven [I].
