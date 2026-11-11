# Firmware request: where does the rest of the block go while rings plays? (2026-11-10)

From the rings port (`plugins/rings`, E3). This is a **measurement** request, not a feature: the plugin side has
run out of things it can prove from its own figures, and the next optimisation depends on the answer. Nothing here
should change what the device plays in a release image; the probes are test builds or switches. Read first:
`docs/performance.md` (p6a-p6i: the performance monitor, the event sweep), `docs/test-runner.md`,
`docs/engines.md` (e4), `plugins/rings/README.md` "Optimisation". Work on `dev`; commits as coldsoldr only, no
Claude attribution lines.

## What is known

Rings 0.3 on the device, rings alone, no effect, no recording (`coop_stats (23).txt`, owner, 2026-11-10) [C]:

| | cycles a block |
|---|---|
| callback, idle windows (nothing playing) | ~85,500 |
| callback, busy windows (rings, four voices of six rendering) | 326,000-360,000 (peak 603,130) |
| busy - idle | ~270,000 |
| rings' renders (`eng_rings_block`) | 148,180 avg |
| coop's voice hook (`hook_sum / blocks`) | ~20,400 |
| the recorder over idle (`rec_sum / blocks`) | ~+3,300 |
| **unaccounted** | **~95,000** |

For comparison, the p5a test run (`p5a_20261108-2155_run.txt`, p6i): six **voltage** voices, callback 158,407 a block
against 88,429 idle, voice renders 63,672: about 6 k unaccounted. Six **dsynth** voices with the delay: 286,528 a
block, renders 132,131 + delay 22,434, about 44 k unaccounted. So something costs far more beside rings than beside
a stock engine.

Also known:
- The stock audio DSP runs from **L1 instruction SRAM** (`docs/audio-graph.md`: voice loop `0xffa009a0`, per-voice LFO,
  envelope and gain, decimation, master stage `0xffa00e50`, tape mix `0xffa036c4`). L1 code does not go through the
  instruction cache, so rings' code cannot evict it. What can slow beside rings: coop's own audio code in SDRAM
  (voice hook, stage, recorder, stats, engine dispatch), the few stock SDRAM functions in the path (`l1v_push_lr`
  `0x019e1b3c`, the EQ coefficient builder), any SDRAM data the L1 code reads, and the SDRAM bus itself (the display
  DMA, the tape I/O).
- On the device rings' own render runs ~1.9x its GNU-simulator compute; the mode-0 counters (code-fetch stalls p0,
  data stalls p1, `synth_rest=20439/.../4158/3639`) explain about half of the difference. Store-side stalls (write
  buffer, store buffer) are not counted in mode 0.
- Rings touches 5-7 KB of its code a block while played with the knobs still, 8-10 KB while knobs move
  (`make -C tests/host/rings footprint`), and ~574 KB of SDRAM state: per voice its resonator or string delay lines
  (two 8 KB lines a sympathetic voice), plus the shared reverb (109 KB) in verb. Its stores to SDRAM are frequent
  (the modal mode states every block, the string delay lines every sample).

## What to measure

### 1. Split the callback into its stages, with the counters

