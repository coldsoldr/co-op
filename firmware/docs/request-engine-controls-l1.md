# Firmware request: engine controls and L1 for engine plugins (2026-11-10)

From the rings port (`/port rings`, E3 in `docs/engines.md`). The owner stopped the port before any code was
written: rings was already at the edge of acceptable performance on hundekraft, so it is ported only once the core
gives it the best possible chance. This document is the core work that has to land first. The plugin side
(`plugins/rings/`) is not part of this request; it follows once the items below are built and gated.

Read first: `CLAUDE.md`, `docs/brick-safety.md`, `docs/engines.md`, `guide/plugin-guidelines.md` §2-4,
`docs/plugin-performance.md`, `docs/performance.md` (T4a / T4c, the L1 pool). Work on the `dev` branch; commits as
coldsoldr only, with no Claude attribution lines.

## Why: what the kennel build relied on

The kennel rings (`te-op1-cfw/ports/knl/rings/`, the engine `te-op1-cfw/ports/rings/op1/engine.cc` with
`-DOP1_KENNEL`) reaches the firmware in ways coop forbids (guidelines §2.1, no firmware addresses in a plugin):

| Kennel rings did | Address | coop today | Needed |
|---|---|---|---|
| Pitch words | `0xff801950` | `pitch[]` argument | nothing |
| Voice index from `self` | `0xffb00000` | `voice` argument | nothing |
| Knob words incl. page-2 words 5, 6 | `0xff8013c4` | `params[0..7]` argument | nothing |
| Model / polyphony edited by a knob press (kennel page-2) | kennel core | **no engine control beyond the four turns** | item 1 |
| Per-call scratch in the L1 scratchpad | `0xffb000e0` (~0.75 KB at 22.05 kHz) | **no L1 for plugins** | item 2 |
| Big state (6 voice states + reverb, ~572 KB) at a fixed SDRAM page | `0x02C00000` | plugin `.bss` in heap 0 | item 3 (check) |
| Screen: page state, all 8 knob words | `op1_eng_page`, `op1_synth_knobs` | `coop_scr_knobs` (4 words) | item 4 |

The OP-1 runs without a data cache (`te-op1-cfw/ports/rings/src/scratch.h`). The mode bank writes its output
buffer once per mode per sample and the string kernels sum into theirs once per string per sample, so that buffer
in SDRAM instead of L1 costs every access a bus wait (Opus measured ~107 cycles per 32-byte line touched outside
L1). Rings has no headroom for that.

## Item 1: engine controls (the owner's control model)

### What the owner wants

On an engine plugin's page (T1), each encoder has these actions:

| Action | Today | Request |
|---|---|---|
| Turn | stock: knob words 0..3 | unchanged |
| Shift + turn | stock fine adjustment | unchanged: verify it applies to a plugin engine's page as to any stock engine (1/16 of a coarse step, guidelines §4.1) |
| Click | ignored by stock on synth pages | dispatched to the plugin's declared control |
| Shift + click | ignored | dispatched |
| **fn + turn** | not dispatched | **dispatched (new layer)** |
| **fn + click** | not dispatched | **dispatched (new layer)** |
| Shift + fn + turn | not dispatched | dispatched |
| Shift + fn + click | not dispatched | dispatched |

fn is the Help key held (`coop_fn_held()`, `coop_tape_ui.cc`: a tap still opens help, a hold used with another
control is fn). fn + turn and fn + click are **new layers beyond the guidelines' six actions**: update
`guide/plugin-guidelines.md` §4 (the action table becomes 4 x 8: turn, click, shift + turn, shift + click,
fn + turn, fn + click, shift + fn + turn, shift + fn + click) and §3.2 (the README table's rows), and record the
owner's decision in `docs/ui-features.md`. Check that fn + turn on T1 doesn't collide with anything coop or stock
already does with Help held (the bend, the drum pan, LFO 2, the effect pages); say what you found.

Rules from the guidelines that still hold: clicks act on **release**, so a click that becomes part of a shift / fn
combo never fires the plain click; every layer is held (no hidden mode); colour follows the encoder; steps never get
fine adjustment; the LFO never moves a steps control.

### Rings' use of it (the first customer)

| | Blue | Green | White | Orange |
|---|---|---|---|---|
| Turn | structure | brightness | damping | position |
| Shift + turn | fine | fine | fine | fine |
| fn + turn | **model** (6 steps: modal, sympathetic strings, string, FM, quantized sympathetic strings, string + reverb) | **polyphony** (4 steps: all, 1, 2, 4) | — | — |
| Click, shift + click, fn + click, shift + fn + turn, shift + fn + click | — | — | — | — |

The engine latches model and polyphony at note on, so a change applies from the next note and never clicks a
ringing voice.

### How the core learns a plugin's controls

The full ABI v2 parameter store (guidelines §3.1) is not required for this. A minimal, forward-compatible subset is:

- **Declaration in `plugin.json`**, packed by `tools/plugin/pack.py` into a new extension tag of the file (the
  format already has an extension table: claims, author, version, cost). Per control: the action (one of the eight
  above) and encoder, the storage word, the kind (`steps` or `continuous`), the step count, the default, and the step
  names (8 characters, stock glyphs) for the screen. Use the guidelines' field names (`id`, `name`, `action`, `kind`,
  `min` / `max` / `default`, `steps`) so v2 can read the same table later.
