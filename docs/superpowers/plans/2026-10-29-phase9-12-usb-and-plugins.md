# Phases 9 to 12: remembered firmware, USB check, plugin library, plugin installs

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** The librarian remembers which firmware the owner picked, can prove a hundekraft 3.x unit over USB MIDI and read its installed plugins, hosts a community plugin library (hundekraft `.knl`/`.op1p`, opus `.opmod`) with manuals and screenshots, and installs plugins: hundekraft over USB MIDI or by download for the hundekraft workshop, opus by copying into `mods/`. A patch that needs a plugin offers to install it.

**Why:** the owner's hundekraft OP-1 was detected as stock (disk mode looks identical; the sniff only fires on hundekraft-only engines). Owner decisions on 2026-10-29: add the USB check and remember the choice; plugin submissions by pull request like packs; installs both over USB from co-op and by download for the workshop; both hundekraft and opus plugins; plugin files are stored in the repo with an optional link to source; seed with the owner's ten plugins from `coldsoldr/te-op1-cfw` branch `hk-plugins`, GPL-3.0-or-later, source link https://github.com/coldsoldr/te-op1-cfw; ship a prompt authors give their coding agents so manuals are consistent.

**Source facts:** `docs/research/2026-10-29-hundekraft-usb-and-plugins.md` (read it first: protocol, inventory, file formats, boot hazards, seed plugin headers). `vendor/hundekraft` is GPL: copy facts, never code. Tests that need vendor files skip without them.

**Conventions (all tasks):** work on `main`; TDD; match the surrounding code's style and comment density; `pnpm --filter @co-op/web test`, `typecheck`, `pnpm lint` green before each commit; e2e (`pnpm --filter @co-op/web e2e`) green for UI tasks. Browser storage reads and writes go in try/catch and the app works without them. Commit messages end with the attribution trailer the controller gives you.

---

## Phase 9: remember the firmware choice

Status: pending.

### Task 1: remembered profile

- New `web/src/ui/remembered-profile.ts`: `loadRememberedProfile(): string | null` and `rememberProfile(id: string | null)` over `localStorage` key `co-op.profile` (JSON `{ id, at }`). Unknown ids (not in the registry) read as null. All access in try/catch.
- `useDevice.setProfile` (the Library dropdown and the suggestion's Switch button) remembers the chosen id. Choosing in the dropdown is the only way a value gets stored (plus phase 10's "Use this profile").
- On connect or drop (`useDevice` around `detect(base)`): if a remembered id exists, use it, unless folder detection found a different profile with confidence >= 0.97 (opus by its `mods/` folder, the only evidence that proves the firmware). The detection line says which: "remembered from last time" or "found mods folder with opus plugins (you chose OP-1 (hundekraft) last time)". When the remembered profile is used, the sniff suggestion still runs as today.
- Unit tests for the module (storage throwing, unknown id, round trip) and hook-level tests where the existing ones live; e2e: choose hundekraft, disconnect, drop the drive again, hundekraft is selected with the remembered note; an opus drive overrides it.

---

## Phase 10: USB MIDI check (read only)

Status: pending.

### Task 2: protocol core

`web/src/core/usbmidi/` (pure, no browser APIs; a port is `{ send(bytes: Uint8Array): void; onSysex(cb: (bytes: Uint8Array) => void): void; close(): void }`):

- `framing.ts`: header constant, 7-bit pack/unpack, `buildMessage(cmd, seq, payload)` refusing anything over 96 bytes, `parseReply(bytes)` returning `{ cmd, seq, status, payload } | null` (not ours / not a reply), reassembly of SysEx from byte runs that skips realtime bytes >= 0xF8. CRC-32 (zlib): reuse one if the codebase has it, else add it here.
- `session.ts`: one message in flight; each `ask(cmd, payload)` sends with a fresh 14-bit seq, waits for the reply with the same cmd and seq (timeout default 3000 ms, 5 tries, an AbortSignal), ignores stale replies. `hello()` -> `{ version, inventory: boolean, plugins: boolean }` (capabilities byte absent = both false). `readInventory()` pages LIST in 64-byte steps until the total; `readOff(which)` pages OFF_GET. No write commands in this task.
- `inventory.ts`: `parseInventory(text)` -> `{ core: { start, len, crc } | null, presets: { count, cap } | null, records: InventoryRecord[] }` per the research notes (unknown header fields ignored; first line must be `kennel-inventory 1`). `savedNameOf(id)` (`_` -> space, `drumplus` -> `drum+`).
- Tests: a fake device in `web/test/` that implements the device side from the research notes (HELLO with and without the caps byte, LIST paging, offset past end, silence, lost replies, realtime bytes inside SysEx, a reply split across callbacks), plus parse tests with hand-written inventory text.

