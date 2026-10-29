# OP Patch Librarian: design

Date: 2026-10-28
Status: approved by owner, ready for implementation planning

## 1. Purpose

A browser-based patch librarian and community patch catalogue for Teenage
Engineering OP-series devices, hosted from this GitHub repository so anyone in
the community can use it, fork it and contribute to it.

Version 1 targets the original OP-1. Firmware support arrives in this order:

1. Stock OP-1 firmware
2. hundekraft custom firmware
3. opus custom firmware

OP-1 Field and OP-XY follow later as additional device profiles.

## 2. Goals and non-goals

Goals

- Manage patches on your own OP-1 from the browser: browse, preview, rename,
  move, copy, delete, back up and restore.
- Browse and install community-shared packs that live in this repository.
- Browse, pull from and upload to a user's own op1.fun account.
- Let anyone submit feedback from the app without a GitHub account, and
  record that feedback in the repo's GitHub Issues for triage.
- Model firmware and device differences in one place so new firmware or a
  new device does not touch feature code.

Non-goals for v1

- Editing synth parameters or sample audio inside patches.
- User accounts, ratings, comments or any state of our own on a server.
- Supporting browsers that offer neither the File System Access API nor
  folder drag-and-drop.

## 3. Decisions already made

| Topic | Decision |
| --- | --- |
| Core scope | Personal device librarian and community catalogue together in v1 |
| Hosting | GitHub Pages static site plus a free Cloudflare Worker |
| Community patch storage | In the repo under `packs/`, submitted by pull request |
| Feedback | In-app form; Worker opens a labelled GitHub Issue |
| Device access | File System Access API on Chromium, drag-and-drop fallback elsewhere |
| op1.fun | Browse directly from the browser (API reads allow CORS); patch files through the Worker file proxy (op1.fun's S3 bucket sends no CORS headers); upload via the Worker (the API refuses cross-origin POST); credentials in memory unless the user ticks Remember |
| Firmware model | `DeviceProfile` registry in order stock, hundekraft, opus; stock is the base profile |
| Firmware detection | Folder detection on open; a content sniff over loaded patches may only suggest a switch, never switch, because sounds prove where they were made, not what is installed |
| Compatibility | Judged from sound traits against each profile's supported names; stock is strict (custom-firmware sounds may stop it at boot), hundekraft and opus warn about missing plugins |
| Write guards | Pack and op1.fun installs and restores confirm before writing patches the active profile marks unsupported; copy and move within the drive do not ask |
| Remembered firmware | The profile last chosen by hand is stored in `localStorage` and wins on the next open, unless folder detection proves another (0.97 or more, opus by its mods folder) |
| USB check | Read only, over Web MIDI SysEx with hundekraft 3.x's kennel protocol; started by a button, never on open, because disk mode and MIDI are exclusive |
| Plugin library | In the repo under `plugins/`, one folder per plugin with the file, `manual.md` and a screenshot, submitted by pull request like packs; the index is built at deploy |
| Plugin licences | SPDX ids from a fixed list of open licences plus `LicenseRef-Freeware`; a source link is optional but encouraged, since a plugin runs as firmware code |
| hundekraft plugin installs | Over USB MIDI from co-op (only `plugins/<id>.knl` or `.op1p`, never DELETE, OFF_SET or RESTART), or by download for the hundekraft workshop; a plugin whose id is already installed is refused (remove it in the workshop first) |
| opus plugin installs | Copied into `mods/` through the drive store and `writeAtomic`, asking before replacing a file of the same name |
| Plugin requirements | Matched by saved name against the library for the active firmware, built-in names left out; on hundekraft a library engine or effect warns `needs plugin "<id>"` and is ok once the USB check shows it loaded |
| Plugin prompt | Pack, op1.fun and restore installs ask after the write guard when patches need plugins not known to be there; Cancel is the default; skipped without the plugin index |
| Manual rendering | A small Markdown parser written for safety: React elements only, raw HTML kept as text, links to http(s) or the plugin folder, images from the plugin folder |
| Reference firmware repos | `vendor/hundekraft`, `vendor/opus`, gitignored |
| Web stack | Vite, React, TypeScript |

## 4. Repository layout

```
web/        Vite + React + TypeScript app, built to static files for Pages
worker/     Cloudflare Worker: feedback endpoint and op1.fun upload proxy
shared/     Types and validators used by web, worker and CI
packs/      Community packs, one folder each (index.json is generated at build time)
plugins/    Community firmware plugins, one folder each (index.json is generated at build time)
docs/       Specs, contributor guide, pack submission guide
vendor/     Gitignored reference clones of hundekraft and opus
.github/    Actions workflows, issue templates, PR template
```

The repo is a pnpm workspace with `web`, `worker` and `shared` as packages.
`shared` holds the pack manifest schema, the plugin manifest schema and
file checks, and the feedback payload schema.

## 5. Web app architecture

The app is layered so that browser APIs, device and firmware specifics, and
UI never mix.

```
web/src/
  ui/          React screens, components and hooks (useDevice, useOp1Fun), one tab each
  core/
    codec/     AIFF reader/writer, OP-1 APPL JSON chunk, patch model, sound traits
    store/     DeviceStore interface and adapters (File System Access, dropped folder,
               in-memory), atomic writes and recovery
    library/   scanning and loading, operations, staged changes, backup and restore,
               drive recovery, opus plugin listing
    profiles/  DeviceProfile registry (op1/stock, op1/hundekraft, op1/opus),
               compatibility from traits, write-guard check
    catalogue/ pack index client and installs
    op1fun/    op1.fun client and installs
    usbmidi/   kennel SysEx framing, session, inventory parser, the USB check and
               plugin installs (pure: a port is send, onSysex and close)
    plugins/   plugin index client, the safe manual Markdown parser, plugin requirements
    feedback/  feedback client
    util.ts    small shared helpers
```

`core/` is pure TypeScript with no React and no direct browser globals
except inside the store adapters. Web MIDI is opened in `ui/webmidi.ts`
and handed to `core/usbmidi` as a plain port. It is unit tested with Vitest against
fixture files.

### 5.1 Patch model and codec

A `Patch` is the in-memory representation of one patch file:

- `kind`: `synth`, `drum` or `unknown`. Drum covers the stock sample kit
  (`drum`), the stock DBox drum synth (`dbox`) and hundekraft Gin kits
  (`gin`).
- `name` from the metadata, `comm` (sample rate, channels, bit depth,
  compression), `form` (the parsed chunk list)
- `meta`: parsed JSON from the OP-1 `APPL` chunk, kept as a typed object
  with an escape hatch for unknown fields
- `raw`: the current bytes. For an unedited file these are the original
  bytes, so copy, move, back up and restore never re-encode. After an edit
  they are the re-serialised file.
- `op1Canonical`: whether re-serialising the metadata reproduces the
  original `APPL` chunk byte for byte. Custom firmware may format the JSON
  differently; such files stay readable and are flagged, and only an edit
  canonicalises them.

A `Patch` is file content only. The device-level record (path on the
device, folder, size, scan kind) is a separate type owned by the device
library (phase 3). Audio is decoded on demand with `decodePcm` for preview;
frame count comes from the `SSND` chunk length, never from `COMM`.

The codec reads and writes AIFF with these requirements:

- Preserve every chunk, including unknown ones, byte for byte on round trip.
- Parse and serialise the OP-1 `APPL` chunk JSON, maintaining chunk sizes,
  padding and the FORM length.
- Handle both container forms the device writes: plain `AIFF` (big-endian
  PCM) and `AIFC` with `sowt` (little-endian PCM) or `NONE`/`twos`
  (big-endian). Real OP-1 files are `AIFC`/`sowt`; synth sampler files can be
  22.05 kHz, so the sample rate is always read from `COMM`.
- A round-trip test over every fixture must produce identical bytes when no
  edit was made.

v1 only rewrites a file when the user edits metadata (for example the name
stored in the chunk). Copy, move, rename, delete, back up and restore work
on raw bytes.

### 5.2 Device profiles

```ts
interface DeviceProfile {
  id: string;                     // "op1/stock", "op1/hundekraft", ...
  device: "op1" | "op1-field" | "opxy";
  firmware: string;
  label: string;
  layout: DriveLayout;            // folders to scan and what each holds
  patchKinds: PatchKindSpec[];    // kind, extensions; the codec classifies every kind so far
  detect(root: DirectoryListing): Promise<DetectResult>; // folder-level; confidence 0..1 plus reason
  sniff?(traits: SoundTraits[]): DetectResult;  // content-level, over loaded patches
  supports: {                     // names as saved in sounds
    synthEngines: readonly string[];
    drumEngines: readonly string[];
    fx: readonly string[];
    lfo: readonly string[];
  };
  compatibility(patch: Patch): Compat;          // ok | warn | unsupported, from traits
  pluginCompatibility?(context: PluginContext): DeviceProfile["compatibility"]; // see 5.6
}
```

Profiles never read raw JSON to judge a patch. `core/codec/sound-traits.ts`
extracts `SoundTraits` from any metadata: kind, engine (`type`), effect
(`fx_type`), LFO (`lfo_type`), an op1r selector word in `knobs[4]` or
`fx_params[4]` (0x5300..0x53ff), an opus engine mark (`knobs[4]` of 0x100
or more with an `opus` member), Gin kit members, and the `opus` member's
plugin and effect slots. `traitCompatibility(supports, { rule })` marks every
name outside `supports` unsupported and lets the profile's rule add, drop or
soften findings. The codec reads `drum`, `dbox` (stock DBox) and `gin`
(hundekraft Gin) as drum kinds.

- `op1/stock` is written first from the real device layout, which the
  implementation plan verifies against a mounted OP-1. Expected top-level
  folders are `album`, `drum`, `synth` and `tape`, with `synth` and `drum`
  holding category subfolders of patches.
- `op1/stock` is strict, because stock R246 can stop at boot on a sound
  from custom firmware: a non-stock engine, effect or LFO, or an `opus`
  member, is `unsupported` with "may stop a stock OP-1 at boot". A
  selector-like fifth knob or effect value with otherwise stock names is
  only a warning, since 0x5300..0x53ff is also a legal stock value. The
  stock name lists are confirmed in `docs/research/2026-10-28-custom-firmware-notes.md`.
- `op1/hundekraft` (phase 7) has the stock layout and folder detection at
  0.85 (stock 0.9), so stock wins on folders alone. It supports the stock
  names plus its own engines, effects, Gin kits and the `quad` LFO; a
  missing plugin is a warning (an unknown engine with a selector plays as
  stock Iter, an unknown effect with a selector as the stock filter), and
  an opus sound needs the opus bridge. Its sniff returns 0.95 when a loaded
  patch uses a hundekraft-only name or a Gin kit.
- `op1/opus` (phase 8) keeps the stock patch roots and adds `mods` to
  `otherRoots` and `backupOtherRoots` (`DriveLayout.backupOtherRoots`
  lists the other roots backups include; only opus has one). Detection: a
  `mods/` folder with at least one `.opmod` file is 0.97, `mods/`
  without one 0.6, otherwise the stock folder rule capped at 0.5 (an opus
  drive without plugins looks like stock). Detection reads names only; the
  library's read-only "Installed opus plugins" section lists `mods/*.opmod`
  in load order with sizes and flags files that do not start with `OPMD`.
  It supports the stock names plus the opus starter effects in `fx_type`
  (bitcrush, chorus, plate, filter, bare and as `opus.*`); opus engines save
  as stock `iter`. Compatibility warns "needs plugin <id>" for the `opus`
  member's plugin and effect slots, and "needs the opus bridge plugin" for
  hundekraft names and op1r selectors; names no known firmware uses stay
  unsupported. Its sniff is 0.96 when a loaded patch has an `opus` member:
  above stock's 0.9 and hundekraft's 0.95 (opus hosts hundekraft sounds
  through its bridge), below the mods-folder match.
- Detection runs on every mount and picks the best folder match. After all
  patches are read, the library runs every profile's `sniff` over their
  traits; if another profile beats the active profile's folder confidence,
  a dismissible notice offers "Switch to <label>". It never switches by
  itself: patch contents prove where sounds came from, not which firmware
  is installed now. A profile chosen by hand is never second-guessed.
  Every patch shown anywhere carries a compatibility badge for the active
  profile, with a details popover listing its traits.
- The profile is remembered (phase 9). A hundekraft drive looks exactly
  like a stock one in disk mode, so without this the choice would have to
  be made again every visit. Choosing a profile by hand (the Library
  dropdown, the suggestion's Switch button, or "Use OP-1 (hundekraft)" in
  the USB check) stores it in `localStorage` under `co-op.profile` as
  `{ id, at }`; detection never stores anything, and an id no longer in
  the registry reads as nothing. The next connect or drop starts on the
  remembered profile, with "remembered from last time" as the detection
  line, unless folder detection finds a different profile at 0.97 or more
  (today only opus by its mods folder, the one folder fact that proves the
  firmware); then the line gives the detection reason and adds "(you chose
  <label> last time)". The content sniff still runs against the remembered
  profile. Every storage access is in try/catch: without storage the
  choice holds for the page only.