Extend the cost table (`core/include/coop_cost.h`) with sites, each with p0 / p1 (`coop_cost_add_pf`), so the stats
`cost` line (and the test runner's sections) give per block:
- **the whole voice loop** (the stock `0xffa009a0` region from the first voice to the summed bus), so
  *voice loop - engine renders - hook* = the stock's per-voice work (LFO, envelope, gain, the iter dispatch);
- **the engine dispatch** around each plugin render (`engine_host.cc`: the thunk's own time outside the plugin);
- **the master stage** `0xffa00e50` (decimation, FX dispatch, pan-mix, EQ, comp, downmix);
- **the tape mix** `0xffa036c4` and **`l1v_push_lr`**;
- **the rest**: callback minus everything above.

Where a stock L1 region can't be timed at its edges without patching L1 code, say so and time the nearest coop
boundary instead (the callback's own CYCLES read at entry, the S3 stage hook, the voice hook), and give the split
that can be had. Mark each site [C] / [I] as the docs do.

### 2. Three probe engines: which resource is it?

Three small test engines (as `tests/plugins/bigbss`, not shipped), each rendering silence or a plain saw, six
voices, each loading one resource only, with a `plugin.json` word or a compile switch for the amount:
- **`probe_alu`**: burns a fixed number of cycles a render in registers (a counted loop, ~100 bytes of code, no
  memory): pure compute.
- **`probe_code`**: runs straight-line code of a given size once a render (4, 8, 12 KB of distinct NOP / ALU blocks,
  as the p6f `icache` probe): code footprint only.
- **`probe_sdram`**: reads and writes a given amount of its own `.bss` a render (16, 64, 128 KB in sequential and in
  strided passes, separate read-only and write-only variants): data cache and SDRAM traffic only.

Set each so its own render cost roughly matches rings' (~25 k a render, ~150 k a block), then compare the
**unaccounted** cycles (section 1) of each against rings and against a stock engine. Whichever probe reproduces
rings' ~95 k is the resource to attack; if none does, say so.

### 3. The counter modes that catch stores

Run the rings and the probe sections with `g_coop_pf_mode` 2 (processor-to-memory stalls, data stalls not hidden)
and 4 (store buffer full, write buffer full), as well as 0, and report p0 / p1 for the engine renders and each stage.
If the stall events need a re-check on this unit (the p6h sweep found the layout of PFCTL differs from kennel's),
use the p6h workloads to prove each event first.

### 4. One device session for the owner

Package the probes and the stage sites in one test image (a test tag, not a release), and a test-runner script
(`tests/sim/run_*.txt`, `docs/test-runner.md`) so the owner presses Run once and gets every section:
- idle (nothing playing), 10 s;
- a stock engine (voltage), six-note chord held 10 s;
- rings 0.3 (`plugins/rings`, `build/plugins/rings.coop`), modal, a six-note chord held 10 s; then sympathetic
  strings, the same;
- each probe at the matched cost, the same chord;
- rings modal again with the knobs turning (the control code running), 10 s.
Each section with its `cost` / `engines` lines and the stall columns; one run per counter mode (0, 2, 4), or a mode
switch per section if the runner can set it.

op1emu can gate that it all runs and that the sections land, but has no cache, SDRAM timing or counters; only the
device figures answer the question.

## Deliverables

- The stage sites, the probes and the runner script in a test build (tag e.g. `m1`), gated in op1emu (no fault,
  sections present, a pristine run unchanged), and a pending entry in `docs/device-tests.md` telling the owner
  exactly what to install, run and send back.
- After the owner's run: a short table in `plugins/rings/README.md` (or a note back to the rings port): for each
  section, the stages' cycles and stalls, and which probe matches rings' unaccounted cost. That decides the next
  step for rings: code (rewrite the control code compact), data / SDRAM (move hot state into the L1 lease or cut
  stores), or the platform (a 44.1 kHz voice interface in the core, below), or nothing more to win.

## Also on the table (not part of this request)

A **44.1 kHz voice** for engines: the core takes 64 samples at 44.1 kHz from an engine that declares it and does the
2x itself (or mixes it straight into the decimated bus). Rings computes at 22.05 kHz and upsamples 4x to 88.2 kHz
only for the stock to decimate the summed bus back to 44.1 kHz: its 44 -> 88 kHz stage is ~1.2 k a render (~5 %).
Size it against the answer above before building it.


## Answer (2026-11-10): m1 built and gated in op1emu; the device run is pending

All four parts are built (`docs/performance.md` "m1", `docs/device-tests.md` "m1"):
1. **The split.** Every runner section has a `stages` line: the whole callback, the DSP, the synth voice loop, the
   drum loop, the master stage (both call sites), `l1v_push_lr`, the callback's last call `0xffa036c4`, and per
   voice coop's thunk, the render, the plugin's own render and the hook, each with p0 / p1 per block. They are timed
   at the stock's own call edges (L1 call sites retargeted through trampolines, test image only), so the stock's
   per-voice work (`voices - voice_coop`) and the dispatcher (`render - plugin`) are measured, not inferred.
   `tools/m1_table.py RUN_FILES...` prints the table and the collateral per stage against the idle section.
   The tape mix: `0xffa036c4` (docs/audio-graph.md's suspect) is the callback's last call, made on its way out
   (`0xffa048fc`) [C bytes], timed as `cb_tail`; if the track mix is not there it is inside `dsp` (`dsp_rest` or the
   master stage), not split further in m1 [I].
2. **Probes** `probe_alu`, `probe_code`, `probe_sdram` (`tests/plugins/`), each held at 25,000 cycles a render by a
   wait on CYCLES, so their renders match rings' and only the collateral differs.
3. **Counter modes** 0, 2, 4: one runner script each (`#@pfmode`, a new runner event).
4. **One session**: `run_m1_mode0/2/4.txt`, 4 min 33 s each, every section the request lists.

op1emu (no caches) gives ~21.6 k of collateral for every engine, rings included: 19.4 k of it is the stock's own
per-voice work for six voices, which the request's "unaccounted" figure included. The device run decides the rest;
the table goes into `plugins/rings/README.md` when it is back.


## Result (device, 2026-11-10)

`plugins/rings/README.md` "m1: where the collateral goes" (tables: `docs/m1-results.md`). Against `probe_alu` (the same
render cost with no memory and no code), rings modal adds 23 k a block outside its renders and sympathetic 47 k. The
code probes reproduce it (4 KB +33 k, 12 KB +47 k), mostly as coop's own per-voice code (render thunk, LFO 2 stage 0,
bookkeeping, voice hook) being fetched again after every render; the write probe gives the rest of rings' pattern
(+10-12 k in the stock DSP outside the voices). Reads and region size matter little. Next: keep the core's
per-voice path resident (a core change, every plugin engine gains), then rings' code per render and its hot stores.