### Task 3: Web MIDI port and the check

- `web/src/ui/webmidi.ts`: `openOp1Port(signal)` using `navigator.requestMIDIAccess({ sysex: true })`, picks the input and output whose names match `/op-?1/i` and are not disconnected, sets `onmidimessage` before opening both ports (early replies are queued), returns the port plus `watch(cb)` for statechange. Clear errors: no Web MIDI (name the browsers), permission refused, no OP-1 port (hint: plug it in normally, not in disk mode; disk mode has no MIDI).
- `web/src/core/usbmidi/check.ts`: `checkDevice(port)` -> one of: `{ kind: 'kennel', hello, inventory, off }`; `{ kind: 'silent' }` (port there, no HELLO answer: stock, op1r, hundekraft 2.7 or older, or busy restarting). Pure, tested with the fake device.
- `web/src/ui/UsbCheck.tsx`, reached from a "Check over USB" button in the Library header (available with or without a drive open): shows the result: firmware ("hundekraft 3.x (kennel protocol <n>)" or the silent explanation), the plugin table (name, kind, source, version, author, state with reason), switched-off list, and "Synth presets: <count> of <cap>" when present (label it as reported by the OP-1). For kennel, a button "Use OP-1 (hundekraft)" that sets and remembers the profile. Keep the last result in `useDevice` state (in memory) as `installedPlugins` for phase 12.
- Disk mode and MIDI are exclusive on the OP-1, so the panel says to leave disk mode first and that the drive can be opened again afterwards.
- e2e: stub `navigator.requestMIDIAccess` in the page with a fake OP-1 that answers HELLO and LIST; the panel lists plugins and switching profile works; a stub with no answer shows the silent message.

---

## Phase 11: plugin library

Status: pending.

### Task 4: plugin format and validation (shared)

`shared/src/plugins.ts` (pure, like `packs.ts`):

