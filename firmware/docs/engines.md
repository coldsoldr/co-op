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
- [I] Order at boot: the stock builds and loads its screens before coop's plugin scan runs (the tape mount). coop
  therefore builds an engine's screen object after the scan, from the main loop, and only swaps it in once its SVG is
  loaded; until then the engine shows iter's screen. To be confirmed in op1emu.

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
- **E3, rings.** Port `te-op1-cfw/ports/knl/rings` with its screen (`/port`). Gate: its op1emu WAV against the
  kennel build's.
- Then on the device: one package for E1 + E2 + saw, the owner's verdict, then rings.

## Out of scope for now

Engines on the drum side (the drum voices are not engine objects, `docs/tracker.md` "Drums beside the synth"),
stereo voices (the voice hook already places a mono voice; a stereo engine needs its own flag later), engines that
use the sampler buffer or the input (kennel's `uses_sample` / `input`), and the v2 parameter store.
