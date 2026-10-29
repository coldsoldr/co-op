# Phases 7 and 8: hundekraft and opus Profiles Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** The librarian understands patches made on hundekraft and opus firmware, recognises which firmware a drive most likely runs, judges compatibility per firmware, and warns before writing a patch that can stop a stock OP-1 at boot.

**Source facts:** `docs/research/2026-10-28-custom-firmware-notes.md` (read it first). The owner's clones are in `vendor/` (gitignored, GPL). We copy facts, never code. Tests that read vendor files must skip cleanly when `vendor/` is absent (CI has no vendor folder).

**Architecture:**
- The codec learns that `dbox` (stock drum synth) and `gin` (hundekraft drum kit) are drum-kind patches. A new pure module `core/codec/sound-traits.ts` extracts firmware-relevant traits from any metadata: synth engine, drum engine, effect type, LFO type, the selector words in `knobs[4]` and `fx_params[4]`, presence of `gin_state`/`dbox_data`, presence of the `opus` member and its plugin id. Profiles judge compatibility from traits, never from raw JSON.
- Each profile declares the engine, effect and LFO names it supports. The stock profile becomes stricter: any non-stock engine, effect or LFO, an `opus` member, or a selector word in the op1r range makes the patch `unsupported` on stock with the reason "may stop a stock OP-1 at boot".
- Detection gains an optional content phase: `DeviceProfile.sniff?(traits: SoundTraits[]): DetectResult`. Folder detection still runs first. After the library loads, the UI runs sniff over loaded patches and, if another profile scores higher than the active one, shows a suggestion ("These patches use hundekraft engines. Switch profile?"). It never switches automatically, because patch contents prove where a sound came from, not which firmware is installed now.
- Writes guarded: installing from packs or op1.fun, restoring, and copying between profiles check the active profile's compatibility first; anything `unsupported` needs an explicit confirm naming the risk.

---

## Phase 7: hundekraft

Status: done (2026-10-29). Deviations from the tasks below, decided during review: a selector-like
fifth knob or effect value with otherwise stock names only warns on stock and does not sniff as
hundekraft; `dbox_data` alone is not a Gin trait; `opusMark` is separate from `engineSelector`;
sounds with an `opus` member never sniff as hundekraft; pack and op1.fun installs download before
the write guard asks, so nothing is written until the user confirms; copy and move within the drive
are not guarded, because the file is already on the device.

### Task 1 (done): stock name lists from the firmware

Extract the stock R246 engine, effect and LFO names as they appear in saved sounds. Sources: the research notes, existing `STOCK_SYNTH_ENGINES`, and the stock firmware string table at `vendor/hundekraft/op1-workshop-main/base/OP1_vdk.ldr.insert.bin` (read-only; search for the lower-case names near the known engine list). Expected stock effects include `delay`, `cwo`, `grid`, `nitro`, `phone`, `punch`, `spring`; stock LFOs include `bend`, `crank`, `element`, `midi`, `random`, `tremolo`, `value`; confirm each against the binary and the real fixtures, and record the evidence in the research notes. Add `STOCK_FX_TYPES`, `STOCK_LFO_TYPES`, `STOCK_DRUM_ENGINES = ['drum', 'dbox']` to `op1-stock.ts`. If a name cannot be confirmed, leave it out and note it.

### Task 2 (done): codec drum kinds and sound traits

- `op1-meta.ts`: `isDrumMeta` accepts `type` in `drum | dbox | gin` with a string `name` (the per-slot arrays become optional in the type for dbox/gin); `metaKind` follows. Keep existing tests green; add tests for dbox and gin shapes (gin: `dbox_data` 24x8 numbers, `gin_state` string).
- `sound-traits.ts`: `SoundTraits { kind; engine: string; fx: string | null; lfo: string | null; engineSelector: number | null; fxSelector: number | null; hasGin: boolean; opusPlugin: string | null; opusEffects: string[] }` and `soundTraits(meta: Op1Meta | null): SoundTraits | null`. `engineSelector` is `knobs[4]` when it is in `0x5300..0x53FF` (op1r family) or `>= 0x100` with an `opus` member (opus mark); otherwise null. `opusEffects` lists `opus.*_type` values found in the `opus` member. Tests with hand-built metas and, when `vendor/` exists, every patch in `vendor/hundekraft/op1-workshop-main/variants/7ff/presets.zip` (read with fflate in the test, never copied into the repo).

### Task 3 (done): profile model update

- `types.ts`: add `sniff?(traits: SoundTraits[]): DetectResult` to `DeviceProfile` and a `supports` block: `{ synthEngines: readonly string[]; drumEngines: readonly string[]; fx: readonly string[]; lfo: readonly string[] }`.
- Replace `engineCompatibility(engines)` with `traitCompatibility(supports, extra?)` returning `ok`, `warn` or `unsupported` with reasons; unknown kinds stay unsupported; a missing trait (no fx) is fine. Stock uses it with the strict rule above. Keep `engineCompatibility` exported as a thin wrapper if other code uses it, or update callers.
- Update stock tests; real fixtures and test-tones must stay `ok`.

