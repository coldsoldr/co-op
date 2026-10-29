# Custom firmware notes: hundekraft and opus

Research notes for the hundekraft (phase 7) and opus (phase 8) device
profiles. Gathered on 2026-10-28 by reading the owner's local clones in
`vendor/` (gitignored). Both projects are GPL-3.0; we record facts about
file formats here and copy no code. Paths refer to `vendor/`.

## Stock firmware gap found along the way

Stock firmware has a drum synth whose saved kits use `type: "dbox"` (seen
in the stock firmware string table and as the template hundekraft copies
for its Gin kits). Our codec only classifies `type: "drum"` as a drum, so
a stock DBox kit currently reads as kind `unknown`. Fix before or with the
hundekraft profile: classify `dbox` as a drum kind and confirm its field
set from a real DBox file.

## Stock name lists (confirmed 2026-10-29)

Source 1: `hundekraft/op1-workshop-main/base/OP1_vdk.ldr.insert.bin`. This
is not the stock firmware itself but the code hundekraft inserts into
stock `OP1_vdk.ldr` (its `manifest.json` names the R246 original
`op1_246.op1`). The insert keeps NUL-terminated tables of the names it
compares saved sounds against. Offsets are into that file:

| Offset | Strings in order | Reading |
| --- | --- | --- |
| 0x217b9..0x217e5 | grid, punch, delay, phone, spring, cwo, nitro | the seven stock effects |
| 0x217ed..0x21825 | midi, quad, tremolo, value, random, element, bend, crank | stock LFOs plus `quad` |
| 0x2182d, 0x21839 | fm, iter (between `knobs` and `sitting`) | stock engines handled apart |
| 0x218e1..0x21939 | digital, drwave, string, cluster, pulse, sampler, phase, drum, dbox, dna, dsynth, voltage | the stock engine table |
| 0x21941, 0x2194d | gin_state, gin | hundekraft Gin kits, right after the stock table |

`quad` sits in the LFO table but hundekraft's manifest and manual list the
"quad LFO" as one of its additions, so it is not a stock name.

Source 2: committed fixtures. `web/test/fixtures/real/synth.aif` (fm,
spring, tremolo) and `real/drum.aif` (drum, delay, tremolo) are device
files; the synthetic fixtures use nitro/value, delay/random and
delay/tremolo.

Source 3: the 152 hundekraft factory presets use the stock names delay,
nitro, phone, spring, punch, grid, cwo and value, tremolo, element, random
next to their own effects.

Confirmed lists (in `web/src/core/profiles/op1-stock.ts`, checked against
the insert by a test that skips without `vendor/`):

- Effects: cwo, delay, grid, nitro, phone, punch, spring.
- LFOs: bend, crank, element, midi, random, tremolo, value. bend, crank
  and midi rest on the table alone; no real file uses them yet.
- Drum engines: drum, dbox. No real DBox file yet, so its field set is
  still unconfirmed (the codec accepts `dbox` with a string `name`).

Selector words seen in the presets: every hundekraft engine sets
`knobs[4]` to a word in 0x5300..0x53FF (supersaw 0x5301 to elg 0x5307;
eva, mbira, oxbow and sid 0x5300), and every hundekraft effect sets
`fx_params[4]` likewise (most 0x5300, washer 0x5304, some snowglobe
0x5305). The 12 `iter` presets and all stock-named effects use other
values. In these presets a selector never appears without a non-stock
name. Stock knob values run 0..32767, so 0x5300..0x53FF (21248..21503) is
also a legal stock value: the selector test can flag a genuine stock
sound whose fifth knob happens to sit in that band.

## hundekraft 3.0.3

- Browser-built firmware for the original OP-1 on R246, running the
  "kennel" plugin runtime. Plugins (`.knl`, legacy `.op1p`) live on the
  internal YAFFS volume, never on the disk-mode drive. Up to 300 user synth
  presets per build. First install via the boot menu, updates over USB MIDI.
