# Handover: integrating the co-op platform with the coop firmware (2026-11-08)

For whoever makes `../co-op` (the librarian and patch platform, https://coldsoldr.github.io/co-op/) and this
firmware behave as one product. Written from a survey of both repositories at co-op `dev` 1ab96e9 (web 0.9.0) and
coop-fw e8609e9 (package p6j). Confidence marks as everywhere in `docs/`: [C] confirmed in code or on the device,
[I] inferred. Companion references: `docs/sysex.md`, `docs/plugins.md`, `docs/test-runner.md`, `docs/ui-features.md`,
`docs/screens.md`, `guide/plugin-guidelines.md`, and in co-op `docs/research/2026-10-29-hundekraft-usb-and-plugins.md`
and `docs/superpowers/specs/2026-10-28-op-patch-librarian-design.md`.

## 0. Where the two stand today

**co-op does not know coop-fw exists** [C: no `coop`, `.coop`, `slot2`, `lfo2`, `coop_stats`, `coop_run` anywhere in
its sources]. It targets three firmware profiles, stock R246, hundekraft 3.0.3 and opus:
- its USB path speaks **kennel's** SysEx command numbers 1..11 (HELLO, BEGIN, DATA, END, COMMIT, ABORT, LIST, OFF_GET,
  OFF_SET, DELETE, RESTART) against a hundekraft 3.x unit, with a staged install and an automatic restart;
- it validates and installs only `.knl` (magic `KNPL`), `.op1p` and `.opmod` plugins at `plugins/<id>.knl|op1p`;
- its sound traits and compatibility rules read the stock keys, the op1r / hundekraft selector in `knobs[4]` /
  `fx_params[4]` and the `opus` member; the coop member round-trips untouched but means nothing to it;
- live audition goes through the hundekraft `audition` `.knl` plugin over MIDI CC / polyphonic pressure.

**coop-fw's host contract is small and different**: the same SysEx header and 7-bit framing (both come from kennel's
development update), but commands 0x20..0x27 of its own, direct file writes without a restart, a `.coop` plugin
format (kennel's layout with the magic swapped), a `coop` member in the sound JSON, and a handful of device-side
state files. Nothing on either side has been written against the other. Everything below is the material to close
that: section 1 is the firmware's contract as it is in code (not as the SDK comments say), section 2 is what co-op
is, section 3 is the work in order, section 4 the acceptance list.

Three firmware defects found in the survey that an integrator will hit first; they are listed in 3.1 as the first
tasks:
1. `HELLO` answers a stale build tag (`"p5g"` compiled into `coop_sysex.cc` while the image is `p6j`), so a host
   cannot identify the build over USB. [C `core/sdram/coop_sysex.cc:41`]
2. The settings menu stores disabled plugin file names in 15 characters, so a file such as `stereo_delay.coop`
   (17) can never be disabled from the menu. [C `core/sdram/coop_settings.cc:44-45`, `:161`]
3. The SDK header's description of a plugin's `params` (Q15 0..32767) is not what the host passes (section 1.5).

## 1. The firmware's host contract (reference) [C unless marked]

### 1.1 Transport and framing

- The OP-1 is a USB-MIDI device while it runs (not in disk mode). co-op already opens the port whose name matches
  `/op-?1/i`, not `/field/i`, per action, and closes it after (Windows lets one program hold a port).
- Message: `F0 00 20 76 7D <cmd> <seq_lo> <seq_hi> <7-bit packed payload> F7`. `seq` is 14 bits, echoed back. A
  reply has `cmd | 0x40` and the same `seq`; its payload is a status byte, then data.
- 7-bit packing: groups of up to 7 bytes, each preceded by one byte whose bit k is bit 7 of data byte k; identical
  to co-op's `web/src/core/usbmidi/framing.ts` pack7 / unpack7 (`docs/sysex.md` calls it `sysex.js`; that file never
  existed under that name).
- Limits: the host keeps every message at or under 96 bytes (the OP-1's USB-MIDI input stops on longer SysEx; the
  device accepts 128). Replies carry at most 72 raw bytes. One message in flight; the firmware's own page uses a 2 s
  timeout; throughput about 1.5 KB/s. Main-loop stalls of 45-170 ms under load are documented, so a host must
  tolerate slow replies rather than fail fast.