- Folder `plugins/<id>/` with `plugin.json`, the plugin file, `manual.md`, a screenshot, and nothing else (ignore OS noise as packs do).
- `plugin.json` format 1: `format: 1`, `id`, `name` (display), `firmware: 'op1/hundekraft' | 'op1/opus'`, `kind: 'synth' | 'fx' | 'feature'`, `author`, `version` (free text, <= 20), `description` (<= 500), `license` (SPDX from a fixed list: GPL-3.0-or-later, GPL-3.0-only, GPL-2.0-or-later, MIT, BSD-3-Clause, Apache-2.0, MPL-2.0, plus `LicenseRef-Freeware` for closed free-to-use plugins), `file` (`<id>.knl`, `<id>.op1p` or `<name>.opmod`), `screenshot` (`.png`), `manual` (`manual.md`), optional `source` (https URL), optional `requires` (free text such as "hundekraft 3.0.3", <= 60), optional `credits` (<= 200), `tags` (<= 8, packs' tag rule), optional `savedName` for opus (the `opus.<id>` plugin id sounds name, used for requirements).
- Ids: hundekraft `[a-z0-9_-]{1,15}`; opus `[a-z0-9_.-]{1,40}`. Folder name equals the id.
- File checks (pure functions over bytes, each returning problems as strings):
  - `.knl`: every header rule in the research notes (magic, format 1, header size, file size equals length, size % 4, <= 512 KiB, CRC with its 4 bytes zeroed, API 1, kind 1..3, order <= 31, saved name 1..15 printable ASCII, id equals saved name with spaces as `_`, file named `<id>.knl`), the SVG decimal boot hazard, and `plugin.json` `kind` matching the header (1 synth, 2 fx, 3 feature). Read author and version from the extension table when present (report a mismatch with plugin.json as a warning, not an error).
  - `.op1p`: trailer and binary rules from the notes, kind 0 synth / 1 fx, id rule, the SVG hazard on any SVG prefix.
  - `.opmod`: magic `OPMD`, <= 2 MiB, firmware must be `op1/opus`.
  - Screenshot: PNG signature, IHDR width and height each 64..2048, <= 1 MiB. `manual.md` UTF-8, <= 64 KiB, non-empty. `plugin.json` <= 64 KiB.
- `PluginIndex` (format 1): per plugin the manifest fields plus `sizeBytes`, `sha256`, `savedName` (from the file header for hundekraft; plugin.json for opus), header `author`/`version` when present, `generatedAt`.
- Tests with hand-built byte arrays for every rule (including a CRC computed in the test).

### Task 5: build, CI, dev server

- `web/scripts/plugins-lib.ts` + CLI `web/scripts/plugins.ts` (`validate`, `build <outDir>`), following `packs-lib.ts`/`packs.ts`: read `../plugins`, validate, copy into `dist/plugins/`, write `dist/plugins/index.json`; `PLUGINS_NOW` fixes `generatedAt`. Dev middleware serving `/co-op/plugins/...` in `vite.config.ts`. `web/package.json`: `plugins:validate`, and `build` runs both builders. CI runs `plugins:validate`; extend the mixed-PR warning to `plugins/`.
- Tests mirror the packs-lib tests.

### Task 6: seed the ten plugins

- Fetch the ten `.knl` files and `screens/<id>_dark.png` from `coldsoldr/te-op1-cfw` branch `hk-plugins` (`gh api` or raw.githubusercontent.com; write into the repo, check sizes match the research notes). One folder each: `plugins/<id>/<id>.knl`, `screenshot.png`, `manual.md`, `plugin.json`.
- `manual.md` per plugin: split from the branch's README (that plugin's section, its controls table and "On screen"), plus a short "Before you start" (original OP-1, hundekraft 3.0.3, install routes) and the shared "How the controls work" rules where they apply. Keep the owner's wording; follow `docs/plugin-manual-prompt.md` (task 7) for structure.
- `plugin.json`: license `GPL-3.0-or-later`, source `https://github.com/coldsoldr/te-op1-cfw`, author `coldsoldr`, requires `hundekraft 3.0.3`, kind from the header, credits on rings, marbles, grids, freq_shift: "Port of Mutable Instruments <module> by Emilie Gillet (MIT)". Validator must pass.

### Task 7: docs and templates

- `docs/contributing-plugins.md`: what a plugin folder holds, the checks, the risk statement (a plugin runs as firmware code; maintainers merge only from authors they trust or with source they have looked at; a source link is strongly encouraged), how users install (USB from co-op, the hundekraft workshop, opus `mods/`), how to update (bump version, replace file).
- `docs/plugin-manual-prompt.md`: a copy-paste prompt for the author's coding agent. It asks the agent to read the plugin's source and produce `manual.md` with fixed sections: title and one-line summary; screenshot line; what it is (one paragraph, plain words); requirements; a controls table `Encoder | Main page | Press, then turn` for blue, green, white, orange; second pages and Shift behaviour; "On screen"; tips and CPU notes; what is saved with a sound; credits and licence. Style rules: short sentences, British spelling, no marketing words, name the OP-1 encoders by colour, units and ranges where known, never invent behaviour not in the source. Include a filled-in example (bitcrusher, from the seed manual).
- `.github/PULL_REQUEST_TEMPLATE.md`: a plugin checklist next to the pack one. Issue template `plugin-request.yml` if packs has one (it does: `pack-request.yml`). README: plugin library section.

### Task 8: Plugins tab