- Synth engines added (saved in `type`): supersaw, fmbell, granular,
  wavetable, taffy, pew, elg, mixtur, arc, aether, eva, mbira, oxbow, sid,
  amp. Its factory pack also uses the stock but normally hidden `iter`.
- Effects added (saved in `fx_type`): tape, clouds, reverb, washer,
  snowglobe, palme, "in a room" (with spaces), stutter, hail, crush, warble,
  plate, father.
- `knobs[4]` carries an engine selector in 0x5300..0x53FF; `fx_params[4]`
  carries the same kind of selector for effects. Factory presets keep the
  stock key set and `synth_version: 2`.
- Gin drum kits: `type: "gin"`, stock DBox fields plus `dbox_data` (24
  arrays of 8 numbers) and a `gin_state` text member. The firmware treats
  `gin` as `dbox` for type comparisons.
- A "quad" LFO type may appear in `lfo_type`; not confirmed in a real file.
- Disk layout: unchanged. No marker file or folder. Only patch contents
  identify op1r-family sounds (non-stock `type`/`fx_type`, selector values
  in `knobs[4]`/`fx_params[4]`, `gin_state`/`dbox_data`). Those prove where
  a sound came from, not which firmware is installed now.
- Compatibility: stock sounds load on hundekraft. A sound whose plugin is
  missing plays as stock Iter (synth) or stock filter (effect) and keeps its
  names. Stock R246 is not forgiving: sounds using non-stock engines or
  effects can stop the OP-1 at boot, so the librarian must warn strongly
  before writing such a patch to a stock device.
- Test data: `hundekraft/op1-workshop-main/variants/7ff/presets.zip` holds
  152 synth patches (AIFC sowt, metadata plus empty SSND) under
  `synth/<engine>/`. GPL-covered; read locally in tests that skip when the
  clone is absent, never committed.

## opus 2 (beta)

- Mods for the original OP-1 on R246 by LizardMan: a small core plus
  `.opmod` plugins. Core built from the user's firmware with a web patcher;
  plugins copied onto the disk-mode drive.
- Disk layout: a new top-level `mods/` folder holding `*.opmod` files
  (magic `OPMD`), loaded in file-name order; plugins may also expose
  folders such as `mods/dx` (for `.syx` banks). This is a strong detection
  signal from the drive alone.
- Engine sounds are saved as `type: "iter"` with a mark of 0x100 or more in
  `knobs[4]` and a new top-level `"opus"` object naming the plugin. Plugin
  effects are saved by name in `fx_type` (for example `bitcrush`). The
  `"opus"` object also holds extra effect slots as `PREFIX_type`,
  `PREFIX_active`, `PREFIX_params` (prefix `fx2` for the overdrive slot).
  Engine data of up to 256 bytes is kept in the sound; its key is
  undocumented.
- Starter engines: opus.dimension, opus.amp, opus.dx, opus.outatime,
  opus.iter. Effects: opus.bitcrush, opus.chorus, opus.plate, opus.filter.
- Its bridge plugin loads hundekraft/op1r sounds (selectors 0x5301..0x53FF).
- Compatibility: stock, opus 1, op1r and hundekraft sounds are kept; a
  sound whose plugin is missing is kept unchanged and shown as needing it
  (engine sounds do not load, effects load switched off). Opus sounds are
  not stock-safe as saved; opus has a "cleaner" for that.
- Licence: repo docs and plugin kit GPL-3.0; release plugins free to use
  but closed; firmware and patcher data "all rights reserved". No example
  `.aif` files in the repo.

## What this means for the profiles

- `op1/opus` detection: `mods/` folder with `.opmod` files gives high
  confidence from folders alone.
- `op1/hundekraft` detection needs a content sniff over patch metadata,
  as already noted in the spec.
- Compatibility rules per profile must look at `type`, `fx_type`,
  `knobs[4]`, `fx_params[4]`, `lfo_type` and the `opus` member, not only
  the engine name.
- Writes to a stock device of a patch that is not stock-safe need an
  explicit warning, because it can stop the device at boot.