- Writes that bring patches onto the device are guarded. Pack installs,
  op1.fun installs and restores first check the active profile's
  compatibility; any
  `unsupported` patch needs an explicit confirm in a shared dialog that
  names each patch and reason (and, on stock, the boot risk). Cancel is
  the default. Files that do not parse as patches are left to the
  writers' own checks. Copy and move within the drive do not ask: the
  file is already on the device.

### 5.3 Device store

```ts
interface DeviceStore {
  readonly capabilities: { write: boolean };
  /** Human label for the source, e.g. the picked folder name. */
  readonly label: string;
  /** Sorted by code point; [] for a missing path or a file. */
  list(path: string): Promise<DirEntry[]>;
  read(path: string): Promise<Uint8Array>;
  /** Size in bytes without reading the file. */
  size(path: string): Promise<number>;
  exists(path: string): Promise<boolean>;
  /** Creates parent folders; overwrites. */
  write(path: string, bytes: Uint8Array): Promise<void>;
  /** Creates the target's parent folders; refuses an existing target. */
  move(from: string, to: string): Promise<void>;
  /** Files only, never folders. */
  remove(path: string): Promise<void>;
  /** Creates a folder and its parents; no-op if present. */
  mkdir(path: string): Promise<void>;
}
```

Paths are `/`-separated and relative to the drive root; failures throw
`StoreError`. There is no `watch`: the library reloads after each operation
and on demand. File contents are written through `writeAtomic` (temp file,
old file set aside, then a move), and `recoverStranded` puts back anything
an interrupted write left behind when a drive is opened.