- **Storage: the slot's free knob words 5, 6, 7** (word 4 is the core's selector and is never written). They are
  already saved with the sound (`op1.db` slot knobs carry all eight words, `docs/engines.md` E1 reload) and already
  passed to `note_on` and `render` as `params[5..7]`. Three words is the ceiling for now; design the declaration so
  more controls go in the sound's `"coop"` member when v2 lands, without changing the plugin's view.
- **Encoding:** store the step index (0..n-1) in the word for `steps`; Q15 for `continuous`. The plugin clamps
  what it reads (a sound written by kennel or by hand may hold anything).
- **Dispatch:** in `coop_ui_knob` / the key handler, on T1 with the slot's engine a plugin (`params[4]` = 0x5300 + n
  and `coop_engine_at(n)` non-zero): look the action up in that engine's table; a declared steps control moves one
  step per detent (or per `detents_per_step`) on a turn and one step per click (cycling) on release; an undeclared
  action does nothing and is consumed, so it can't fall through to a stock behaviour on that page. Write the word the
  way the stock writes a knob word, so it is saved, undone and randomised as the others are (check
  `docs/ui-features.md` "randomise": steps words should randomise within their count, or not at all; say which).
- **Velocity, MIDI CC:** out of scope; note what would be needed.

## Item 2: L1 data for engine plugins

The core already has the L1 pool (`core/sdram/l1_pool.cc`, `coop_l1.h`): the stock Lander buffers in L1 data B,
a bump allocator from `0xFF900820` to `0xFF902900` (8,416 bytes, 2,976 used by the stereo stages and the record tap
at T4a: about 5.4 KB free). Request:

- **An export** (`sdk/coop_exports.cc`, append-only) that gives a plugin a zeroed, 32-byte aligned L1 block, callable
  from the engine's `init` (main thread, at load) only: e.g. `void *coop_l1_lease(uint32_t bytes)`, returning 0 when
  the pool is refused or full, so the plugin keeps an SDRAM fallback. Never from the audio interrupt. A file loaded
  at boot keeps its block for the session (nothing is freed today; that is fine).
- **Or** an `l1_bytes` field the core allocates at load and passes to `init`, if that suits the ABI better. Either
  way, document it in `sdk/coop_plugin.h` and `docs/engines.md`.