### Task 4 (done): hundekraft profile

- `op1-hundekraft.ts`: `id 'op1/hundekraft'`, label "OP-1 (hundekraft)", same layout and folder detection as stock but `FULL_MATCH` reduced so stock wins ties on folders alone (stock 0.9, hundekraft 0.85). `supports` = stock lists plus the hundekraft engines (supersaw, fmbell, granular, wavetable, taffy, pew, elg, mixtur, arc, aether, eva, mbira, oxbow, sid, amp, iter), drum engines plus `gin`, effects plus (tape, clouds, reverb, washer, snowglobe, palme, "in a room", stutter, hail, crush, warble, plate, father), LFOs plus `quad` (marked unconfirmed in a comment). Compatibility: opus-only traits (an `opus` member) -> warn "needs the opus bridge or opus firmware". Unknown engine in the op1r selector range -> warn "plugin may not be installed; plays as stock Iter" (per the hundekraft manual).
- `sniff`: confidence 0.95 when at least one loaded patch has a hundekraft-only engine/effect, gin traits, or an op1r selector; otherwise 0.
- Register in `registry.ts` after stock. Packs may now list `op1/hundekraft` in `firmware` (the validator uses the registry list automatically; add a test).
- Tests including the vendor presets (skip without vendor).

### Task 5 (done): UI for content detection and write guards

- After loadAllPatches completes, compute traits and run every profile's `sniff`; if the best sniff profile differs from the active profile and scores above the active profile's folder confidence, show a dismissible suggestion with a "Switch to <label>" button. Remember dismissal per source in memory.
- Write guards: a shared `confirmUnsupported(patches, profile)` dialog used by pack install, op1.fun install, restore, and copy/move when any patch is `unsupported` on the active profile. The dialog names each patch and reason, and for the stock profile says "may stop a stock OP-1 at boot". Default button is Cancel.
- Library rows show traits in a details popover (engine, effect, LFO, plugin) to explain badges.
- e2e: a dropped drive with a hand-built hundekraft patch (supersaw) shows the suggestion; switching profile turns its badge `ok`; installing a pack patch tagged with a hundekraft engine onto a stock drive shows the boot warning.

### Task 6 (done): docs and deploy

Spec sections 5.2 and 11 updated (sniff phase, supports block, write guards); research notes updated with confirmed stock lists; README phase 7; push; watch ci and pages.

---

## Phase 8: opus

Status: done (2026-10-29). Deviations: the opus sniff is 0.96, not 0.9, so a stock-looking drive of
opus sounds (0.9 by folders) gets the suggestion and a drive mixing hundekraft and opus sounds is
offered opus, which hosts both; hundekraft names and op1r selectors warn "needs the opus bridge
plugin" rather than being unsupported; opus effect slots warn "needs plugin <id>" too; backups take
`mods/` through a new `DriveLayout.backupOtherRoots` instead of an opt-in flag on `createBackup`.

### Task 7 (done): opus profile

- `op1-opus.ts`: `id 'op1/opus'`, label "OP-1 (opus)". Layout: stock patch roots, `otherRoots` adds `mods`. Folder detection: `mods/` containing at least one `.opmod` file -> 0.97 with reason "found mods folder with opus plugins"; `mods/` without `.opmod` -> 0.6; otherwise fall back to the stock folder rule capped at 0.5 (an opus drive without mods is indistinguishable from stock).
- `supports`: stock lists plus opus effect names saved in `fx_type` (bitcrush, chorus, plate, filter, and any `opus.*` names seen), synth engines stock plus `iter` (opus engines save as iter + mark). Compatibility: an `opus` member naming a plugin -> `ok` with reason "needs plugin <id>" shown as info (warn level if we cannot know whether the plugin is installed: use `warn` with that reason); op1r selector traits -> warn "needs the opus bridge plugin".
- `sniff`: 0.9 when a loaded patch has an `opus` member.
- Registry order: stock, hundekraft, opus.

### Task 8 (done): mods listing

- Library shows, for an opus profile, a read-only "Installed opus plugins" section listing `mods/*.opmod` file names and sizes (no parsing beyond checking the first four bytes are `OPMD`, flagging files that are not). Backups include `mods/` when the active profile is opus (extend `createBackup` with profile `otherRoots` opt-in for `mods`; tape and album stay excluded).
- Tests with MemoryStore; e2e: a dropped drive with `mods/x.opmod` (bytes `OPMD....`) is detected as opus and lists the plugin.

### Task 9 (done): docs, final review, deploy

Spec sections 5.2, 11 and the decisions table updated; README "What exists today" covers all eight phases; research notes updated; plan marked done; push; watch ci and pages.