- `FsAccessStore` wraps a directory handle from the File System Access API.
  Full read and write. The handle is persisted in IndexedDB so reconnecting
  is one click.
- `DropStore` wraps a folder dropped onto the page or picked with a
  `webkitdirectory` input. Read only. A `StagedStore` layered over it
  collects every change, which the user downloads as a zip with
  instructions (`CHANGES.txt`) for applying it to the device by hand.
- `MemoryStore` holds a drive in memory for tests.

Feature code only sees `DeviceStore` and `capabilities`, and disables
write actions when `write` is false.

### 5.4 Features

Device library

- Mount, scan with the active profile, show a tree by folder and a flat list
  by kind with search and sort.
- Preview audio in the browser using the codec's PCM output.
- Rename, move, copy, delete, with confirmation on destructive actions.
- Back up: zip the whole patch tree or a selection, with a manifest.
- Restore: pick a backup zip, show a diff against the device, apply.

Community catalogue

- Fetch `packs/index.json` from the same origin.
- Browse by pack, author, kind, firmware compatibility; preview patches.
- Install a pack or a single patch to a chosen folder on the device, or stage
  it for download when the store is read only.
- "Share a pack" opens the contributor guide.

op1.fun

- The op1.fun tab holds the email, API token and username. They stay in
  memory for the page unless "Remember on this device" is ticked, which
  stores them in `localStorage` (`co-op.op1fun`); Forget clears both. The
  token goes only to `api.op1.fun` (redirects refused) and to the Worker's
  upload route. Test connection loads the user, since the API has no "me"
  call.