- `web/src/core/plugins/catalogue.ts`: fetch `plugins/index.json`, filter by firmware, kind, tag, text; fetch plugin bytes and verify sha256 and size.
- `web/src/ui/Plugins.tsx`: a tab next to Packs. Cards with screenshot (lazy, alt text), name, kind, author, version, licence, requires, description, source link. "Read the manual": render `manual.md` with a small safe Markdown renderer written here (headings, paragraphs, lists, tables, code, links with http(s) only, images only from the same plugin folder; no raw HTML). Actions per plugin: Download (always); for hundekraft "Install over USB" (enabled in phase 12; until then hidden); for opus "Copy to mods/" when an opus drive is open (phase 12).
- Markdown renderer gets its own unit tests (XSS cases: raw HTML, `javascript:` links, image hosts).
- e2e: the tab lists the seed plugins from the dev server, filters, opens a manual, downloads a file.

---

## Phase 12: plugin installs and requirements

Status: pending.

### Task 9: install over USB (hundekraft)

- `web/src/core/usbmidi/install.ts`: `installPlugins(port, files, events)` for `plugins/<id>.knl|.op1p` only (the client refuses any other path), following the notes: HELLO must report the plugin capability; the file must pass the task 4 checks again in the browser; ABORT, then for each file BEGIN (size, CRC, path) -> DATA in 72-byte pieces, one in flight, handling status 5 by resending from the reported count -> END; then COMMIT, polling while it answers 9 and telling the UI to "press T1 on the OP-1 to install and restart, T2 to cancel"; status 2 after the prompt means cancelled. Timeouts: 10 s control steps, 3 s data, as in the notes.
- Refuse before sending anything: a plugin whose id is already installed (inventory record with that path or name), with the message to remove it first in the hundekraft workshop because replacing in place can hang the restart; no inventory (older core) -> point to the workshop.
- After COMMIT succeeds the OP-1 restarts; the UI waits for the MIDI port to come back (statechange) and re-runs the check, then shows each plugin's state.
- Confirm dialog before sending (default Cancel): names the files, says a plugin runs as firmware code, that the OP-1 restarts, and that if it fails to start, switching it off and on starts it without plugin files.
- Tests against the fake device extended with the device side of BEGIN/DATA/END/COMMIT/ABORT (wait key, cancel, gap, lost replies, path refusals).

### Task 10: opus copy to mods/

- From the Plugins tab with an opus drive open: write `mods/<file>.opmod` through the existing store and `writeAtomic` (drop-store collects it into the changes zip as other writes do). Refuse overwriting an existing file of the same name without a confirm. Refresh the Installed opus plugins list afterwards. Note in the UI that opus loads plugins in file-name order.

### Task 11: patches that need a plugin

- `web/src/core/plugins/requirements.ts`: `requiredPlugins(traits, index, firmwareId)` -> the library plugins whose `savedName` matches the patch's engine (synth), effect (`fx_type`) or opus plugin id, for the active firmware. `installedNames(inventory)` -> saved names of loaded records.
- Compatibility: the hundekraft profile's finding for an engine or effect that is a library plugin says `needs plugin "<id>"` (warn) instead of the generic message; when the USB check shows it loaded, it is ok. Keep profiles pure: pass the library and installed names in through a small context the UI supplies (for example a `withPlugins(profile, context)` wrapper), and keep the existing profile tests green.
- Prompt: when installing patches (packs, op1.fun, restore), after the write guard and before writing, collect required plugins not known to be installed and show "This patch needs the plugin <name>. Install it?" with Install over USB (hundekraft, when available), Copy to mods/ (opus), Download, Continue without, Cancel. Library rows show the requirement in the traits popover.
- e2e: installing a pack patch whose engine is a seed plugin (add a small test pack or reuse test fixtures in the e2e harness, not in `packs/`) on the hundekraft profile shows the prompt.

### Task 12: docs, review, deploy

Spec: new sections for remembered profile, USB check, plugin library and installs; decisions table rows. README "What exists today". Research notes "As built". Plan statuses. Final review over phases 9 to 12; push; watch ci, pages and worker.