- Status codes: 0 ok, 1 format, 2 state, 3 path, 4 size, 5 offset, 6 crc, 7 io. An unknown command gets 1.
- Threading: receive in the timer context (the stock `midi_rx_parse` call retargeted at `coop_sysex_rx_poll`, a
  4-entry queue, overflow counted in `g_sysex_drop`); service on the main loop from the tape I/O step; replies
  through the stock `usbmidi_tx_enqueue`. The image must be built `--tape-stereo` (every package is) or the service
  is not wired.

### 1.2 Commands

| cmd | name | request payload | reply after the status byte |
|---|---|---|---|
| 0x21 | HELLO | none | `"coop"`, u16 LE protocol (1), the ASCII build tag (stale: see 3.1) |
| 0x20 | FILE_GET | u32 LE offset, path bytes (no NUL) | u32 LE total size, up to 64 bytes from the offset; status 3 cannot open, 7 io; offset at or past the end: status 0, no data. **Any path**, no whitelist |
| 0x22 | PUT_BEGIN | path | creates or truncates the file; `mkdir /yaffs2/plugins` first; a new BEGIN closes a previous PUT |
| 0x23 | PUT_DATA | u32 LE offset, data (host uses 64 bytes, 72 fit) | u32 LE next offset; a wrong offset is status 5 and the file stays open; a short write is 7 and closes it; no file open is 2 |
| 0x24 | PUT_END | u32 LE total, u32 LE crc32 (zlib) | status 4 size, 6 crc, 7 close failed |
| 0x25 | DELETE | path | 0, or 3 if unlink failed |
| 0x26 | SLOT2 | op byte: 0 get, 1 set, 2 plugin list | see 1.3 |
| 0x27 | RUN | op byte: 0 status, 1 start + path, 2 abort | see 1.3 |

Path whitelist for PUT, DELETE and RUN: the path must start with `/yaffs2/plugins/` or `/yaffs2/user/`, one level
only (no further `/`, no `..`). `/yaffs2/user/audio/...` (the presets) cannot be written over USB; FILE_GET can
read it. Paths fit in 72-76 bytes.

### 1.3 SLOT2 and RUN

SLOT2 op 0 (get) replies `00` then 10 bytes per slot, instrument first, then master: `kind` (0 none, 1 stock type,
2 plugin), `id`, 4 x u16 LE knob (Q15). Op 1 (set) takes `pos` (0 instrument, 1 master), `kind`, `id`, 4 x u16 knob;
status 2 unknown effect or no memory, 7 save failed; on success the slot file is saved and the get reply returned.
Op 2 replies `00` then `n=name;` per loaded plugin (truncated at 71 characters by the reply cap). Stock type ids:
delay 0, grid 1, punch 2, phone 3, spring 4, filter 5, nitro 6, cwo 7; a plugin id is its number 32..63. Note the
u9 UI: slot 2 has no "none" any more from the device's own menu (the first drop copies FX 1), but the protocol still
accepts kind 0. Switching a slot on / off (the `off` flag) is not reachable over SysEx; only the menu's T3 does it.

RUN op 1 starts a CRUN file (written first with PUT under `/yaffs2/user/`). Every reply is 13 bytes: status (start
refused: 2), state (0 idle, 1 armed, 2 running, 3 done, 4 stopped, 5 error), u32 LE block, u16 LE event index, u16
LE event count, sections (u8), error (1 open, 2 format, 3 too many events, 4 bad event, 5 busy), run-file-written.
An image without the runner answers status 1. The host polls every 400 ms; the report is `/yaffs2/user/coop_run.txt`
(1.4). CRUN format and the script compiler: `docs/test-runner.md`, `tools/run_script.py`.

### 1.4 Files on the device (none visible in disk mode)

