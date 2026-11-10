# coop synth engines (P3, 2026-11-09)

Plugin synth engines for coop: a `.coop` file that adds an engine to the stock engine list (shift + T1), plays
through the stock voice loop and shows its own screen. The owner's decisions (2026-11-09): host it the way kennel
does (the hidden iter engine), add it to ABI v1 beside the effects, prove it with a small reference engine before
porting rings, and build plugin screens now rather than later. [I] until the op1emu gate and the device test; the
addresses are kennel 3.1's (`third_party/kennel/synth_sdk`, `incoming/hk/`) checked against `OP1_vdk.dis`.

## How it works

**The iter engine is hijacked**, as P2 hijacked the filter FX type for effects:
- R246 has a hidden synth engine, *iter* (instrument type 14), with no row in `synth_types`. Its voice vtable at
  `0x019103CC` has render at +0x14 (`0x019B99D4`), note on at +0x1C (`0x019B9B8A`), query at +0x24 (`0x019B9980`)
  and note off at +0x2C (`0x019B996C`) [C: OP1_vdk.dis]. The package points render, note on and note off at coop's
  dispatchers (`plugin_host.cc`); query stays stock.
- **The selector** is the slot's fifth knob word: `knobs[4] = 0x5300 + n` names engine plugin n. Anything else plays
  the stock iter voice. Numbers come from the same persistent map as the effects (`coopplugins`, 32..63, one
  namespace for both kinds).
- **The list.** At boot every loaded engine gets a `synth_types` row in the factory database: its type name (the id
  with `_` as a space) and `default_params = [d0, d1, d2, d3, 0x5300 + n, 0, 0, 0]`. The `instr_types` name compare
  at `0x019B975C` (`R3 = R0; P1 = R3`, [C]) becomes a JUMP.L through a region-C trampoline to `coop_instr_name_eq`:
  the table's "iter" entry also matches every name that is not a stock engine, so a plugin's name, a plugin that is
  gone and an unknown name all load as iter instead of asserting (`patch.cpp:390`). A sound whose plugin is gone
  plays the stock iter voice and keeps its name and selector, so it plays again when the file returns.
- **Voices.** The stock voice loop calls the iter vtable for each of the six voice objects (`0xFFB00000 + i * 0x24`);
  the dispatcher computes the voice index from `self` and calls the plugin with that voice's state. Everything after
  the render is stock or already coop's: the stock envelope and gain, LFO, portamento in the pitch words, and coop's
  voice hook, which places each voice in the stereo pair (stereo bus v2, S3). An engine therefore needs nothing
  stereo of its own.

## The ABI (`sdk/coop_plugin.h`, API 1, additive)

```c
#define COOP_ENGINE_MAGIC 0x4E454F43u                  /* "COEN" */
typedef struct coop_engine_t {
    uint32_t magic, api, flags;                         /* COOP_ENGINE_MAGIC, COOP_PLUGIN_API, 0 */
    uint32_t voice_bytes;                               /* per-voice state the host provides, zeroed, 32-aligned */
    int  (*init)(void);                                 /* main thread, once at load: tables. 0 = ready */
    void (*note_on)(int voice, void *state, int32_t velocity, const int16_t *params);   /* velocity Q15 */
    void (*render)(int voice, void *state, int16_t *out, const int32_t *pitch, const int16_t *params);
    void (*note_off)(int voice, void *state);           /* may be 0: the stock envelope releases */
    const coop_screen_t *screen;                        /* 0 = the stock iter screen */
} coop_engine_t;
```

- `render` fills `out[128]`, one block of the voice at the stock voice rate (88.2 kHz), mono int16. `pitch[128]` is
  the per-sample phase increment the stock computed for the voice (2^32 = one cycle a sample, bend, LFO and glide
  included): the engine never reads firmware addresses for it. There is no note number: like the stock engines and
  kennel's (rings latches `pitch[0]` each block), an engine takes its pitch from these words.