- **Size:** rings needs ~0.75 KB for its per-call scratch at its 22.05 kHz build (`scratch.h`: render_out, wet
  share, and a union of the modal / string / upsampler buffers). Allow at least **4 KB** to one engine so the plugin
  can also move its hottest per-voice data into L1 (the mode bank's state and coefficients are a candidate). Report
  in the stats what was leased (the `l1` line) and what remains.
- **Budget the pool.** With ~5.4 KB free, say how the pool is shared between coop's own stages, future effect
  plugins and the engine. If more L1 is wanted, look at the options and report rather than take them blind:
  - the spare L1 scratchpad after the iter voice objects (`0xffb000d8..0xffb005ff`, which kennel rings used; the
    effect objects start at `0xffb00600`, LFO objects at `0xffb00a00`). Confirm whether coop or the stock uses any of
    it while an engine plugin plays, and whether a stock engine with larger voice objects can overlap it;
  - the 16 KiB L1 sine table (T4c) only serves the stock FM / voltage engines. Say whether it could be given back
    while a plugin engine is the live engine (likely not cheaply: 27 retargeted references); just report.
  - the 512 B at `0xFF907E00` (`docs/performance.md` T4 plan).
- Add the lease to `guide/plugin-guidelines.md` §2.3 / §7.1 (`op1_l1_claim` → the coop lease, no longer *proposed*).

## Item 3: memory for a large engine (check, fix only if needed)

Rings' `.bss` is ~572 KB (six `RingsVoice` states and the shared reverb), relocated once (engines are relocated
once, `plugin_host.cc` "E1"). The loader stages the file in a 1 MiB block from heap 0, then mallocs the image + bss.
- Confirm heap 0 can take a ~620 KB block at boot **with the other plugins and the stock delay / spring / grid
  tanks already allocated**, in op1emu, and report the largest free block (a stats field if there isn't one).
- Confirm the loader zeroes `.bss` of that size at boot without tripping anything (the loader clears with word
  stores; ~570 KB is ~143 k stores on the main thread).
- `voice_bytes` stays at 0 for rings (its voice states move between voices on a steal, so they are one array in
  `.bss`); nothing to change there.

## Item 4: screen exports

Rings' screen (`te-op1-cfw/ports/knl/rings/rings_screen_knl.cc`) shows the MODEL and POLY rows and highlights them
while that layer is held. coop's `coop_scr_knobs` hands only words 0..3. Request:
- an export for all eight words of the shown slot (or 0..3 live after the LFO plus 5..7 stored, as kennel had);
- an export for the held modifiers (shift, fn) so a screen can show the held layer (guidelines §4: "the screen shows
  a layer while its modifier is down");
- if the core draws step names for declared controls itself (as v2 plans, §3.3), say so, and the plugin won't.

## Item 5: cost visibility (check)

The stats `engines` line has `eng_<id>=avg/max/n` per engine (E1). Confirm it is per voice render, and add a
per-block total for the engine (all six voices in one block), since rings' worst case is six ringing voices at once
and the budget is per block (580,499). Kennel rings had internal limits (`coefficient_budget.h`, voices sleeping
after 8 silent blocks); nothing core-side is needed for those.

## Gates (op1emu, one run at a time)

- **Regression:** with no engine file, `det_chord.txt` on the pristine NAND is sample-identical to the e3 image
  (as the E1 / E2 gates did). With `saw.coop` only, `det_eng_select.txt` / `det_eng_note.txt` unchanged.
- **Controls:** a test engine (extend `plugins/saw`: declare, e.g., a fn + blue steps control and a shift + fn +
  orange click, and make the sound show it) and scenarios that turn and click each layer: the word moves by the
  declared steps, wraps on a click, is saved and reloads after a power cycle (`det_eng_reload.txt` style), a plain
  click doesn't fire when it ends a shift / fn combo, an undeclared action does nothing, and a tap on Help still
  opens help. Shift + turn on the engine page still gives fine adjustment.
- **L1:** the saw test engine leases a block, the stats `l1` line shows it, the pool's other users are unchanged
  (the stereo stages and the record tap stay sample-identical on the chord and record scenarios).
- **Memory:** a dummy engine with ~600 KB of `.bss` loads beside the other plugins; report the heap figures.
- **Screen:** the saw screen shows the declared control's value and the held layer.

## Deliverables

- Core changes with a new package tag (e4 or next), built, gated in op1emu, the stats lines quoted.
- `sdk/coop_plugin.h`, `sdk/coop_screen.h`, `sdk/coop_exports.cc` updated (append-only exports).
- `tools/plugin/pack.py` packs the control declaration; `docs/engines.md`, `guide/plugin-guidelines.md`,
  `docs/ui-features.md` updated.
- A pending entry in `docs/device-tests.md` for the owner: what to flash, what to turn and click, what to listen for,
  the `l1` and `engines` lines to copy from `/yaffs2/user/coop_stats.txt`.
- A short note back to the rings port: the final names of the control declaration, the L1 export and the screen
  exports, and how much L1 an engine may take.

Mark anything not seen in bytes or on the device [I], as the docs do.


## Answer (2026-11-10, e4 built and gated in op1emu; device test pending)

Everything above is built: `docs/engines.md` "e4" has the details and the gate results. For the rings port:

- **Controls:** declare them in `plugins/rings/plugin.json` "controls". Model: `{"id": "model", "name": "model",
  "action": "fn_turn", "encoder": "blue", "word": 5, "kind": "steps", "steps": ["modal", "sympath", "string", "fm",
  "qsympath", "str+rev"], "default": 0, "detents": 2}`; polyphony: `{"id": "poly", "name": "poly", "action":
  "fn_turn", "encoder": "green", "word": 6, "kind": "steps", "steps": ["all", "1", "2", "4"], "default": 0}`. Step names
  are at most 8 characters from the stock glyphs (lower case); pick the final ones. The engine reads `params[5]` /
  `params[6]` as the step index (clamp them) and latches them at note on, as it does. The core shows "MODEL STRING"
  etc. itself; randomise draws both unless you add `"random": false`.
- **L1:** `void *coop_l1_lease(uint32_t bytes)` (sdk/coop_plugin.h), from the engine's `init` only, up to 4096 bytes
  for all engines (rings is the only one: take the 4 KB if the mode bank state fits), 0 when refused: keep the SDRAM
  path. `void *coop_l1_scratch(uint32_t *bytes)`: 1312 bytes at 0xFFB000E0, per call, inside `note_on` / `render` only
  (kennel rings' `0xffb000e0` window, confirmed free while a plugin engine plays). Rings' ~0.75 KB per-call scratch goes
  there; its per-voice hot data in the lease.
- **Memory:** a 614 KB `.bss` engine loads beside saw and stereo_delay with 8 MB left in heap 0 (`heap_largest`).
- **Screen:** `coop_scr_words(int16_t out8[8])` (all eight words of the shown slot) and `coop_scr_mods()` (bit 0 shift,
  bit 1 fn) in sdk/coop_screen.h; draw the MODEL and POLY rows from words 5 and 6, highlight them while fn is held.
- **Cost:** the stats `engines` line gives `eng_rings=avg/max/n` per voice render and `eng_rings_block=avg/max/n` for
  all its voices in a block.
- Shift + turn stays fine adjustment on an engine's page (iter's range table, confirmed).