| file | format | written | read |
|---|---|---|---|
| `/yaffs2/user/coop_slot2.bin` | 43 B: `"CS2"` ver 3; [4..13] instrument slot, [14..23] master slot, each `kind, id, 4 x u16 LE knob`; [24],[25] the slots' off flags; [26] LFO 2 type (0 tremolo, 1 value, 2 random, 7 off) and [27..42] four u32 LE word pairs `p0|p1<<16` .. `p6|p7<<16` (the image-wide LFO 2 words, the fallback when a sound has none) | main loop on a menu change or a SLOT2 set | boot, after the plugin scan; versions 1..3 accepted |
| `/yaffs2/user/coop_settings.bin` | `"CST"` ver 1; [4] spread 0..100 %; [5..8] four choice bytes (forced 0 on load); [9] count n; then n x 16 B NUL-padded disabled plugin file names | main loop when the menu closes dirty | boot, before the plugin scan (read only if 10+ bytes) |
| `/yaffs2/user/coop_stats.txt` | text, line 1 `coop_stats 3 build <tag>`, then tagged lines (1.7) | main loop: after 64 passes, then every 5 s while not recording, and after every run; O_TRUNC each time; 16 KiB buffer | — (a host reads it with FILE_GET; an 8 KiB copy also sits in the last 8 KiB of disk-mode `tape/track_4.aif`) |
| `/yaffs2/user/coop_run.txt` | text: `coop_run 1 build <tag> script <name>`, `run ...`, `switches ...`, then per section `section`, `cost`, `engines`, `gaps` lines | main loop when a run ends | — |
| `/yaffs2/user/coop_test.crun` | CRUN (the host's upload name) | host PUT | RUN start |
| `/yaffs2/user/coop_markers.bin` | `"CMK"` ver 1; 4 x i32 LE sample positions (blue, green, white, orange), -1 none, 0..0xF23FA0 | main loop; cleared by a tape erase | after the tape mounts |
| `/yaffs2/user/coop_tape.txt` | exactly `coop_tape 1 stereo4\n` | after an image rebuild | each mount; a mismatch rebuilds `tape_c.raw` (recordings lost) |
| `/yaffs2/user/tape_c.raw` | FAT16 image 0x18000000 B, four stereo AIFC tracks (`sowt`, 2 ch, 0xF28000 frames), no album | coop | the stock tape layer |
| `/yaffs2/plugins/*.coop` | the plugin format (1.5) | host PUT | boot scan: the first 8 files, names under 40 characters |
| user db `op1.db`, table `coopplugins`, row 0 | `data` = `f:<type name>=<n>,` per plugin, `-f:` for a file gone (its number kept); numbers 32..63 | boot when dirty | boot |
| factory db `content/op1_factory.db`, table `fx_types` | one row per plugin: `type` = its name, `default_params = "[v0,v1,v2,v3,<0x5300+n>,0,0,0]"` | every boot | the stock FX list |
| user db `synth_user` / `drum_user`, `patch` JSON | the preset row | overwrite save; rename (synth only) | — |

Device-side coop state is therefore in five small files plus two db rows, none of which co-op's disk-mode backup
sees (it covers `synth/`, `drum/` and opus `mods/`).

### 1.5 The sound JSON `coop` member (per preset, in the AIFF `APPL` chunk and the db row)

`"coop": {"bend": -12..12 (default 12), "bend_t": 0..128 (0 off, 1..127, 128 max; default 0), "pan": [24 x -50..50]
(drum kits; default 0), "lfo2": {"t": 0 tremolo | 1 value | 2 random | other = 7 none (default 7), "on": 0|1,
"p": [8 x int16]}}`. The writer emits every key for synth and drum sounds alike; values are read as jsoncpp int or
uint only. There is **no `spread` key** (spread is device-wide in the settings file) and **FX 2 is not per sound**
(it lives in `coop_slot2.bin`; `docs/ui-features.md` §7's "persists with the sound" is not true in code). The member
survives stock saves because the stock JSON rebuild's assignment is retargeted at `coop_keep`, which copies the old
object across; writes go through a stock setter so the stock's background writer saves the patch. It travels with
slots, user presets and lift / drop. On a stock R246 the member is dropped silently by the same rebuild [I: by the
mechanism; not tested].

### 1.6 Plugins: file format, ABI as implemented, listing

- File: kennel 3.1's format with magic `COOP`, format 1, a 144-byte header (`docs/plugins.md`, `plugin_load.cc`),
  optional leading screen SVG, 12-byte relocations (ABS32, HI16, LO16, PCREL24), extension tags (1 claims, 2 author,
  3 version, 4 bigbuf, 5 cost). Output must be `<id>.coop`, id `[a-z0-9_-]{1,15}` = the folder name, saved type name
  = id with `_` as space. Manifest `plugins/<id>/plugin.json` is `tools/plugin/pack.py`'s document, **not** co-op's
  `plugin.json` format 1 (section 2); `description` is not packed.
- ABI v1, effects only: `coop_fx_t { magic "COFX", api 1, flags 0, init(slot), render(slot, l, r, params, n) }`;
  two int32 planar buffers (the int16 bus widened 16 bits), n = 64, in place. Slots in code: 0 synth, 1 drum, 2
  master, 3 instrument slot 2, 4 master slot 2 (the header says 0..2). `init` runs on the audio thread for slots 0-2
  and on the main loop for 3-4. **`params` is not Q15**: at slots 0-2 the plugin gets the stock slot's raw knob words
  clamped into the filter type's ranges; at slots 3-4 the Q15 knobs mapped into those ranges; `params[4]` is
  `0x5300 + n` at every slot. Flags must be 0 (`pack.py`'s `"stereo": true` sets flag 4 and the loader refuses it);
  `COOP_FX_STEREO` in `docs/fx-abi.md` does not exist. The export table is empty, so any import is refused.
- Memory: 1 MiB staging per file; each plugin relocated 5 times into heap 0 (never freed); no numeric budget, a
  plugin that does not fit fails "plugin memory full". The L1 lease is proposed only.
- Listing and names: each loaded plugin gets an `fx_types` row; the stock FX list name compare is replaced so any
  non-stock name resolves to the filter type; a sound naming a missing plugin loads as the filter. The device-side
  enable / disable is the settings menu's PLUGINS rows, applied at the next boot (and broken for long names, 3.1).
- Install: PUT the file, then **power-cycle**; the loader scans only at boot. No restart command exists.

### 1.7 Stats, counters, versions

- `coop_stats.txt`: line 1 `coop_stats 3 build <tag>`; tags in order `cb`, `stages`, `cost`, `engines`, `icache_l0`,
  `icache_l1`, `pfsweep_code`, `pfsweep_data`, `l1`, `switches`, `cplb`, `dcplb`, `icplb`, `buffers`, `slow`, `gaps`,
  `slow_worst`, `slow_recent`, `plugins`, `tape`, `io`, `edits`, `stats`, `top`, `win`. Mostly `key value` pairs;
  `cost` and `engines` are `name=avg/max/n[/p0/p1]`, `gaps` is `aa>bb=per-block/avg/max/n`, `plugins` may end in
  free text. The counters (p0 / p1) in mode 0 are code fetch stall cycles and data stall cycles, proven by the boot
  probe on the device (`docs/performance.md` "p6h"); the `cost` header carries `pf_mode`, `pf_live`, `pf_layout`.
- Version signals a host can read: `HELLO` protocol 1 (and the tag, once fixed), `coop_stats 3`, `coop_run 1`,
  plugin API 1 / format 1, `CS2` 3, `CST` 1, `CMK` 1, `CRUN` 1, `coop_tape 1 stereo4`. The region-A `OP1X` header
  (magic, version 1, entry points) is only in the package, not reachable over USB.

## 2. What co-op is (reference) [C in its code]

- A pnpm monorepo: `web/` (Vite 8, React 19, TypeScript; `web/src/core/` pure logic: codec, profiles, usbmidi,
  library, store, plugins, op1fun; `web/src/ui/` the views), `worker/` (Cloudflare: feedback to GitHub issues, the
  op1.fun proxy; never talks to the device), `shared/` (pack, plugin and feedback schemas, CRC-32), `plugins/<id>/`
  (11 hundekraft plugins: `.knl`, `plugin.json`, `manual.md`, `screenshot.png`), `packs/`. Run: `pnpm install`,
  `pnpm --filter @co-op/web dev`; checks `pnpm lint && pnpm typecheck && pnpm test && pnpm build`, Playwright e2e,
  `plugins:validate`, `packs:validate`. Published at `/co-op/` (main) and `/co-op/dev/`.
- Device access: disk mode through the File System Access API (`synth/`, `drum/`, `.aif`, one category level; opus
  `mods/*.opmod`; atomic writes via `.co-op-tmp-*`; a read-only copy yields `op1-changes-<date>.zip`); Web MIDI
  SysEx (`webmidi.ts`, `framing.ts`, `session.ts`, `install.ts`, `check.ts`, `inventory.ts`) speaking kennel 1..11
  with 3 s / 5 tries, 64-byte pages, 72-byte DATA; the audition channel (`audition.ts`: CC 118 / 119 on channel 16,
  data as polyphonic pressure on channel 15, patch frame format 4 of at most 800 bytes plus audio).
- Schemas: `plugin.json` format 1 (`shared/src/plugins.ts`: `format, id, name, firmware ∈ {op1/hundekraft,
  op1/opus}, kind ∈ {synth, fx, feature}, author, version, description, license (allow-list), file (<id>.knl|op1p or
  *.opmod), screenshot, manual`, optional `source, requires, credits, video, tags, savedName`; unknown fields
  rejected); the plugin index `plugins/index.json` with size and SHA-256; `pack.json` format 1 with `firmware[]`
  of known profile ids; `co-op-backup.json` format 1.
- Profiles (`profiles/types.ts`, registry `[op1Stock, op1Hundekraft, op1Opus]`): detection, sniffing, supported
  names, compatibility and plugin compatibility. The stock FX list is `cwo, delay, grid, nitro, phone, punch, spring`
  (no `filter`; coop's SLOT2 id 5 is the filter). Sound traits read the op1r selector `0x5300..0x53FF` in `knobs[4]`
  / `fx_params[4]` and the `opus` member; coop's plugins reuse the same `0x5300 + n` selector in `fx_params[4]` (1.4,
  1.6), which co-op will read as "an op1r / hundekraft plugin" unless taught otherwise.
- Firmware requirements are hard-coded as "hundekraft 3.0.3"; the research doc says the USB check and install
  "have not been tried against a real OP-1 by co-op yet"; only the audition proof of concept was device-confirmed.

## 3. The integration, in order

### 3.1 Firmware first (coop-fw; small, each a package the owner flashes)

1. **HELLO tag**: derive `BUILD_TAG` from `STATS_BUILD` (one define in a shared header) so `HELLO`, the stats and
   the run file agree. Add a **capabilities byte** after the tag: bit 0 file read, bit 1 file write, bit 2 SLOT2,
   bit 3 RUN, bit 4 inventory (below), so a host feature-detects instead of version-matching.
2. **Settings name width**: `NAME_LEN` 16 → 40 (the loader's own limit), `CST` version 2 (read 1 and 2). Until
   then the menu cannot disable `stereo_delay.coop`.
3. **Inventory command** `0x28 INVENTORY`: payload u32 offset; reply u32 total + up to 64 bytes of a text co-op can
   page exactly as it pages kennel's LIST. Keep kennel's shape so co-op's `inventory.ts` parser can be reused with
   one new magic: `coop-inventory 1<TAB>build <tag><TAB>protocol 1<TAB>presets - cap -`, then one row per plugin
   file the boot scan saw: `kind name source path version author size crc state reason` with `source = coop`,
   `state ∈ loaded | failed | disabled`, `reason` the loader's text. This is the only way a host learns a plugin
   failed to load and why; SLOT2 op 2 lists loaded ones only and truncates at 71 characters.
4. **Enable list over USB** `0x29 PLUGIN_SET`: payload `enable` byte + file name; writes the settings file's
   disabled list from the main loop (never the key thread) and replies the inventory row; "applied at next boot" is
   in the reply status (use 9 "wait", as kennel does, so co-op's existing "restart needed" path fits).
5. **Slot on / off over USB**: extend SLOT2 set with an `off` byte after the knobs (the get reply gains the two off
   bytes); the UI's `coop_slot2_bypass` path already exists.
6. **Settings read / write** `0x2A SETTINGS`: op 0 get (spread %, the four choices), op 1 set spread live (the
   same path as the menu: `g_coop_spread`, dirty, saved by the main loop). Everything else in the menu is "stock" or
   "not yet" today.
7. **A restart command is not possible** from coop code safely today (no stock reset path is mapped); document
   the power cycle in the reply (status 9) rather than pretend.
8. Make `SLOT2` op 2 / inventory / HELLO replies fit the 72-byte cap by paging (offset in, total out), never by
   truncation.
9. Fix the SDK comment and `docs/fx-abi.md` to the implemented `params` contract (1.6), or implement the SDK's
   contract (Q15 at every slot) in the host: decide once, in the plugin guidelines, before co-op ships a `.coop`
   catalogue. The proposed ABI v2 parameter store (`guide/plugin-guidelines.md` §3) is the right moment.
10. Record device passes for SLOT2 and RUN in `docs/device-tests.md` (they ran, the entries never say "pass").

### 3.2 co-op: a coop profile and transport

1. **Profile `op1/coop`** in `web/src/core/profiles/` beside stock, hundekraft, opus: layout as stock (a coop drive
   is a stock drive in disk mode), `supports` = stock names plus the loaded plugins' type names (from the
   inventory) plus `filter` for FX (SLOT2 id 5), compatibility rules: a sound with a `coop` member is "ok" on coop,
   "ok, coop extras ignored" on stock [I: dropped by the stock's JSON rebuild, no boot risk], and a sound whose
   `fx_type` names a plugin is "needs plugin <name>" unless the inventory shows it `loaded`. Sniff: the `coop`
   member (`sound-traits.ts`), and note that coop plugins also set `fx_params[4] = 0x5300 + n`, which today reads
   as op1r; disambiguate by the profile in effect.
2. **Detection over USB**: send coop `HELLO` (0x21) first, then kennel's (1); `"coop"` + protocol + tag + caps
   selects the coop profile and remembers it as the USB check does today. The check page shows the build tag, the
   protocol and the inventory table (3.1.3) in place of kennel's.
3. **Transport module**: a `coop.ts` beside `install.ts` with FILE_GET paging (64 B, one in flight, a timeout of
   3 s and 5 tries as `session.ts`, tolerant of 200 ms stalls), PUT_BEGIN / DATA (64 B) / END with the zlib CRC
   from `shared/src/crc32.ts`, DELETE, SLOT2, RUN, and the new commands. The framing is already `framing.ts`'s; only
   `HDR` command numbers and payloads differ.
4. **Plugin schema**: add `op1/coop` to `PLUGIN_FIRMWARE`; accept `file = <id>.coop`; a `checkCoop` beside
   `checkKnl` (magic `COOP`, format 1, header 144, file size and CRC, api 1, kind 2 only today, svg_offset 0,
   svg_name 0xFFFFFFFF, size at most 1 MiB, multiple of 4); `kind` maps 2 → `fx`; `requires` = "coop p6j" style
   tags until the firmware has a version number worth the name. Catalogue entries for coop plugins carry the
   coop-fw `plugin.json` fields that matter to a user (name, version, author from the extension tags, the
   `saved_name`), generated from the `.coop` header by `scripts/plugins.ts`, not hand-typed twice.
5. **Install flow for coop**: HELLO caps → inventory (refuse a name already loaded unless overwrite) → PUT_BEGIN
   `/yaffs2/plugins/<id>.coop` → PUT_DATA → PUT_END → **prompt the user to power-cycle** (no COMMIT, no automatic
   restart; `waitForOp1Restart` can wait for the port to go and return once the user does it) → inventory again
   to confirm `loaded`. Remove = DELETE + the same prompt. Enable / disable = PLUGIN_SET when it exists; until then
   tell the user to use the device menu.
6. **Reading back**: FILE_GET makes the device-side state reachable for the first time. Add to backup a "coop
   state" group: `coop_slot2.bin`, `coop_settings.bin`, `coop_markers.bin` (restore by PUT; markers only make
   sense with the same tape), and offer the presets themselves from `/yaffs2/user/audio/...` read-only (disk mode
   stays the write path for presets, the whitelist forbids writing there).
7. **Stats page**: FILE_GET `coop_stats.txt`, parse the tagged lines (1.7), show `cb`, `cost`, `engines`, `plugins`,
   `l1` and the `top` / `win` windows; the firmware's `tools/coop_sysex.html` has the parsing and the layout to
   port. Same for the **test runner** ("Run a test": compile a script with the logic of `tools/run_script.py`, PUT
   the CRUN, RUN start, poll, FILE_GET the run file, record the browser's audio input as the page does).
8. **FX 2 and LFO 2**: an editor on the coop profile's device page: SLOT2 get / set for both slots (type, four
   knobs, on / off once 3.1.5 exists), and LFO 2 per sound through the `coop` member in the patch file (disk mode
   or FILE_GET read-back). The audition frame format 4 cannot carry FX 2 / LFO 2 / pan; see 3.4.
9. **Patch-level handling**: `op1-meta.ts` keeps unknown keys, so nothing breaks; add the `coop` member to the
   badge details ("bend -8, bend time 40, LFO 2 tremolo on, pan set") and to the Inspector.

### 3.3 Shared: one spec, one set of test doubles, one compatibility table

1. A single protocol document is the source of truth for both repos: this file's section 1 plus `docs/sysex.md`,
   mirrored into co-op's `docs/research/` as `coop-fw-usb-and-files.md`, with a version number both sides bump.
2. co-op's `web/test/fake-op1.ts` and `web/e2e/op1-stub.ts` emulate kennel; add a coop stub that answers 0x20..0x2A
   from the contract, including the 72-byte reply cap, the whitelist and the one-level path rule, so the install,
   check, backup and stats paths are tested without a device.
3. A compatibility table in both READMEs: firmware package tag ↔ protocol ↔ plugin API ↔ file versions ↔ the
   co-op version that understands them; the firmware's `HELLO` caps are what the app actually keys on.
4. Release coupling: a coop-fw package that changes a wire format bumps `PROTOCOL` or the file version and this
   table; co-op refuses (with a message) a device whose protocol it does not know rather than guessing.

### 3.4 Not now, and why

- **Audition on coop.** co-op's live audition needs the hundekraft `audition.knl`, which coop cannot load (different
  magic, kennel's runtime). A coop-native audition would be a firmware command that applies a patch JSON to the
  current slot (the stock `PATCH_SET` + `op1_sound_apply` path the hundekraft plugin uses; the research doc has
  the addresses), carrying the `coop` member too. Worth doing, but after the above; it is a new firmware feature,
  not integration.
- **Writing presets over USB.** The whitelist keeps `/yaffs2/user/audio/` read-only on purpose (a half-written
  preset file is a boot risk). Disk mode remains the preset write path.
- **kennel plugins on coop.** co-op's eleven `.knl` plugins do not run on coop; the porting route is
  `guide/plugin-guidelines.md` and the `/port` skill, one plugin at a time, each becoming a `.coop` entry in the
  catalogue.

## 4. Acceptance: "perfectly integrated" means all of these pass on the device

1. Plug in a coop unit (not disk mode), "Check over USB": the app names the profile `coop`, the build tag, the
   protocol, lists every plugin file with `loaded` / `failed` + reason / `disabled`, and remembers the profile.
2. Install `stereo_delay.coop` from the Plugins tab: transfer, the power-cycle prompt, and after the restart the
   check shows it `loaded`; the FX list on the device shows it; remove it the same way.
3. Disable and enable a plugin from the app; the next boot honours it (and the device menu shows the same state).
4. Open the drive in disk mode: a coop sound shows its `coop` details in the badge; a sound naming a plugin that
   is not loaded shows "needs plugin"; installing a pack on the coop profile warns only for that.
5. Backup includes the coop state files; restore puts them back; the device boots with the same FX 2, settings and
   markers.
6. The stats page reads `coop_stats.txt` live and shows the cost and engines tables; "Run a test" runs the p5a
   script end to end from the app and shows the sections.
7. FX 2 editor: set the master's slot 2 to spring from the app, hear it, see it on the device's page; set LFO 2 on
   a sound through its file, reload it, hear it.
8. Every path above passes against the coop stub in CI, and the compatibility table names the package and app
   versions the run was made with.

## 5. Pointers

- Firmware: `core/sdram/coop_sysex.cc` (protocol), `plugin_host.cc` (slots, files, db rows, loader host),
  `plugin_load.cc` (format), `coop_sound.cc` (the member), `coop_settings.cc`, `coop_stats.cc`, `coop_runner.cc`,
  `coop_tape_ui.cc` (markers), `tape_io.cc` (tape files, the main-loop step), `tools/coop_sysex.html` (a complete
  host in one file), `tools/run_script.py`, `tools/plugin/pack.py`, `sdk/coop_plugin.h`.
- co-op: `web/src/core/usbmidi/*` (framing, session, install, check, inventory, audition), `web/src/core/profiles/*`,
  `web/src/core/codec/op1-meta.ts` and `sound-traits.ts`, `shared/src/plugins.ts`, `web/src/ui/UsbCheck.tsx`,
  `PluginInstall.tsx`, `Plugins.tsx`, `web/test/fake-op1.ts`, `web/e2e/op1-stub.ts`, `docs/research/*.md`.