- `params[0..3]` are the four knobs as the slot stores them (0..32767 in the stock iter ranges); `params[4]` is the
  selector (the core's, never written); `params[5..7]` the free preset words. The v2 parameter store
  (guide §3.1) replaces this with declared parameters later, as for effects.
- Per-voice state: `voice_bytes` up to 8 KiB, six voices, from heap 0 at load (kennel's engines had 32 bytes in the
  voice object; rings-sized state needs more). The file is relocated once: engines share code and `.bss` across
  voices, which is what a global table or a shared resonator wants.
- `init` runs once on the main thread at load (kennel had none; the effect `init(slot)` is a per-position reset).
- The loader takes file kind 1 (synth) as an engine and checks magic, api and that every callback is inside the code.
- Cost: an engine's voice runs in the audio interrupt six times a block at most. The stats file's `engines` line
  gains `eng_<id>=avg/max/n`; the budget is the stock engines' (`docs/performance.md`).

## Screens (`coop_screen_t`)

The owner chose plugin screens in the first build. Kennel's model, ported:
- The `.coop` file may start with an SVG (the format already allows it: `svg_offset`, the leading screen). The
  stock SVG loader opens it straight from the plugin file, `../../plugins/<file>` relative to `content/display`
  (kennel's `svg_path`): no copy, no content change in the package.
- For every engine with a screen, coop builds a second object of the iter screen class (`SCREEN_CTOR 0x01A3BAB0`
  with screen id 0x14, the table entry restored after), copies iter's screen vtable (`0x019103FC`) and points its
  update and init entries at the plugin's callbacks, then loads the SVG (`SVG_LOAD 0x01A503D8`) and calls `init`.
- `ScreenId::for_synth`'s case 14 (data word `0x018F8074`, stock target `0x01A3BFD0`) points at a stub that swaps
  the engine's screen object into table slot 0x14 (or iter's own) for the slot on screen and returns 0x14.
- The plugin draws through `sdk/coop_screen.h`: find a node by id, set a transform, set line / fill colours, show or
  hide a node, plus the knob values and per-voice levels from the core. Kennel's address macros become core exports
  (`sdk/coop_exports.cc`), so a plugin never names a firmware address.
- Kennel found that swapping documents into the screen table leaks their drawing caches (R246 resets only documents
  reachable from the table, `kn_screens.c`); coop takes its frame-snapshot fix with the screens.
- Order at boot: the stock builds and loads its screens before coop's plugin scan runs (the tape mount). coop
  therefore builds the engine screens on the Gfx thread (`coop_screens_frame`, from coop_ui_frame), on the first
  frame after the scan once the stock iter screen has its document, and only swaps one in once its SVG is loaded;
  until then the engine shows iter's screen. [C op1emu, E2 below]
- The plugin draws through `sdk/coop_screen.h` (core exports, `sdk/coop_exports.cc`): find a node (in the screen's
  document, no assert: `0x019EB8AC`, as te-op1-cfw's `fx_screen.h`), set its matrix, show / hide it, its line / fill
  colour (0 for an id the SVG lacks: the stock getters assert), draw the SVG, a raster line, the shown slot's knobs.

## Milestones

- **E1, the host.** ABI, loader kind 1, the iter vtable and name-compare patches, `synth_types` rows, per-voice
  state, the stats line, and the reference engine `plugins/saw` (two detuned saws and a one-pole low-pass: brightness,
  detune, a sub octave, drift). Gate (op1emu): with no engine file, the chord scenario is sample-identical to p6k;
  with `saw.coop`, selecting "saw" from the engine list and playing a chord gives saws at the right pitches, six voices
  at once, no fault; a sound saved on saw reloads as saw; with the file removed it loads as iter.
  **E1 built and gated in op1emu (2026-11-09)** [C op1emu]:
  - Regression: `det_chord.txt` on the pristine NAND is sample-identical to p6k (3,306,014 frames). The same image
    built with `COOP_NO_ENGINE=1` (no iter / name-compare patches, same layout) is identical too. One early run of the
    E1 image landed a block late and matched nothing; its rerun matched both: run-to-run timing, not the build.
  - Select and play (`det_eng_select.txt`, `det_eng_note.txt` on a NAND with `saw.coop`): "saw" is in the engine list
    in alphabetical order (8 rows below digital); `seq` then `synth` closes the list; the stats say `engines 1`, every
    voice render goes to the plugin (`eng_iter 0`, `eng_foreign 0`). A probe build showed what the plugin is handed:
    `params[4] = 0x5320` (number 32), velocity word `127 << 8`, `pitch[0] = 6,370,030` = 130.8 Hz at 88.2 kHz (c4 with
    the pristine NAND's synth octave at -1), the plugin's own peak 11,715. The WAV's fundamental is 131 Hz with the
    sub square's odd harmonics (196, 458 Hz). Its level in the dump (peak 573) matches a stock engine in the same
    scenario (voltage, peak 727): op1emu's speaker path is quiet, not the engine.
  - Cost: `plugin_engine=6644/6740` cycles a voice render (op1emu), in the range of the stock fm voice. The voices
    the slot's mode starts all render: the pristine slot carried a unison mode, so one key started six voices.
  - Power cycle (`det_eng_reload.txt` on the NAND the select run left): the sound boots as saw, no list touched
    (2,304 plugin renders, `eng_iter 0`); `op1.db` slot 0 holds `"knobs":[20000,8000,6000,4000,21280,...]` and the
    map `f:saw=32,`.
  - The file gone (the same NAND, an image built with `--set-word g_coop_no_plugins=1`, which boots as if
    `/yaffs2/plugins` were empty): the sound loads as iter and plays iter's voice (2,304 iter renders), the page shows
    iter's dials, no assert.
- **E2, screens.** `coop_screen_t`, `sdk/coop_screen.h`, the SVG-in-file path, the case-14 swap, the frame fix;
  `saw` gets a screen (four bars and a waveform drawn from the knobs). Gate: op1emu screenshots of the engine page.
  **E2 built and gated in op1emu (2026-11-09)** [C op1emu]:
  - `saw` with its screen (`plugins/saw/saw.svg`, `saw_screen.cc`), packed with the SVG in front: the stats say
    `scr_built 1 scr_failed 0`; with saw selected the engine page is saw's drawing (`det_eng_select.txt`: the saws
    scaled by brightness, the second saw offset by detune, the sub square, the drift dot), drawn by the plugin's
    update (`scr_updates` counts its frames). `explore_eng_screens.txt`: saw, then string (string's stock page), then
    saw again (saw's page, the dot moved on): the case-14 swap follows the engine.
  - The order at boot [C op1emu]: the build runs on the first frame after the plugin scan with the stock iter screen
    loaded, as planned; one swap of slot 0x14, one document reset by the frame fix (iter's, when saw's took its place).
  - Regression: with no engine file the chord scenario matches p6k exactly up to 51.7 s and by at most 1 LSB after;
    the E2 image is identical to itself run to run, and the same image with `--set-word g_coop_scr_stock=1` (the stock
    frame path) is identical to p6k. That run reset no document (`scr_resets 0`): the only difference on that path is
    the frame copy each Gfx frame. [I] The extra Gfx work moves when some non-audio thread updates a value the audio
    reads (an LFO step, a smoothed knob), which lands 1 LSB apart; inaudible, and any change on the Gfx path would do
    the same. Not traced further.
  - Package `build/pkg/op1_246_coop_e2_engines.op1` (tag `e2`): E1 + E2; `saw.coop` separately.
- **E3, rings.** Port `te-op1-cfw/ports/knl/rings` with its screen (`/port`). Gate: its op1emu WAV against the
  kennel build's.
  **E3 built and gated (2026-11-10)** (`plugins/rings/README.md`; device test pending):
  - The planned gate (a WAV against the kennel build's) became a stronger one: hundekraft does not run in op1emu, but
    the kennel build compiled te-op1-cfw's own engine, so `tests/host/rings` builds that engine and the coop one on
    the host from one seeded call sequence (4,367 notes, every model x polyphony pair, steals, bends, knob jumps,
    idle gaps): bit-identical over 300,505 voice blocks; a 1-LSB change to a live constant is caught [C host].
  - op1emu (`build/e4c.ldr`, its stats tag `e4b`; a NAND with only `rings.coop`; `tests/sim/det_eng_rings.txt`)
    [C op1emu]: `plugins seen 1 loaded 1 failed 0`; rings selected from the engine list (7 rows below digital, 56
    steps); a chord in each of the six models (fn + blue, 4 Gray steps = 2 detents a step), polyphony 1 with fast
    notes, then 4: `eng_rings=15124/32128/118600`, `eng_rings_block=85742/153017/20920`, `eng_iter 0`, `ctl_turns 8`,
    `l1 ... leased 1056 leases 1 lease_refused 0 free 4160`, `heap_largest 8388608`, `scr_built 1 scr_failed 0`; op1.db
    slot 0 `[8192, 19660, 22937, 9830, 21280, 5, 3, 0]` (verb, polyphony 4); every model section sounds (RMS 445-685,
    distinct zero-crossing rates), no fault. `det_eng_rings_fn.txt` on the NAND it leaves: the sound boots as rings,
    and with Help held the screen brackets MODEL / POLY and dims the rings and rays.
  - A Help hold that turns nothing still opens help on its release (`help_taps 1`): the core's tap rule, the same for
    every engine.
  - Needed from the plugin tools: `--force-group-allocation` on the packer's `ld -r` (C++ COMDAT groups, as
    te-op1-cfw's `knl_pack.py`; saw and stereo_delay pack byte-identical with it, `docs/plugins.md`).
  - **Device, rings 0.1 (owner, 2026-11-10)** [C device]: the controls work well; the sound crackles often and the UI
    lags with one effect added. `eng_rings=32741/187416/760620`, `eng_rings_block=181951/415022/136868` (about twice
    op1emu's figures: op1emu models no instruction cache or SDRAM), the callback ~490 k of 580 k in busy windows.
  - **rings 0.2 (2026-11-10)**: the upsampler's half-band stage in assembly (bit-identical) and the control rate
    halved (the owner's choice). op1emu: `eng_rings=13412/30417/118592`, `eng_rings_block=76056/143676/20914` (-11 %),
    every model sounding at the 0.1 levels, `ctl_turns 8`, no fault. Profile, simulator figures and what is left:
    `plugins/rings/README.md` "Optimisation". **Device (2026-11-10): felt worse** (the tape recording):
    `eng_rings_block=173719/278457`, 5 % below 0.1; the callback peaked at 882 k.
  - **rings 0.3 (2026-11-10)**: four voices at most (Rings' polyphony 4; the steps are 4, 1, 2), 10 modal modes, and
    the control work batched once a block (fewer code fetches). op1emu (e4e): `eng_rings_block=55373/122785` (-27 %
    from 0.2), every model sounding, `ctl_turns 8`, no fault. Device: pending.
- Then on the device: one package for E1 + E2 + saw, the owner's verdict, then rings.

## e4: controls, L1, memory, screen exports, cost (2026-11-10)

From the rings port's request (`docs/request-engine-controls-l1.md`); everything below built and gated in op1emu, tag
`e4`, the device test pending.

**Controls** (`core/sdram/engine_ui.cc`, `docs/ui-features.md` §13, guidelines §4). An engine declares up to eight in
`plugin.json`:

```json
"controls": [
  {"id": "shape", "name": "shape", "action": "fn_turn", "encoder": "blue", "word": 5, "kind": "steps",
   "steps": ["saw", "square", "triangle"], "default": 0, "detents": 2},
  {"id": "sub_oct", "name": "sub", "action": "shift_fn_click", "encoder": "orange", "word": 7, "kind": "steps",
   "steps": ["-1 oct", "-2 oct"], "random": false}
]
```

- `action`: `click`, `shift_click`, `fn_turn`, `fn_click`, `shift_fn_turn`, `shift_fn_click` (turn and shift + turn
  are the stock knobs 0..3); `encoder`: blue, green, white, orange; `word`: 5, 6 or 7, one control each; `kind`:
  `steps` (2..16 named steps, the step index in the word) or `continuous` (Q15, turns only, 1 % a detent); `default`;
  `random` (default true: randomise draws it); `detents` per step on a turn (1..8, default 1); `name` and step names up
  to 8 characters from the stock glyphs, lower case. The field names follow the v2 parameter table (guidelines §3.1).
  e5: `wrap` (steps only, default false): a turn past the last step goes to the first, and back (rings 0.5's model and
  polyphony). It packs as an 11th field (`... name steps 1`); a core before e5 skips the rest of the line, so the same
  file loads there and its turns stop at the ends.
- `tools/plugin/pack.py` checks the declaration and packs it as extension tag 6 (text, one line per control:
  `action encoder word kind count default random detents name steps`); the loader refuses a malformed one with a
  reason the inventory shows. The control defaults go into the engine list's row (words 5..7).
- The engine reads `params[5..7]` (clamp: a sound may hold anything) and latches what it needs at note on.
- Why words 5..7 are safe: iter's range table, read from a running image (`COOP_PROBE_ITER`), is `{0, 32767, 10485, 0}`
  for words 0..3 and `{0, 32767, 0, 0}` for 4..7 [C op1emu]: the boot check accepts 0..32767 there (step indexes, Q15,
  the selector 0x5300 + n), and scale 0 means shift + turn is fine adjustment of 0..3, never a write to word 4.
- The word is written as randomise writes knobs (the live words, then `Patch::SetKnobs`), so it saves, reloads and
  randomises like the others.

**L1** (`core/sdram/l1_pool.cc`, `sdk/coop_plugin.h`):
- `void *coop_l1_lease(uint32_t bytes)`: from an engine's `init` only; zeroed, 32-byte aligned, kept for the session;
  up to 4096 bytes for all engines together; 0 when refused. Leases come down from the top of the L1 pool
  (`0xFF900820..0xFF902900`, 8,416 bytes) while coop's own blocks go up from the bottom (3,200 bytes with saw loaded:
  some are taken after the plugin scan), and a lease leaves coop at least 3,584 bytes. With saw (192 bytes leased) the
  stats say `free 5024`: room for rings' 4 KB and ~900 bytes for later effect plugins. Rings 0.4 leases 3,296 bytes
  (its per-render state, the modal modes, the soft-limit table: `plugins/rings/README.md` "0.4"), so beside saw the
  engines hold 3,488 of the 4,096; each part rings asks for falls back to SDRAM on its own when refused.
- `void *coop_l1_scratch(uint32_t *bytes)`: 1312 bytes of the L1 scratchpad at `0xFFB000E0`, per call, inside the
  engine's own `note_on` / `render` only. While a plugin engine is the live engine the stock voice objects are iter's
  six (`0xD8` bytes) and the effect objects start at `0xFFB00600`; the op1emu write tracer saw no store to
  `0xFFB000D8..0xFFB005FF` once saw was the engine (the last ones were the stock building the previous engines' voices
  while the list scrolled) [C op1emu]. Kennel rings used the same window for its ~0.75 KB per-call scratch.
- Not taken: the 16 KiB L1 sine table serves the stock fm / voltage engines and 27 retargeted readers (giving it back
  while a plugin engine plays would mean undoing those per engine change: not cheap); the 512 bytes at `0xFF907E00`
  (T4 plan) stay a reserve.

**Memory.** Heap 0's largest free block after the plugin scan is in the stats (`heap_largest`): 9,437,184 bytes with
saw; 8,388,608 with saw, stereo_delay and a test engine of 614,404 bytes of `.bss` (`tests/plugins/bigbss`; rings'
voice states and reverb are ~572 KB), which loaded and was zeroed at boot with no fault [C op1emu]. Rings fits with
room to spare.

**Screen exports** (`sdk/coop_screen.h`): `coop_scr_words(out8)` (all eight words of the shown slot) and
`coop_scr_mods()` (bit 0 shift, bit 1 fn), so a screen shows the held layer. The core draws the control's name and
value itself (above); a screen draws the state its own way.

**Cost.** The stats `engines` line now has, per plugin engine, `eng_<id>=avg/max/n` per voice render and
`eng_<id>_block=avg/max/n` for all its voices in one block (the budget is per block, 580,499 cycles). op1emu, saw,
six voices in unison: 7,396 / 7,491 per voice, 37,015 / 44,790 per block.

**Gates (op1emu, 2026-11-10)** [C op1emu]:
- Regression: `det_chord.txt` with no engine file is sample-identical to e3 (3,425,581 frames; one earlier e4 run
  landed a block late and its rerun matched). `det_eng_note.txt`: e3 with the old saw, e4 with the old saw and e4 with
  the new saw (L1 states, controls declared, shape 0) are sample-identical (681,638 frames, aligned on the onset);
  that scenario's first run of the day differs from its reruns, as the list scroll mixes wall and guest time.
- Controls (`det_eng_controls.txt`): fn + blue moved the shape one step (`ctl_turns 1`), fn + green (undeclared) did
  nothing (`ctl_ignored 1`), shift + fn + orange clicked the sub to -2 oct (`ctl_clicks 1`), a blue push with shift
  pressed before its release was cancelled (`ctl_cancelled 1`), Help tapped alone opened help (`help_taps 1`); op1.db
  slot 0 then holds `[20000, 8000, 6000, 4000, 21280, 1, 0, 1]`, and after a power cycle (`det_eng_reload.txt`) the
  same words, saw playing as square, the screen showing it. The popup drew 72 frames for the two changes (once each;
  an earlier build re-showed it every time CYCLES wrapped: fixed).
- L1: saw leases 192 bytes (`leased 192 leases 1`, coop `used 3200`); the record scenario `det_tape_rec.txt` with the
  lease and with every lease refused (`--set-word g_coop_l1_no_lease=1`, same NAND) is sample-identical (3,441,966
  frames). That scenario has two outcomes run to run in op1emu (pairs of reruns of one image differ by a block), so it
  was run several times and the identical classes compared.
- Memory: above. Screen: the shape icons light the chosen shape and turn dim blue while fn is held (sampled pixel
  `#31457b` while held, the dim grey after), the octave dots show -1 / -2.

**e4b (2026-11-10): the cache sync.** The device crashed at boot with saw 0.2 (undefined instruction in heap 0): the
loader never wrote back the data cache or dropped the instruction cache after relocating a plugin, and saw 0.2 is the
first plugin that runs (its `init` calls the L1 lease through a call stub) right after relocation. Fixed as kennel does
it (`plugin_host.cc code_sync`: FLUSH, SSYNC, IFLUSH, CSYNC over the image and its stubs, after every relocation). The
rings request's "the OP-1 runs without a data cache" (from `te-op1-cfw/ports/rings/src/scratch.h`) does not hold for
this: SDRAM is cached at least for these stores. L1 still matters for the same reason (an uncached SDRAM line costs a
bus wait).

**v1 (2026-11-10): the per-voice path in one section.** m1 showed an engine's code evicting coop's per-voice code
(the render thunk, LFO 2's stage 0, the counters, the bookkeeping, the dispatcher, the voice hook), which runs after
every render from SDRAM spread over 100 KB of region A. They now sit together in `.text.coop_voice` (`coop_cost.h`
`COOP_HOT_VOICE`, `coop_cb.ld`), 2,520 bytes from `0x02c00020`; cold branches are `noinline` outside it. Device A/B
against m1: `docs/device-tests.md` "m1b".

**v2 (2026-11-10): the per-voice path cut to 1,608 bytes.** m1b on the device: the grouped section helped engines up
to ~4-7 KB of code (rings sympathetic -33 k a block) and lost 12-24 k with 8-12 KB (`docs/m1-results.md` "m1b"): its
79 lines filled one way of sets 1-79, so an engine with 2-3 lines in every set, next to the stock's own per-voice SDRAM
code, overflowed the 4 ways there [I]. v2 makes the section smaller (51 lines, sets 1-51) and removes per-voice code
that sat outside it. Same audio, same stats:
- **The bookkeeping once a block.** `coop_eng_cost` (542 -> 180 bytes) and `coop_eng_render` (868 -> 476) only record
  each render (`g_vrec`, `g_prec`: function, cycles, the two counter deltas); `eng_fold` / `prec_fold` enter the
  records at the block's end, or when a list is full, in the same order with the same arithmetic (the 64-bit sums, the
  engine table lookup, first-of-block vs the rest).
- **The marks.** One out-of-line copy (`coop_mark_hot`, `coop_mark_hot1`) for the edges every image keeps (0x10 / 0x18,
  0x21). The gap profile's own edges (0x12, 0x14, 0x1C, 0x23) are a test of `g_coop_gap_prof` and a call to cold code
  (`coop_mark_prof_cold`, `coop_m1_thunk_in` outside the section, the synth thunk tests the word in assembly), so a
  release image runs none of their code; 0x14 / 0x1C no longer appear in a release image's `slow` lines.
- **LFO 2's `sync` once a block** (`lfo2.cc` `g_live`): it ran for every voice, reading the sound and the parameter
  words through code outside the section; inside a block they cannot change (the voice loop runs in the audio
  interrupt), and a note-on's own `sync` refreshes the cached answer.
- `voice_of`'s divide by 36 is a multiply by 1,821 and a shift (exact for every voice offset; the divide brought a
  64-bit multiply sequence).

Every call that leaves the section is now cold or conditional (objdump): the stock-sum fallback (stereo off, debug
compare), LFO 2's `process` (LFO 2 on), `sync_block` (once a block), the folds (once a block), `voice_start` (a note
on), the gap profile's marks (the profile on).

op1emu [C op1emu; no cache, so no gain can show there]: `det_chord` against the m1b image is identical over the whole
run in the m1 configuration (`COOP_M1=1`, `g_coop_gap_prof=1`; 2,055,504 frames) and in the release one (1,643,307
frames; one earlier pair differed from frame 1 by up to 7 LSB, and each image against itself differs the same way:
op1emu's two run-to-run outcomes). No fault. `run_m1_mode0.txt` lands all 17 sections; per block with six voices the
thunk is 0.74 k and the dispatcher 0.2 k cheaper, the hook 0.1 k dearer (the mark's call), and the block start 0.8 k
(a stock engine) to 1.4 k (a plugin engine) dearer (the folds): +0.2-0.3 % a block in op1emu, where every fetch is
free. On the device the folds run once a block from cold code; the per-voice path is what an engine's code evicts six
times a block. The device run decides (`docs/device-tests.md`).

## The 44.1 kHz voice (v1, 2026-11-10)

From `docs/request-rings-collateral.md` "Also on the table" and the rings README's levers.

**What happens today.** The stock voice loop wants each voice as 128 int16 samples at 88.2 kHz (`render`'s
`out[128]`). It applies the envelope, gain and LFO at that rate; coop's voice hook adds the voice into the stereo pair
at 88.2 kHz; then the half-band decimator (`coop_hb_decimate2`, the stock's taps: 32 on the even phase, one tap of
0.5 on the odd) and the DC block bring each side to 44.1 kHz (`docs/stereo-bus-v2.md` S3). An engine that computes
at 44.1 kHz or below must upsample to 88.2 kHz only for that decimator to undo it. Rings computes at 22.05 kHz: a
24-tap half-band to 44.1 kHz, then a 4-point Lagrange 2x stage to 88.2 kHz (`plugins/rings/rings.cc` `Upsampler`);
the README gives ~1.6 k a render for the 2x stage and the copies, and the fade and screen loops run on 128 samples.

**The contract (additive, API 1).** An engine opts in with both:
- `COOP_ENGINE_F_44K` in `coop_engine_t.flags` (`sdk/coop_plugin.h`): `render` fills `out[0..63]`
  (`COOP_VOICE_BLOCK_44K`) at 44.1 kHz. `pitch[]` is unchanged, 128 words at 88.2 kHz, so output sample i's
  increment at 44.1 kHz is `2 * pitch[2 * i]` (an engine that latches `pitch[0]`, as rings does, changes nothing).
- `"voice_rate": 44100` in plugin.json (default 88200). `tools/plugin/pack.py` reads the engine object's flags word
  and refuses a file where the two disagree, and refuses `voice_rate` on an effect.

**What the core does.** `coop_eng_render` (`engine_host.cc`) calls `coop_voice_44k_to_88k` (`voice_rate.cc`, in the
per-voice section) right after the render: every sample twice, in place, a hardware loop of `PACK` and one 32-bit
store a sample (~140 cycles). It is counted in the engine's own cost (`eng_<id>`); the stats line's `eng_44k` counts
the renders that went through it. Flags-0 engines take one more test of a word already in the cache line.
The loader (`plugin_host.cc`) now refuses only unknown flags. **A core older than v1 refuses a `COOP_ENGINE_F_44K`
file** ("engine flags not supported yet"): it does not load and play it wrong [C op1emu: the base image's stats say
`plugins seen 2, failed 1`, only tone88 loaded].

**Why repeating each sample is enough** [I, derived]. Repeat-by-two followed by the half-band decimator is linear and
time-invariant at 44.1 kHz: the voice comes out with no image and no alias, only a response of about
cos(pi f / 88,200) on top of the decimator's own: -0.14 dB at 5 kHz, -0.56 dB at 10 kHz, -2.4 dB at 20 kHz (the
envelope, which moves slowly, aside). Rings' Lagrange stage in the same place gives about -0.36 dB at 10 kHz, so for
rings (nothing above ~11 kHz: its half-band is -59 dB from 13 kHz) the difference is ~0.2 dB at the top of its band.
An engine whose top octave matters, or one native at 88.2 kHz, keeps the default.

**Gate (op1emu, `tests/sim/det_eng_tone44.txt`)** [C op1emu]: two test engines from one source
(`tests/plugins/tone_common/tone.h`): `tone44` (the flag, 64 samples) and `tone88` (flags 0, 128 samples), the same
parabolic sine from the stock pitch; c4 and c5 on each in the pristine slot (whose sound moves the pitch).
- v1 image: both load (`engines 2`), `eng_44k 40654` = tone44's renders, no fault. Per 0.5 s chunk the two engines
  give the same spectral peaks (the slot's pitch pattern: 116 / 132 / 64 Hz at c4, 276 / 392 / 524 Hz at c5) at the
  same amplitudes within the slot's own movement, the same high-frequency weight within 1 dB (no image, no block-rate
  artefact), and the same level: c4 RMS 108.3 / 108.1, c5 166.1 / 174.0 (-0.4 dB, the modulation's phase in the window).
- Cost (op1emu, no cache): tone44 791 cycles a render including the 2x, tone88 1,083. The first build's C loop
  cost ~320 more (a branch loop); the hardware loop gives the same output sample for sample.
- The base image (HEAD, no v1) on the same NAND: `plugins seen 2`, `failed 1`, `engines 1`: tone44 refused, tone88 plays.
- Flags-0 engines unchanged, base against v1 [C op1emu]: `det_chord.txt` identical over 2,356,174 frames to a base
  run in the same outcome class (op1emu's two run-to-run classes differ by 1 LSB throughout: base against base showed
  the same split), saw (`det_eng_select.txt`) identical, probe_alu identical. Rings 0.3 (`det_eng_rings.txt`, `eng_44k 0`) is
  not repeatable in op1emu even on one image (base against base: differences from 13.5 s, max 4,363, the same RMS);
  base against v1 the same picture.
- The per-voice section grows by 116 bytes (`coop_voice_44k_to_88k`, `0x02c00a08..0x02c00a6c`).
- Device: nothing to measure until an engine uses it (rings' adoption below); the 2x is 2 instructions a sample in L1, ~140 cycles a render [I].

**How rings adopts it** (fork B's change, not made here): in `plugins/rings/rings.cc`, set `COOP_ENGINE_F_44K` in
`ENGINE_rings` and `"voice_rate": 44100` in its plugin.json; `Upsampler::Process` keeps the half-band and writes
`out[k] = Saturate16(mid[k + 1])` for k = 0..63 (the even outputs it computes today, the same 44.1 kHz samples with
the same delay) instead of the 128-sample Lagrange loop; the fade (`AdvanceFade`, `fade_step`) and `ScreenRendered`
run over 64 samples, so a fade that should last as long takes twice the step a sample
(`voice_allocator.h` `kShortFadeSamples` 128 -> 64); `op1::kBlockSize` stays 128 where it means the stock block
(the pitch words). The host test (`tests/host/rings`) compares the 44.1 kHz samples against the te-op1-cfw engine's
even outputs, bit-identical by construction.

## Out of scope for now

Engines on the drum side (the drum voices are not engine objects, `docs/tracker.md` "Drums beside the synth"),
stereo voices (the voice hook already places a mono voice; a stereo engine needs its own flag later), engines that
use the sampler buffer or the input (kennel's `uses_sample` / `input`), and the v2 parameter store.
