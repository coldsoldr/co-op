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