- Browse any user's patches and packs (filtered in the browser; the API has
  no search), preview with an `<audio>` element on the MP3 link, and
  install a patch or every patch of a pack (from the pack's `included`
  list, not the zip). op1.fun's files live in the `op1fun` S3 bucket,
  which sends no CORS headers, so installs need the Worker: without a
  Worker URL, Install and Install all are disabled and the reason is shown
  above the list. Each file is fetched without credentials (the client
  tries the link directly, then the Worker file proxy on the resulting
  network/CORS failure; 20 MB cap), checked with `readPatch`, and written unchanged through the catalogue
  install path into `<synth root>/op1fun` or `<drum root>/op1fun` (kind
  from the file, else op1.fun's patch type) under a free name, as one
  queued device action. Failures are reported per file and the rest carry on.
- Upload a library patch: a dialog (name, description, licence blank or
  one of the four CC values, public) sends the raw file bytes as a data URI
  to the Worker, which forwards it to op1.fun with the user's headers and
  returns the response; the dialog links to the new patch. Files over
  9 MiB and descriptions over 2000 characters are refused before sending,
  and the request times out after 65 s. The action is disabled, with the
  reason shown once above the library table, when the build has no Worker
  URL or no credentials are saved. The Worker never
  logs or stores credentials or file contents.

Feedback

- Form with category (bug, idea, pack request, other), message, and an
  optional contact field.
- The app attaches: app version and commit, active profile, store type,
  browser user agent, and whether a device was mounted.
- Submission goes to the Worker, which opens a GitHub Issue labelled
  `feedback` plus the category. The user gets the Issue URL back.
- The Feedback button sits in the app header. The dialog lists exactly what
  is attached. A build knows the Worker through `VITE_WORKER_URL` and
  `VITE_TURNSTILE_SITE_KEY`; when either is missing it shows the same
  fields with an "Open GitHub issue" link prefilled with the same title and
  body (capped at 6000 characters by cutting the message, with a note).
  The Turnstile script is loaded only when the dialog opens in Worker mode.
- `shared/src/feedback.ts` holds the payload schema, validation, and the
  issue title and body. Every user string is treated as hostile: mentions,
  `#123`, `GH-123` and `github.com` get a zero-width space so feedback
  cannot ping people or cross-reference issues; ampersands, angle brackets,
  backticks, backslashes and pipes become entities; control and bidi characters are stripped; the message is quoted
  line by line, never inside a fence the user could close; lone surrogates
  are rejected.

Plugins

- The Plugins tab, after Packs, lists the plugin library (section 6.1):
  cards with the screenshot (lazy, with alt text), name, kind, author,
  version, licence, requirements, description, credits and source link,
  filtered by kind, firmware, tag and text. "Read the manual" renders
  `manual.md` in the tab. Every plugin has Download, which checks the file
  against the index's size and SHA-256 first; hundekraft plugins carry a
  safety note beside it (a plugin runs as firmware code). The tab takes its
  install buttons from `App` (Install over USB for hundekraft, Copy to
  mods/ for opus, section 5.6), so it holds no device code itself.
- Manuals are rendered by a small Markdown parser written for safety, not
  completeness (`core/plugins/markdown.ts`). It turns the text into plain
  data that the UI renders as React elements, so nothing in a manual
  reaches the page as HTML. Supported: ATX headings, paragraphs, hard
  breaks, emphasis and strong, inline and fenced code, nested lists, GFM
  tables, block quotes, rules, links, images, autolinks and backslash
  escapes. Raw HTML, reference links, setext headings, indented code and
  footnotes are shown as text, as is anything nested deeper than eight
  levels. Links go only to http(s) (opened in a new tab) or into the
  plugin's own folder; images come only from that folder.

### 5.5 USB MIDI check

Disk mode cannot tell hundekraft from stock, but hundekraft 3.x answers a
SysEx protocol (kennel) on the OP-1's USB MIDI port. "Check over USB" in
the Library header, with or without a drive open, opens a panel that asks
it. The protocol is recorded in
`docs/research/2026-10-29-hundekraft-usb-and-plugins.md`; co-op's
implementation is its own (the hundekraft sources are GPL and only facts
are taken from them).

- Disk mode and MIDI are exclusive on the OP-1, so the panel first says
  to eject the drive and leave disk mode, and checks only when Check is
  pressed (then Check again). Nothing on the OP-1 changes.
- `ui/webmidi.ts` asks for `navigator.requestMIDIAccess({ sysex: true })`
  and picks the connected input and output whose names match `/op-?1/i`
  and do not contain "Field" (an OP-1 Field has no kennel protocol). The
  message handler is set before the ports open. Errors name what is wrong:
  no Web MIDI (with the browsers that have it), permission refused, or no
  OP-1 port, in which case it lists the ports the browser does see and
  explains that on Windows one program at a time can hold a MIDI port. The
  port is closed after each check, and a disconnect during the read is
  reported.
- `core/usbmidi` is pure. `framing.ts` packs and unpacks the 7-bit
  payload, refuses any message over 96 bytes (a longer SysEx stops the
  stock OP-1's MIDI input until it restarts), reassembles SysEx across
  callbacks skipping realtime bytes, and drops a message that grows past
  96 bytes. `session.ts` keeps one message in flight with a fresh 14-bit
  seq per try, ignores stale replies, and retries (3 s, 5 tries by
  default). `check.ts` sends HELLO, pages LIST (64 bytes at a time) and
  both OFF_GET lists, and returns either `kennel` (version, capabilities,
  inventory, switched-off ids) or `silent` (the port is there but HELLO
  gets no answer).
- The panel shows the firmware ("hundekraft 3.x (kennel protocol <n>)"),
  the plugin table (name, kind, source, version, author, state and
  reason), the switched-off list and, when the core reports it, "Synth
  presets: <count> of <cap> (reported by the OP-1)", which co-op only
  displays. A silent port gets the explanation: TE's firmware, op1r,
  hundekraft 2.7 or older, or busy restarting.
- "Use OP-1 (hundekraft)" always remembers the profile, but switches the
  open library at once only for a dropped, read-only copy, which lives in
  memory. A connected drive is gone while the OP-1 is out of disk mode, so
  the panel says it starts on hundekraft when it is opened again (or that
  the next drive does).
- The last inventory is kept in `useDevice` as `installedPlugins`, in
  memory only, for plugin requirements (5.6).

### 5.6 Plugin installs and requirements

Install over USB (hundekraft)

- `core/usbmidi/install.ts` sends only `plugins/<id>.knl` or
  `plugins/<id>.op1p` and never sends DELETE, OFF_SET or RESTART: removing
  a plugin is left to the hundekraft workshop. Every refusal is checked
  before the first write: the path; the file checks of section 6.1 again
  on the downloaded bytes, with the saved name giving the id in the path;
  HELLO reporting both the inventory and the plugin-file capabilities; and
  no file of the same id in `plugins/` or `content/`, or plugin record of
  that name, in any state (built-ins do not count). Replacing a plugin in
  place can hang the OP-1's restart, so the message says to remove it in
  the workshop first.
- Then ABORT (clearing the staging area), and per file BEGIN (size, CRC-32,
  path), DATA in 72-byte pieces with one in flight (a gap or a lost reply
  carries on from the OP-1's count), END, and COMMIT, polled while the
  OP-1 answers "wait key". Control steps time out after 10 s and data
  after 3 s, 5 tries each. A resent BEGIN answering "state" counts as
  opened (its first try opened the file and lost the reply); a resent END
  answering "state" fails, since it cannot tell whether the file was
  staged. A failed transfer sends no cleanup ABORT: nothing staged applies
  without COMMIT and T1, and the next install starts with ABORT.
- The dialog confirms first (Cancel focused): it names the file, says a
  plugin runs as firmware code, that the OP-1 asks for T1 and restarts,
  and that an OP-1 that does not start is switched off and on to start
  without plugin files. Then it shows progress and "Press T1 on the OP-1
  to install and restart, or T2 to cancel".
- From just before the first COMMIT until T1, T2 or silence the dialog
  cannot be closed (no button, Escape ignored); focus moves to the
  instruction. Once T1 is pressed the OP-1 keeps its prompt up, takes no
  more keys and applies the next COMMIT from any host at once, so a host
  that left during the prompt could leave a later COMMIT to apply without
  a new T1.
- T2 means nothing was installed. Silence after the prompt (or the ports
  going away) is taken as the restart after T1. The dialog waits for the
  OP-1's ports to return, checks again (retrying for up to 60 s while it
  boots), updates `installedPlugins` and shows the plugin's state: loaded,
  failed, off, skipped (the last boot did not reach the tape screen) or
  missing.

Copy to mods/ (opus)

- With an opus drive open, "Copy to mods/" downloads and checks the
  plugin against the index and writes `mods/<file>.opmod` with
  `writeAtomic` as one queued operation (the folder as the drive spells
  it, created when missing). A file of the same name in any case is
  replaced only after a confirm, keeping the drive's spelling. On a
  dropped drive the write is collected into the changes zip like any
  other. The reload refreshes the Installed opus plugins list. The panel
  notes that opus loads `mods/` in file-name order; without an opus drive
  the button says what is needed.

Patches that need a plugin

- The plugin index loads with the page, not only with the Plugins tab,
  because compatibility uses it; the library is judged again when the
  index or the USB check changes what is known.
- `requiredPlugins(traits, index, firmwareId)` returns the library
  plugins for the active firmware whose saved name (from the `.knl` or
  `.op1p` header, or `plugin.json` for opus) is a synth sound's engine,
  the effect, or the `opus` member's plugin or effects. Names the firmware
  has built in are left out. `installedNames(inventory)` gives the saved
  names of records the USB check found loaded.
- Profiles stay pure: `withPlugins(profile, context)` hands
  `pluginCompatibility` the library (saved name to id) and the loaded
  names. On hundekraft a library engine or effect is then a warning,
  `needs plugin "<id>"`, instead of the generic finding, and ok once
  loaded; the other findings are unchanged. Profiles without
  `pluginCompatibility` (stock, opus, whose own "needs plugin <id>"
  warnings stand) or a missing index leave the profile as it is. Rows,
  write guards and the Inspector all use the wrapped profile, and the
  traits popover names the plugins a row needs.
- Pack, op1.fun and restore installs, after the write guard and before
  writing, collect the plugins the patches need that are not known to be
  there (loaded in the last USB check for hundekraft; a valid `.opmod` of
  that name in `mods/` for opus) and ask "This patch needs the plugin
  <name>. Install it?" with Install over USB (hundekraft), Copy to mods/
  (opus) or Download for each, then Continue without or Cancel (the
  default, like the other install guards). An install waits at most 5 s
  for the plugin index and skips the prompt without it.

## 6. Community packs

```
packs/
  <slug>/
    pack.json
    README.md       optional
    synth/<file>.aif
    drum/<file>.aif
```

Patches sit directly in `synth/` or `drum/` (lower case, no deeper
folders). No other files, and no links, are allowed in a pack folder.

`pack.json` fields: `format` (1), `name` (1 to 60 characters), `slug`
(equals the folder name, lower-case words joined by single hyphens, at most
40 characters), `author` (1 to 60), `description` (1 to 500), `license`,
`firmware` (non-empty list of known profile ids the pack is made for),
`tags` (optional, up to 8 of `[a-z0-9-]{1,20}`), and `patches` (1 to 100
entries of `{ file, notes? }`). `license` is one of five Creative Commons
licences: `CC0-1.0`, `CC-BY-4.0`, `CC-BY-SA-4.0`, `CC-BY-NC-4.0`,
`CC-BY-NC-SA-4.0`. All-rights-reserved packs are not accepted: the catalogue
exists so people can install and use what they find.

Validation (the pure functions in `shared/src/packs.ts`, run by
`web/scripts/packs.ts`): every rule above; patch file names FAT-safe and at
most 64 characters, not a Windows reserved name, unique ignoring case; every listed file present with that
exact spelling and nothing unlisted; every file parses with the codec as the
kind its folder names. Limits: 2 MB per patch, 20 MB of patches per pack,
64 KB each for `pack.json` and `README.md`.

CI runs `pnpm --filter @co-op/web packs:validate` on every pull request;
maintainers merge only when the check passes (there is no branch protection,
and a first-time contributor's run needs a maintainer's approval to start).
`.DS_Store`, `Thumbs.db` and `desktop.ini` in a pack folder are ignored and
never published. **`index.json` is not committed.** The Pages
build (`pnpm --filter @co-op/web build`) validates the packs again, copies
each pack's `pack.json`, `README.md` and listed patches into
`web/dist/packs/<slug>/`, and writes `web/dist/packs/index.json` (format 1:
each pack's fields, total size, whether it has a README, and per patch its kind,
name, engine, size, SHA-256 and notes), so the index can never drift from the files. The dev server
builds the same index on the fly. The app fetches `packs/index.json` from
its own origin, revalidating each time, and checks every downloaded patch
against its SHA-256 before playing or installing it.

The contributor guide (`docs/contributing-packs.md`) walks through forking,
adding a folder (with git or in the GitHub web UI), validating and opening
the pull request. The pull request template asks for licence confirmation.

### 6.1 Plugin library

Firmware plugins are shared the same way as packs: a folder per plugin in
the repo, submitted by pull request, validated in CI and indexed by the
Pages build.

```
plugins/
  <id>/
    plugin.json
    <id>.knl | <id>.op1p | <name>.opmod
    manual.md
    screenshot.png
```

Nothing else may be in the folder (OS noise is ignored as in packs), and
the folder name equals the id.

`plugin.json` format 1: `format` (1), `id`, `name` (display), `firmware`
(`op1/hundekraft` or `op1/opus`), `kind` (`synth`, `fx` or `feature`),
`author`, `version` (free text, at most 20), `description` (at most 500),
`license`, `file`, `screenshot` (`.png`), `manual` (`manual.md`), and
optionally `source` (an https URL), `requires` (free text such as
"hundekraft 3.0.3", at most 60), `credits` (at most 200), `tags` (the
packs rule) and, for opus, `savedName` (the `opus.<id>` name sounds save,
used for requirements). `license` is an SPDX id from `GPL-3.0-or-later`,
`GPL-3.0-only`, `GPL-2.0-or-later`, `MIT`, `BSD-3-Clause`, `Apache-2.0`,
`MPL-2.0`, or `LicenseRef-Freeware` for closed plugins that are free to
use. Ids: hundekraft `[a-z0-9_-]{1,15}` (the file is `<id>.knl` or
`<id>.op1p`), opus `[a-z0-9_.-]{1,40}`.

Validation (the pure functions in `shared/src/plugins.ts`, run by
`web/scripts/plugins.ts`; layouts in
`docs/research/2026-10-29-hundekraft-usb-and-plugins.md`):

- `.knl`: the optional screen SVG, its NUL and zero padding; the 144-byte
  header (magic `KNPL`, format 1, header size, file size equal to the
  length, CRC-32 over the file with its own 4 bytes as zero, API 1, kind 1
  to 3 matching `plugin.json`, legacy order at most 31, saved name of 1 to
  15 printable ASCII characters giving the id with spaces as `_`, the SVG
  size and reserved SVG fields); size a multiple of 4 and at most 512 KiB;
  the SVG boot hazard (a number with five or more decimals hangs R246's
  boot); and the optional extension table, whose offset is read from the
  file start. Its author and version are returned, and a mismatch with
  `plugin.json` is a warning, not an error.
- `.op1p`: the trailer and binary rules, kind 0 synth or 1 effect, the id
  rule, and the SVG hazard on any SVG prefix. The binary is at most
  512 KiB (op1r's limit) and the whole file at most 1 MiB (co-op's own
  limit, with room for the SVG).
- `.opmod`: the `OPMD` magic and at most 2 MiB, for `op1/opus` only;
  nothing more of the format is known.
- The screenshot is a PNG of 64 to 2048 pixels each way and at most 1 MiB;
  `manual.md` is non-empty UTF-8 of at most 64 KiB; `plugin.json` at most
  64 KiB.

`index.json` is not committed. The Pages build validates the plugins
again, copies each plugin's four files into `web/dist/plugins/<id>/` and
writes `web/dist/plugins/index.json` (format 1: the manifest fields plus
`sizeBytes`, `sha256`, `savedName` and the header's author and version
when present, and `generatedAt`, fixed by `PLUGINS_NOW` in tests). An
absent `plugins/` folder gives an empty index. The dev server serves
`/co-op/plugins/` the same way. The app fetches the index with
`cache: 'no-cache'` and checks every plugin file against its size and
SHA-256, retrying a mismatch once without the cache, before saving or
installing it.

The library is seeded with ten hundekraft plugins by the owner (rings,
bitcrusher, drive, tape drive, wavefolder, freq shift, ensemble, tape
echo, marbles, grids) from `coldsoldr/te-op1-cfw` branch `hk-plugins`,
GPL-3.0-or-later with that repo as the source link; credits follow the
owner's README (Mutable Instruments ports by Émilie Gillet: Rings,
Marbles and Warps under MIT, Grids under GPL-3.0, and tape echo's wow and
flutter from Beads under MIT).

A plugin runs as firmware code, so maintainers merge only from authors
they trust or with source they have looked at. `docs/contributing-plugins.md`
covers the folder, the checks, that risk, installing and updating;
`docs/plugin-manual-prompt.md` is a prompt authors give their coding agent
so every manual has the same sections (summary, requirements, the
encoder table by colour, second pages and Shift, on screen, tips, what is
saved with a sound, credits and licence). The pull request template has a
plugin checklist and there is a plugin request issue form.

## 7. Worker

Cloudflare Worker, TypeScript, deployed with wrangler.

Endpoints

- `POST /feedback` Refuses bodies over 16 KB (counted as the body streams,
  413) and non-JSON (400), validates the payload against the shared schema
  (400 with the errors), applies the rate limits (429, `Retry-After: 60`),
  verifies the Turnstile token with siteverify, requiring `success` and a
  `hostname` that belongs to `ALLOWED_ORIGINS` (403; siteverify unreachable
  or `internal-error`, `invalid-input-secret` or `missing-input-secret` is
  502), then creates an Issue through the GitHub REST
  API using a fine-grained token scoped to Issues on this repo (any failure
  is 502 without GitHub's answer). Both subrequests time out after 10 s.
  Returns 201 `{ url, number }`.
- `POST /op1fun/patches` Validates that the body matches op1.fun's create
  patch shape and size limit, forwards it with the caller's `X-User-Email`
  and `X-User-Token` headers to `https://api.op1.fun/v1/patches`, and
  returns the upstream status and body unchanged. Both headers are
  required (401); the body is capped at 13 MiB while it streams (413) and
  must be a `patches` resource whose `file` is an
  `data:audio/x-aiff;base64,` URI (400). Only those two headers and
  `Content-Type: application/json` are forwarded, redirects are refused,
  and the subrequest times out after 60 s (502).
- `GET /op1fun/file?url=` The file proxy op1.fun installs go through,
  since op1.fun's S3 bucket sends no CORS headers. Only `https:` URLs (no
  credentials or port) on `op1.fun`, `www.op1.fun`, the bucket's own hosts
  (`op1fun.s3.amazonaws.com`, `op1fun.s3.<region>.amazonaws.com`) or path
  style under `/op1fun/` on `s3.amazonaws.com` or
  `s3.<region>.amazonaws.com`, whose path ends in `.aif`, `.aiff` or
  `.mp3`, are fetched (400 otherwise; other buckets and AWS hosts are
  refused), without credentials and with `redirect: 'manual'`: an
  upstream redirect is a 502, not followed. The body streams back cut off
  once it passes 20 MB (a larger declared `Content-Length` is a 413 up
  front), with `Content-Type` limited to `audio/x-aiff`, `audio/aiff` or
  `audio/mpeg` (anything else becomes `application/octet-stream`),
  `Content-Disposition: attachment` and
  `Content-Security-Policy: default-src 'none'; sandbox`.

Rules

- CORS allows only the exact origins in the `ALLOWED_ORIGINS` var. In
  `wrangler.toml` that is the Pages origin alone; localhost is allowed only
  through `.dev.vars` for `wrangler dev`, with a separate development
  Turnstile widget. Other origins, and requests
  without one, get 403; preflights from them get no CORS headers. Allowed
  headers are `Content-Type, X-User-Email, X-User-Token`; `Retry-After` is
  exposed.
- Rate limiting uses the Workers Rate Limiting binding (`[[ratelimits]]` in
  `wrangler.toml`): `FEEDBACK_LIMIT`, 5 a minute per client keyed by
  `CF-Connecting-IP` (IPv6 by its /64, IPv4-mapped IPv6 as the IPv4
  address), and `FEEDBACK_GLOBAL_LIMIT`, 30 a
  minute under one `global` key. The op1.fun routes use `OP1FUN_LIMIT`, 60
  a minute per client, keyed `op1fun-upload:<client>` and
  `op1fun-file:<client>` (a pack install through the proxy fetches one file
  per patch, so the feedback limit would be too low). Counts are per
  Cloudflare location.
- No persistent storage. No logging of request bodies or headers.
- Vars: `ALLOWED_ORIGINS`, `GITHUB_REPO` (`coldsoldr/co-op`). Secrets:
  `GITHUB_TOKEN`, `TURNSTILE_SECRET`, set with `wrangler secret put` (or
  uploaded by `worker.yml` with the deploy, through `--secrets-file`, from
  the repo secrets `FEEDBACK_GITHUB_TOKEN` and `TURNSTILE_SECRET`), never
  committed. `.dev.vars` is gitignored.
- Tests call the handler directly with `Request` objects and a stubbed
  global `fetch`; there is no Miniflare dependency. The owner's setup is in
  `docs/deploying-the-worker.md`.

Why a proxy for uploads: api.op1.fun answers preflight with
`Access-Control-Allow-Methods: GET, OPTIONS` only, so browsers block a
cross-origin POST. GET requests with the auth headers are allowed and go
direct.

## 8. Deployment

- `pages.yml`: on push to `main`, check, run the Playwright test, build `web/`
  (which validates the packs and writes `dist/packs/index.json`), upload to
  GitHub Pages. The build passes the repository variables `WORKER_URL` and
  `TURNSTILE_SITE_KEY` as `VITE_WORKER_URL` and `VITE_TURNSTILE_SITE_KEY`,
  and `GITHUB_SHA` becomes the commit shown in feedback context.
- `worker.yml`: on push to `main` touching `worker/`, `shared/` or the
  lockfile, and on demand: test, typecheck and `wrangler deploy --dry-run`,
  then deploy with wrangler only when the `CLOUDFLARE_API_TOKEN` and
  `CLOUDFLARE_ACCOUNT_ID` repo secrets are set (a `check` step turns their
  presence into an output, since `secrets` cannot be used in `if:`), and
  only from `refs/heads/main`. The optional Worker secrets are uploaded in
  the same `wrangler deploy --secrets-file` call, so a first deploy is never
  without them. Checkout does not persist credentials. Without the
  Cloudflare secrets the deploy is skipped with a notice and the job passes.
- `ci.yml`: typecheck, lint, unit tests (web, shared and worker), pack
  and plugin validation, build and the Playwright smoke test on every PR
  and push. On pull requests it also warns when a PR changes files under
  `packs/` or `plugins/` and elsewhere, or under both (diffing the merge
  commit against its first parent, with two commits of history). There is
  no separate packs or plugins workflow: both are validated in `ci.yml`
  and indexed by the Pages build, which runs both builders.

The owner supplies a Cloudflare account, a Turnstile widget and the GitHub
fine-grained token (`docs/deploying-the-worker.md`). Until the Worker is
deployed, feedback falls back to a prefilled GitHub issue link and op1.fun
install and upload show as unavailable rather than failing.

## 9. Error handling

- Device operations are atomic per file: write to a temp name, then move.
  A failed batch reports which files succeeded and stops.
- The codec rejects containers it cannot parse (truncated chunks, missing
  pad bytes, trailing bytes, more than one op-1 chunk) and the UI marks them
  as unreadable rather than hiding them. Metadata it would re-serialise
  differently is flagged via `op1Canonical`, not rejected; unchanged
  operations always copy `raw`.
- Network features degrade independently: a Worker outage disables feedback
  and op1.fun install and upload while the catalogue and device library keep working.
- op1.fun 401 responses prompt the user to re-check credentials; other
  upstream errors are shown with the upstream message.

## 10. Testing

- Vitest: codec round trip on fixtures for every supported kind; profile
  detection on synthetic directory listings; manifest validation; Worker
  handlers with mocked fetch.
- Playwright: mount a fixture folder through drag-and-drop, browse, preview,
  stage a change, download the zip; packs and op1.fun installs (op1.fun and
  the Worker mocked with page.route); firmware suggestion, write guard and
  opus plugin listing; the remembered profile; the Plugins tab, manuals
  and downloads; the USB check and plugin installs against a Web MIDI stub
  of a hundekraft OP-1 (`web/e2e/op1-stub.ts`); copy to `mods/`; and the
  plugin prompt on a pack install. Feedback is covered only in its GitHub-issue fallback:
  the Worker-mode form (Turnstile and the feedback endpoint) is unit-tested
  only, because the e2e build has no Turnstile site key.
- Vendor data: tests that read `vendor/` (the hundekraft factory presets and
  firmware name table) skip when it is absent, as in CI. `CO_OP_PRESETS_ZIP`
  and `CO_OP_FIRMWARE_TABLE` point them at other copies.
- USB MIDI: the protocol core, check and installs are tested against a
  fake OP-1 in `web/test/fake-op1.ts`, written from the research notes
  (HELLO with and without capabilities, LIST paging, silence, lost
  replies, realtime bytes inside SysEx, split replies, the T1/T2 prompt
  that ignores keys after T1, gaps and path refusals). Neither has been
  checked against a real OP-1 (section 12).
- Plugin files: the validator is tested with hand-built byte arrays for
  every rule, including a CRC computed in the test; the seed files pass.
- Fixtures: a small set of real stock patches contributed under a permissive
  license, checked into `web/test/fixtures`.

## 11. Phasing

1. Scaffold the workspace, CI and Pages deploy with a placeholder page.
2. Codec and stock profile with fixtures and tests.
3. Device stores and the device library UI.
4. Packs validator, index generation, catalogue UI, contributor guide.
5. Worker with feedback endpoint, feedback UI.
6. op1.fun client, browse and pull, Worker upload proxy, upload UI.
7. hundekraft profile from `vendor/hundekraft`: sound traits, strict
   stock compatibility, content sniffing with a switch suggestion, and
   write guards.
8. opus profile from `vendor/opus`: mods-folder detection, plugin needs
   as warnings, the installed plugins list and `mods/` in backups.
9. Remembered firmware: the profile chosen by hand is stored and wins on
   the next open unless folder detection proves another.
10. Read-only USB MIDI check of a hundekraft 3.x OP-1: kennel framing and
    session, the inventory, the check panel and "Use OP-1 (hundekraft)".
11. Plugin library: format and validation in `shared`, build, CI and dev
    server, the ten seed plugins, contributor docs and the manual prompt,
    and the Plugins tab with the safe manual renderer.
12. Plugin installs and requirements: install over USB for hundekraft,
    copy to `mods/` for opus, `needs plugin` findings and the plugin prompt
    on pack, op1.fun and restore installs.

Each phase is independently shippable and ends with a deploy.

## 12. Open items for the implementation plan

- Still open: confirm the exact stock OP-1 drive layout and patch limits
  per folder against a mounted device. The stock profile was built from
  the owner's real device files and the firmware notes, but no mounted
  drive has been walked end to end, so per-folder patch limits are not
  enforced.
- Still open: the USB check and plugin installs have not been tried on a
  real OP-1. The assumptions this rests on are listed under "As built" in
  `docs/research/2026-10-29-hundekraft-usb-and-plugins.md`.
- Decide the pack size limit for `packs/` once the first real packs are in,
  and whether Git LFS is needed.
- Settled: op1.fun's API docs put files, previews and pack zips in the
  `op1fun` S3 bucket, which sends no CORS headers (its preflight is a 403),
  so installs need the Worker file proxy. Still unverified against the live
  API: the exact list, pack and create response shapes, pagination, and
  where the profile page shows the API token.
