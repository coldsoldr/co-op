# The plugin bridge: other firmwares' plugin files on coop (2026-11-20)

The roadmap item: coop loads `.knl` (hundekraft's kennel) and `.opmod` (Opus 2) plugin files as they are, "similar to how
the other firmwares do it". This document is the research, the design, the survey of the files in the wild, the plan
(B1 .. B9, each with its host test and its op1emu gate) and the record of B1, which is built and host-tested. Nothing
of the bridge is linked into the core yet. [C] confirmed in bytes or by a test here, [I] inferred.

## Decisions for the owner (recommended option first)

1. **Where the bridge lives.**
   - *In the core, files unmodified* (recommended). It is what both others do (below), it keeps the author's file and
     name intact, and a refusal happens on the device with a reason. Cost: core code (B1's four files are 13.8 KB of
     Blackfin text, most of it the import table's names and notes, which a device build can drop) and core phases B2 .. B8.
   - *On the site, converting `.knl` to `.coop`*: not possible as a header rewrite. A `.coop` must export a `coop_fx_t`
     / `coop_engine_t` with coop's call signatures; a `.knl` exports kennel's `op1_fx_t` / `op1_engine_t`. A converter
     would have to append wrapper code and hand out a modified binary under the author's name, which the Opus kit's
     licence terms and plain etiquette both speak against. The site's part is checking and labelling (section 9).
   - *Both*: nothing the site could add that the core does not need anyway.
2. **Which format first.**
   - *kennel effects, then kennel engines, then Opus* (recommended): a `.knl` is coop's own file format (the loader is
     kennel's), kennel's inlines address the same R246 the stock OS still is on coop, and 42 of the 48 kennel effect and
     engine files load with shims alone. Opus needs an adapter per service (registration, parameter limits, pages).
   - *Opus first*: its effect ABI is the better fit for coop (stereo, host-owned state), but only 1 of the 8 Opus
     files here loads without the library file `opus.orbit3d`, and its engines need services coop lacks.
3. **Memory for bridged effects.** A mono kennel effect is relocated twice (one a channel, owner's dual-mono decision
   of 2026-11-01), so an effect costs 2 x its load size against the 1,536 KiB budget.
   - *Count it as it is, refuse `op1_fx_big`* (recommended): all 17 loadable effects together are 922 KB, a realistic
     handful far less; `rackverb` and `steve` (which want kennel's 1 MiB lease per position) stay refused by name.
   - *Serve `op1_fx_big` with a smaller lease*: both files read the size, but a short reverb tank is a different
     effect; decide after hearing B2.
4. **FX 2 positions.** kennel effects know three positions (synth, drum, master).
   - *Bridged effects at FX 1 only* (recommended): the FX 2 lists do not offer them. No extra memory.
   - *All five positions*: two more relocations per effect and two more position buffers; the position argument must be
     remapped per call. Only worth it if FX 2 without bridged effects feels like a hole.
5. **A type name coop already has** (coop's `tape_echo` port beside kennel's original file).
   - *coop's own plugin keeps the name, the bridged file is refused* "name taken by a coop plugin" (recommended): a
     saved name never means two things (kennel's rule D4).
   - *Suffix the bridged one* (kennel's `_o`): both load, but a sound saved on kennel no longer finds its effect by
     name, which is the point of the bridge.
6. **Files nobody tested.**
   - *Load anything that passes the checks, mark it "bridged, not tested" in the inventory and on the site, keep a
     crash stage marker* (a file whose start crashed the boot is skipped the next boot, as kennel does) (recommended).
   - *Allow-list by sha256*: only files a gate has run load. Safest, and every new plugin release waits for a coop
     release.
7. **Opus scope.**
   - *Effects (B6) and plain engines (B7); library files, LFO types, sequencers, drum engines and features refused by
     name* (recommended). Of the 8 Opus files here that is bitcrush and dimension; chorus, plate and sim need the
     library `opus.orbit3d` (a further phase, B9 with the pages), dx needs file access and fast memory.
   - *Leave Opus out*: the owner's own plugins already ship as `.coop`; the third-party Opus pool is small.
8. **op1r's `.op1p`.** Same entry objects as kennel (kennel is op1r's descendant), another small file format (kennel's
   `plugload/`, 110 lines, GPL). 53 listings in the catalogue.
   - *Add it after B4 as B5b* (recommended) / *leave it out*.

## How the others do it

**hundekraft (kennel 3.3, 2026-11-17)** [C source: `hundekraft-3.3.1-source/ext/kennel/synth_sdk/opus/`, `kennel/
kn_foreign.h`; local copy in `../../te-op1-cfw/incoming/hundekraft/3.3.1/`, gitignored]. In the core, in two layers.
- A neutral layer, `kn_foreign.h`: two weak boot hooks (`kn_foreign_need` counts the arena before it is taken,
  `kn_foreign_boot` runs after every `.knl` loaded), and inside the boot stage a transaction per file: `kn_foreign_begin`,
  `kn_foreign_alloc` (arena memory), `kn_foreign_sync` (D-cache flush, I-cache invalidate), `kn_foreign_add` (registers
  an engine or effect under a saved name), `kn_foreign_commit` / `kn_foreign_rollback`. A saved name never means two
  things: a foreign registration is refused when the name exists, and the bridge then picks `<name>_o`.
- The Opus folder (5,000 lines): `opus_load.c` checks a whole `.opmod` before a byte is written and relocates it;
  `opus_imports.c` is the import table (61 `fw_*` firmware names and the `opus_*` services as shims); adapters turn each
  registration into a kennel object through stamped trampolines (`opus_fx.c` effects, `opus_engine.c` engines,
  `opus_drum.c`, `opus_lfo.c`, `opus_seq.c`); `opus_saved.c` rewrites sounds saved on Opus into kennel's form before
  every parse; `opus_quirk.c` holds per-file handling keyed by id, version and the file's sha256.
- Refusals are named: places (patches of R246 code), a missing library, an unknown import ("needs <name>"), a file
  that crashed its start last boot (`/yaffs2/opus/.stage`, `.crashed`). The file's code is never patched in memory
  (their reading of the kit's licence): an accelerator is called instead of the file's render, through the bridge's
  own pointer.
- The workshop site only checks and counts: `opus.js` reads the `.opmod` header, `opmodArena` repeats
  `kn_foreign_need` (6,144 bytes a file + image + zero fill, 20 KiB more for an LFO type), `release.json` carries
  `foreignArena` for bundled files, and the device says in HELLO (capability 128) that it takes them.

**Opus 2** [C its README]: the reverse bridge is itself a plugin, `opus.bridge` (alpha), which runs op1r's `.op1p`
and hundekraft's `.knl` synths, effects and sequencers. Source not published; nothing more is known here.

**kennel and op1r**: kennel loads op1r's `.op1p` natively (`plugload/`), the entry objects being the same.

**The owner's own adapters** (`../../op1-plugins/adapter/copa_*.c`, `kennel/cokn.cc`) run coop plugins on Opus and on
kennel: the same mapping read the other way, and the reference for the parameter and screen translations.

Everyone loads the file unmodified at boot and adapts in code. Nobody converts files.

## Architecture

Two layers, as kennel: the loader and host stay format-neutral, a folder holds everything that names another format.

- `core/bridge/` (new; nothing else may name kennel's or Opus's services). B1: `bridge_imports.def` (the table),
  `bridge_knl.cc` (header, verdict, exports, address audit), `bridge_opmod.cc` (the `.opmod` checks and verdict),
  `bridge_fx.cc` (the effect adapter), `bridge_shims.cc` (the stand-ins).
- **A `.knl` goes through coop's one loader.** The format is byte-compatible but for the magic, so
  `br_knl_normalise` rewrites the header in the staging block (magic `KNPL` to `COOP`, the API word, kennel's extension
  tag 7) after checking kennel's size, CRC and API, and redoes the CRC; `kn_parse` and `kn_relocate_far` then run
  unchanged, against the bridge's export table instead of `coop_exports`. Only header bytes change, never code. Two
  traps the normalisation exists for [C test]: kennel's flag `0x20` is its own-envelope engine and coop's scratch
  effect; kennel's extension tag 7 is its settings rows and coop's scratch size (a bare magic swap makes `kn_parse`
  say "scratch flag and size disagree", or worse, take a size from settings text).
- **The host calls adapters, not the file.** A bridged plugin's record carries a `br_fx_inst` (or, B4, an engine
  record); `plugin_host.cc` calls `br_fx_init` / `br_fx_render` where it calls `fx->init` / `fx->render` for a coop
  file. No stamped trampolines are needed: the host knows which record it is running.
- **An `.opmod`** keeps its own loader (B6: `ol_place`'s equivalent after `br_opmod_check`), then `opl_init` runs
  inside a transaction and its registrations become coop records.

### Effects: mono, stereo, state

kennel: `render(pos, int16 buf[64], params)`, mono in place at 44.1 kHz, state per position 0..2; with the stereo flag
also `render_stereo(pos, l, r, params)`, which kennel calls at the master only. coop: `render(slot, int32 *l, int32 *r,
params, n)`, the int16 bus widened by 16 bits. `params[0..3]` are the filter type's range units on both [C headers],
`params[4]` the selector on both.

- `br_fx_render` narrows both channels to int16 (rounded and saturated: exact for the widened bus [C test]), calls the
  file, widens again. A block that is not 64 samples, or a position past the master, passes dry and is counted.
- **Dual-mono generically: two relocations of the file**, left and right, so every static of the effect exists twice.
  That covers state in `.bss` (snowglobe: 153 KB) without knowing the file.
- **State in R246's position buffer** is the part an import table does not show: `op1_fx_scratch(pos)` is a static
  inline reading the stock table at `0x01AB8BA0` (14 of the 19 effect files carry that address [C survey]). The left
  instance uses the stock's buffer for the position, as on kennel; around every call into the right instance the
  bridge swaps the table's entry to a second buffer and puts it back [C test on a host table; I on the device: that the
  stock effect of a position does not touch its buffer while a plugin plays there]. The second buffer is coop's own
  position buffer for that slot (176,460 bytes wanted, `COOP_FX_SCRATCH_MAX` is 180,224): a bridged effect that
  reaches the table makes the five buffers that size, 882,300 bytes against 673,920 today (+208 KB of heap 0, outside
  the per-file count, once).
- A stereo-flag file at the master gets one `render_stereo` call (true stereo, as on kennel); at the synth and drum
  positions its mono render once a channel.
- Cost: a mono effect runs twice. kennel's files carry a declared cost (extension 5, bcore packets, tools only); the
  gate measures each on op1emu (B2).

### Engines

coop's engine host is kennel's mechanism [C `engine_host.cc`: iter's voice vtable, the six voice objects at
`0xFFB00000 + i * 0x24`, pitch words `0xFF801950`, velocity `0xFF801064`]. A plain kennel engine (`note_on(self,
params)`, `render(self, out[128], params)`, `note_off(self)`) reads exactly those through its inlines, so the adapter
is three thin calls with `self` = the voice object. One relocation, no dual-mono (coop places the voice in stereo
after it). Extended entries (own envelope, voice-sum block, own T2 / T4 pages, input, service: flags `0x20` .. `0x800`)
need core services coop does not have: B8.

### Screens

A kennel screen is coop's: the SVG leads the file, `op1_screen_t` is `{init(scr), update(scr, dt)}` like
`coop_screen_t`, and the node and raster calls are static inlines calling R246 by address, the same addresses coop's
`coop_scr_*` exports wrap [C: `0x01A3B580` find, `0x019EAD58` transform, `0x01A3B4C8` draw, `0x01A3B490` raster,
`0x01A24A4C` line]. What is imported is kennel's helper library (`sq_text`, `sq_line`, `sq_num`, `sq_cat`, `sq_dot`,
`op1_screen_draw_cached`), which B3 vendors from kennel (GPL, `seq/seq_text.h`). B2 loads such a file with the stock
filter page: its screen object is not registered, so the stand-ins are never called.

### Addresses a file carries

kennel's SDK reaches R246 through inlines, so a `.knl`'s code holds firmware addresses as immediates. "Every import
is mapped" does not make a file safe; the addresses must also mean on coop what they mean on kennel. Blackfin has no
literal pools (a 32-bit constant is `reg.H = imm16` with `reg.L = imm16`), so one walk of the text finds them
(`br_knl_audit`, `formats.scan_abs`; in all 80 files the walk decodes every relocated immediate where the relocation
table says one is [C]). `bridge_imports.def` lists the 38 addresses kennel's headers use that are R246's own.
- Known and the stock's own on coop [I until the gate]: the sine table, the knob words, the screen calls, the voice
  objects, pitch and velocity, the position buffer table.
- To check in B2: `0xFF801774` (the FX output meter kennel effects write; coop's stereo bus may not show it),
  `0xFF802970 / 74` (tape speed and position), `0xFF8016E0` (block counter).
- Unlisted but inside R246's L1 in files that would load: 21 values. Most are offsets into the listed tables
  (`0xFF8013C6..CE` knob words + 2..10, `0xFF801954..1B50` inside the pitch words, `0xFF801E4E` inside the envelope
  words, `0xFF8016C6..CA` master knob words); not identified yet: `0xFF801054..5C`, `0xFF8012BC`, `0xFF8012D4` and one
  L1 code address, `0xFFA055B4`. The survey prints them; each needs a line in the table or a reason before its file is
  called tested.
- Not detectable: an address assembled another way (arithmetic, a table in `.rodata`). The crash stage marker
  (decision 6) is the net under that.

## ABI gap

**kennel effects and engines** (every import the 48 effect and engine files of hundekraft 3.3.1 use; the full table
with notes is `core/bridge/bridge_imports.def`):

| Import | Files | On coop | From |
|---|---|---|---|
| `read_fx_param_safe` | 14 fx | shim: the stock `0x01A28DA0` behind coop's `fx_sanitize` | B2 (core) |
| `op1_fx_page`, `op1_fx_wrap` (data) | 12, 2 fx | present as arrays (page 1 only); real with coop's controls | B2 / B5 |
| `op1_l1_claim`, `op1_l1_claim_state`, `op1_l1_sine_quarter` | 7 fx, 15 synth | present: answer 0, kennel's documented "no pool, same output" | B2 |
| `op1_dbg`, `op1_fx_fast` | 3, 1 | present: empty, 0 = no lease | B2 |
| `op1_sc_source` | 1 fx (ente) | present: 0 and zeros (the sidechain hears nothing); real needs the tape taps | B2 / B8 |
| `op1_fx_big` | 2 fx | needs a decision (3): 1 MiB a position | B8 |
| `op1_screen_draw_cached`, `_cached_extra`, `sq_*` | 9 fx, 16 synth | stand-in (never called) then vendored | B2 / B3 |
| `op1_eng_page` (data) | 18 synth | present as an array (page 1 only) | B4 / B5 |
| `sq_synth_knobs_live`, `op1_engine_knob_steps` | 2, 1 synth | shim over `coop_scr_knobs`; a stepped knob turns fine | B4 / B5 |
| `op1_harmony_context` | 1 synth | impossible: fed by kennel's chord and scale features | never |
| `op1_input_gain_db`, `op1_input_tuner_active` | 3 synth | impossible without an input engine path | never |
| the 134 names the 32 feature files import | 32 | impossible: features hook kennel's dispatcher | never |

| Feature of the file | On coop | From |
|---|---|---|
| kind FX, mono | adapter, dual-mono | B2 |
| flag stereo (9 fx) | adapter: `render_stereo` at the master | B2 |
| flag page2, `press[]` (11 fx, 4 synth) | loads on page 1; presses mapped onto coop's e4 / e6 controls generically | B2 / B5 |
| kind synth, plain | adapter | B4 |
| flag coarse (stepped knobs, 1 synth) | turns fine until B5 | B4 / B5 |
| flag uses_sample (granular), bigbuf, engine extensions `0x20`..`0x800` | core work | B8 |
| flag input (amp, snap, vox) | impossible as coop stands | never |
| kind feature | impossible | never |
| screen (SVG + `op1_screen_t`) | stock page, then the file's own | B2 / B3 |
| saved sounds: type name, selector `0x5300 + n` | same scheme on both [I]; coop numbers plugins itself, so a sound finds a bridged plugin by name, never by kennel's number | B2 |
| kennel's settings rows (extension 7) | ignored (parked) | B2 |

**Opus** (the 8 third-party files here; table in the same `.def`): `opus_effect_register` with per-parameter
`limits {min, max, step}`, host-owned state (`need` gives state / bulk / fast bytes: coop's position buffer), `mono`
and `stereo` renders in int16: a clean fit, B6. `opus_engine_add`: B7. Pages (`opus_page_*`, `opus_art_*`, the
`o3_*` library): B9, until then "nothing to draw", as kennel's phase 2 did. Never: `opus_lfo_register`,
`opus_seq_register`, `opus_on_key`, `opus_on_knob` (kennel refuses that one too), drum engines, `opus_fast_lease`
(dx leases 8 KiB, coop's whole L1 pool for engines is 4 KiB), `opus_engine_data` (coop saves eight words), places.
Sounds saved on Opus (its own effect names and parameter units, `"opus": {...}` members) need kennel's `opus_saved.c`
equivalent: not planned before the owner asks for it.

## Survey (2026-11-20)

`python3 tools/bridge/survey.py` over hundekraft 3.3.1's own 80 `.knl` files (`incoming/op1-workshop/variants/7ff`)
and the 8 `.opmod` files kennel bundles (the Opus 4.2 starter pack's). The core's code gives the same verdict for all
88 [C host test].

| | Files | Load with shims | Later (core work) | Never |
|---|---|---|---|---|
| kennel effects | 19 | 17 from B2 | 2 at B8 (`op1_fx_big`: rackverb, steve) | 0 |
| kennel engines | 29 | 25 from B4 | 1 at B8 (granular: sample) | 3 (amp, snap, vox: input) |
| kennel features | 32 | 0 | 0 | 32 |
| Opus | 8 | 0 | bitcrush B6, dimension B7 | 6 as planned (3 need `opus.orbit3d`, dx, matrix, the library itself) |

- Nothing loads "as is": every file needs at least the adapter. 42 of the 48 kennel effect and engine files load once
  B2 and B4 exist, with nothing but the stand-ins B1 already has plus `read_fx_param_safe`.
- Memory at two relocations an effect: the 17 effects 922,238 bytes (snowglobe 305 KB, nostromo 104 KB, clouds
  99 KB), the 25 engines 1,213,975 (vapora 477 KB, strum 266 KB); everything at once is 2.1 MB against the 1,536 KiB
  budget, so the budget's own per-file refusal does the choosing. kennel's arena is 2.5 MiB or more.
- The catalogue (`plugins/*/plugin.json`, 153 plugins): 61 hundekraft builds (25 fx, 27 synth, 9 features), 53 op1r
  (19 fx, 25 synth, 9 features), 18 Opus (14 fx, 4 synth), 13 coop. The owner's 52 Opus files (`coldsoldr.*`) exist as
  `.coop` already and were left out.
- `fmbell.knl` shows why the loader needs `kn_parse_as`: its 16-byte entry object sits in the image's last 36 bytes,
  where coop's `kn_parse` wants a `coop_engine_t` ("entry object truncated"). The other 79 pass `kn_parse` as it is.

## Safety

- **Named refusal before anything runs.** Order for a `.knl`: magic, size, CRC, API (kennel's); kind and flags
  (`br_knl_normalise` returns 1 and leaves the bytes alone); `kn_parse` (every table, relocation and name); every
  import against the table (`br_knl_imports`: "needs <name>"); then `kn_relocate_far`, which looks every import up
  before writing a byte [C test: the destination is untouched after a refusal]. The reason goes where a `.coop`'s
  goes: the stats line and the inventory.
- **Cache sync** (e4b): the bridge relocates through `plugin_host.cc`'s path, so `code_sync(base, base + used)` runs
  for each of the two relocations before `init`. The table swap and the adapters write data only.
- **Callbacks inside the file's text**: as for a `.coop` (init, render, render_stereo, the engine's three), checked
  after relocation; `kn_relocate` already does it for stereo files.
- **Crash stage marker** (decision 6): `/yaffs2/plugins/.stage` names the bridged file whose `init` is running; found at
  boot, that file is skipped and named until its bytes change.
- **CPU**: a bridged effect is measured like any plugin; dual-mono doubles kennel's figure. No automatic refusal by
  cost in B2: the gate publishes the cycles per file, and the site shows them.
- **What stays [I] until a device says otherwise**: everything under "Addresses a file carries"; that a kennel
  effect's `init` is as cheap as coop asks at positions 0-2 (it runs in the audio interrupt there).

## Licences and etiquette

- kennel is GPL-3.0-or-later with the OP-1 firmware permission, as this folder: its code may be vendored with its
  notices (the loader already is; B3's `sq_*`, B5b's `plugload` would be). `bridge_opmod.cc` adapts kennel's
  `opus_load.c` checks and says so.
- The Opus plugin kit is GPL-3.0 (kennel's notes add two section 7 terms: a modified version is marked and does not
  use the original plugin's name). The bridge declares the layouts itself (offsets and meanings), as kennel does, and
  never patches a file's code; relocation and the header normalisation in RAM are what loading is.
- Released plugin files are their authors'. The catalogue's rule stands: only the owner's own or licensed files are
  hosted, listings link to the author's page. The bridge loads files the user brings; no `.knl` or `.opmod` is in this
  repository, and the tests read local, gitignored copies or skip.
- A bridged plugin shows its author's name and the file's own type name. A coop port of the same plugin stays a
  separate listing ("port" = the porter).

## What the site needs

- `web/src/core/profiles/op1-coop.ts`: accept `.knl` (later `.opmod`, `.op1p`) for `/plugins` when the connected
  core reports a bridge level (a word in the inventory, as kennel's HELLO capability 128), else explain why not.
- A TypeScript twin of `tools/bridge/formats.py` + the table (`survey.py --json` is the fixture) so the Plugins tab
  shows the verdict before sending: "loads (bridged)", "needs coop B4", "never: a kennel feature".
- Compatibility labels on listings: a hundekraft or op1r build gains "runs on coop through the bridge" when its file's
  verdict is at or below the released level; "tested" when a gate ran that exact sha256 (decision 6).
- Memory: the page already totals `.coop` files against the budget; a bridged effect counts twice its load size.
- Nothing is rehosted: the user picks the file they downloaded from the author.

## Plan

Each phase: a host test under `tests/host/bridge`, then an op1emu gate, then a device line in `device-tests.md`.

- **B1 (done, 2026-11-20): what the host can prove.** The table, the header normalisation, the verdict, the export
  table, the `.opmod` checks, the effect adapter, the stand-ins, the address audit, the survey. Host: `make -C
  tests/host/bridge` (327 checks; fixtures made in the test; then the core's verdict against `survey.py`'s on every
  local real file, SKIP without them). No gate: nothing is in the core.
- **B2: kennel effects play.** Core: (a) the loader change below; (b) `plugin_host.cc`: the boot scan takes `.knl`
  beside `.coop` (behind the image flag `bridge`), `load_one` branches on the magic into `br_knl_normalise` /
  `br_knl_parse` / `br_knl_imports`, relocates twice against `br_knl_exports(BR_LEVEL)`, `code_sync`s both, checks
  the callbacks, builds a `br_fx_inst` with `scratch_tab = (void **)0x01AB8BA0` and `right_scratch[pos]` = coop's
  position buffer, counts 2 x the load size against the budget, sizes the position buffers to 176,460 when the audit
  finds the table's address; (c) the render and init call sites go through `br_fx_render` / `br_fx_init` for a bridged
  record; (d) bridged effects are left out of the FX 2 lists (decision 4); (e) `read_fx_param_safe`; (f) the name
  rule (decision 5) and the stage marker (decision 6); (g) `BR_LEVEL 2`. Host: a real `.knl` relocated twice at two
  bases and compared word for word outside its relocation sites. Gate: crush (mono, no imports but one), plate
  (stereo), tape (page 2), snowglobe (153 KB of `.bss`) at the synth, drum and master positions on op1emu: audio
  against kennel's own render of the same input (te-op1-cfw's host builds), left and right independent, cycles per
  block, a refused file (rackverb) named in the stats, boot with all 17.
- **B3: kennel screens.** Vendor `sq_*` and the cached draw; register `op1_screen_t` as an e9 effect screen. Host:
  the screen host (`tests/host/screen_host.cc`) with a kennel screen's calls. Gate: screenshots of hail, stutter,
  warble against kennel's.
- **B4: kennel engines.** The engine adapter (`self` = the voice object), `op1_eng_page`, the engine list name.
  Host: a fake engine through the adapter; pitch and velocity read where the engine reads them. Gate: wavetable,
  taffy, fmbell (the `kn_parse_as` case), vapora (477 KB) on op1emu: six voices, the stats' cycles.
- **B5: controls.** kennel's `press[]` (page 2, latch, semitone page), `OP1_FX_STEPS`, coarse knobs, `op1_fx_shift`,
  `op1_fx_push_bit` mapped onto coop's e4 / e6 controls without a declaration in the file (a generic control set per
  press code); words 5..7 saved as kennel saves them. Host: the mapping table. Gate: tape's page 2, strum's selector.
  **B5b: `.op1p`** (decision 8).
- **B6: Opus effects.** `ol_place`'s equivalent, `opl_init` in a transaction, `opus_effect_register`, the per-block
  parameter map (`lo + q15 * (hi - lo) / 32767`, as kennel), state from the position buffer. Host: the kit's GPL
  bitcrush example built for the host through the adapter. Gate: `bitcrush.opmod`.
- **B7: Opus engines** (dimension). **B8: big buffers, sample engines, kennel's engine extensions** (each its own
  decision). **B9: Opus pages and library files** (orbit3d: chorus, plate, sim).

### Loader change (B2a; not applied: `plugin_load.cc` belongs to the core build)

`kn_parse_as(file, size, api, info, err, entry_min)`: `kn_parse` with the entry object's least size per kind handed
in; `kn_parse` becomes a call with coop's sizes `{0, 36, 12, 84}`; the bridge passes kennel's `{0, 16, 12, 84}`
(`br_knl_parse` uses it when `KN_HAVE_PARSE_AS` is defined). 3 changed lines and one new function. Applied to a copy,
the host test runs clean and `fmbell.knl` loads from B4 like the other 24 [C].

```diff
--- a/core/sdram/plugin_load.h
+++ b/core/sdram/plugin_load.h
@@ int kn_parse(const uint8_t *file, uint32_t size, uint16_t api_version, kn_info *info, const char **err);
+#define KN_HAVE_PARSE_AS 1
+int kn_parse_as(const uint8_t *file, uint32_t size, uint16_t api_version, kn_info *info, const char **err,
+                const uint8_t *entry_min);
--- a/core/sdram/plugin_load.cc
+++ b/core/sdram/plugin_load.cc
-int kn_parse(const uint8_t *file, uint32_t size, uint16_t api_version, kn_info *in, const char **err)
+int kn_parse_as(const uint8_t *file, uint32_t size, uint16_t api_version, kn_info *in, const char **err,
+                const uint8_t *entry_min)
@@
-    uint32_t entry_size = in->kind == KN_KIND_FEATURE ? 84 : in->kind == KN_KIND_SYNTH ? 36 : 12;
+    uint32_t entry_size = entry_min[in->kind];
@@ (after kn_parse_as)
+int kn_parse(const uint8_t *file, uint32_t size, uint16_t api_version, kn_info *in, const char **err)
+{
+    static const uint8_t coop_entry[4] = { 0, 36, 12, 84 };
+    return kn_parse_as(file, size, api_version, in, err, coop_entry);
+}
```

## Risks

- **The stock position buffer under a plugin** [I]: if R246 touches a position's buffer while the filter-slot plugin
  plays there, or reads the table from another thread during the right instance's call, the swap corrupts a delay
  line. B2's gate plays a stock delay at a position, switches to a bridged effect and back, and compares.
- **`init` in the audio interrupt**: a kennel effect that clears a large tank in `init` (kennel allows more there than
  coop's e8 contract) overruns a block at positions 0-2. The gate measures `init`; the fallback is to run a bridged
  effect's `init` on the main loop as FX 2 does.
- **Addresses** nobody listed (above). **Unknown code**: a bridged file is someone's binary with no source in the
  gate; the marker and the "not tested" label are the answer, not a guarantee.
- **Budget pressure**: two relocations an effect and bigger position buffers arrive while the budget is being raised
  only slightly; a user with coop's own nine ports installed has about 800 KB left before any bridged file.
- **The table's promise**: an entry is append-only once a released core resolves it; a stand-in that later becomes
  real (page 2, the sidechain) changes how an already-loading file sounds or pages. Each such step is a release note.
- **kennel moves**: 3.3.1's files are API 1 ("stays 1 until the first release"); a new flag or import in a later
  kennel is refused by name, which is correct and will look like breakage to users.
