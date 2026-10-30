# co-op

A community patch librarian and patch-sharing platform for the Teenage
Engineering OP series: OP-1 first, then OP-1 Field and OP-XY.

Live app: https://coldsoldr.github.io/co-op/

## What exists today

All eight phases are complete. The app is a working librarian for an OP-1 in
disk mode (shift + COM, then DISK):

- **Library tab.** In Chrome or Edge, "Connect OP-1" opens the drive for
  reading and writing (the folder is remembered, so reconnecting is one
  click). Every browser can also drop or pick the drive folder to open a
  read-only copy. Patches are listed by folder with their name, kind, size
  and a compatibility badge for the detected firmware profile (which can be
  overridden); unreadable files stay visible with the error.
- **Operations.** Play, rename the file, set the patch name, move or copy
  to another folder (or a new one), and delete with confirmation. Copies,
  moves and backups use the original bytes; only setting the name rewrites
  the metadata. Writes are atomic per file and interrupted writes are put
  back the next time the drive is opened. On a read-only copy, changes are
  collected and downloaded as `op1-changes-YYYY-MM-DD.zip` with a
  `CHANGES.txt` saying what to delete, copy and rename by hand.
- **Backup and restore.** Download the whole patch tree as a zip with a
  checksummed manifest; restore picks a backup, shows which files are new
  or changed, and writes the ones you select (never deleting anything).
- **Packs tab.** A catalogue of community patch packs that live in this
  repository's `packs/` folder: search and filter by kind, firmware,
  licence and tag, preview patches in the browser, and install a whole pack
  or selected patches into a folder on the OP-1 (by default
  `synth/<pack>` and `drum/<pack>`), or add them to the changes to
  download on a read-only copy. Every download is checked against its
  SHA-256. Anyone can add a pack by pull request; see
  [docs/contributing-packs.md](docs/contributing-packs.md).
- **op1.fun tab.** Enter your op1.fun email and API token (kept for the
  page only unless you tick "Remember on this device"), load any user's
  patches and packs, filter them, preview the MP3, and install a patch or
  a whole pack into `synth/op1fun` and `drum/op1fun` (or the changes to
  download). Each file is checked as an OP-1 patch before it is written.
  Library rows get an Upload action that sends the patch file unchanged to
  your op1.fun account. Both go through the co-op Worker (op1.fun's file
  host does not let the browser download directly), so installing and
  uploading stay disabled until the Worker is deployed.
- **Firmware profiles.** Stock OP-1, hundekraft and opus. An opus drive is
  recognised by its `mods/` folder of `.opmod` plugins, which the Library
  lists (in load order, flagging files that are not opus plugins) and
  backups include. Hundekraft drives look the same as stock ones in disk
  mode, so co-op detects stock and, once the patches are read, offers to
  switch when they use hundekraft or opus sounds (it never switches by
  itself). Each badge has a details popover with the patch's engine,
  effect, LFO and plugin. Installing or restoring a patch the active
  profile marks unsupported asks first, naming each patch and the reason;
  on stock the warning is stronger, because sounds from custom firmware can
  stop a stock OP-1 at boot. hundekraft and opus only warn when a sound
  needs a plugin.
- **USB check.** "Check over USB" asks a hundekraft 3.x OP-1 (plugged in
  normally, not in disk mode) which firmware it runs and which plugins it
  loaded, read only. The profile you choose by hand, or from the check, is
  remembered for the next drive; a drive whose `mods/` folder proves opus
  still opens as opus.
- **Inspector tab.** Drop a single `.aif` to see how co-op reads it and
  how the open drive's profile (or one you pick) judges it.
- **Feedback.** The Feedback button in the header sends a bug report, idea,
  pack request or anything else, with a short list of attached context
  (app version, profile, store type, browser) shown before sending. Once the
  Cloudflare Worker in `worker/` is deployed, it goes through a Turnstile
  check and becomes a labelled GitHub issue without needing an account;
  until then the button opens a prefilled GitHub issue instead. The owner's
  setup is in [docs/deploying-the-worker.md](docs/deploying-the-worker.md).

Under the hood: a pure TypeScript AIFF/AIFC codec that round-trips OP-1
files byte for byte, a device-profile layer for stock, hundekraft and opus OP-1 firmware, a
`DeviceStore` abstraction with File System Access, dropped-folder,
staged and in-memory implementations, a Cloudflare Worker tested by calling
its handler directly, unit tests with vitest and Playwright smoke tests in
Chromium.

Next: the OP-1 Field and OP-XY, which need their own profiles. The full design is in
`docs/superpowers/specs/2026-10-28-op-patch-librarian-design.md`; the
implementation plans are in `docs/superpowers/plans/`.

## Plugin library

The `plugins/` folder holds community plugins for OP-1 custom firmware:
synth engines, effects and sequencers, one folder each with the plugin file,
a `plugin.json`, a manual and a screenshot. It starts with ten hundekraft
3.0.3 plugins by coldsoldr (rings, bitcrusher, drive, tape drive,
wavefolder, freq shift, ensemble, tape echo, marbles and grids; source at
[coldsoldr/te-op1-cfw](https://github.com/coldsoldr/te-op1-cfw)). The build
checks every plugin and publishes them with an index, and the **Plugins**
tab lists them: search and filter, read each manual, download the file,
install a hundekraft plugin over USB MIDI (the OP-1 asks for T1 and
restarts), or copy an opus plugin into `mods/` on an open opus drive. When a
pack, op1.fun or restore install has patches that need a library plugin not
known to be on the OP-1, co-op names it and offers to install it first.

A plugin runs as firmware code, so maintainers merge one only from an author
they trust or with source they have looked at. To add one, follow
[docs/contributing-plugins.md](docs/contributing-plugins.md); the manual
structure, as a prompt for your coding agent, is in
[docs/plugin-prompt.md](docs/plugin-prompt.md).

## Versions

The site's version is `version` in `web/package.json`, shown at the bottom left of every
page and attached to feedback. Raise it when a change reaches users: the minor number for
a new feature, the patch number for a fix. Tag the release as `v<version>`.

## Running it

Requires Node 22 or newer and pnpm 11 (pinned through `packageManager`, so
`corepack enable` is enough).

```bash
pnpm install
pnpm --filter @co-op/web dev      # local dev server, open the printed /co-op/ URL
pnpm lint && pnpm typecheck && pnpm test && pnpm build
pnpm --filter @co-op/web exec playwright install chromium   # once
pnpm --filter @co-op/web e2e      # Playwright smoke test against the production build
pnpm --filter @co-op/web packs:validate   # check every pack in packs/
pnpm --filter @co-op/web plugins:validate # check every plugin in plugins/
pnpm --filter @co-op/worker exec wrangler deploy --dry-run --outdir dist   # bundle the Worker without deploying
```

Tests that read the gitignored `vendor/` clones skip when they are absent;
`CO_OP_PRESETS_ZIP` and `CO_OP_FIRMWARE_TABLE` point them at other copies
(see [vendor/README.md](vendor/README.md)).

CI runs the same commands, and the Playwright test in its own job, on every
push and pull request. Pushes to
`main` that pass are deployed to GitHub Pages automatically.

## Repository layout

```
web/      Vite + React + TypeScript app; web/scripts holds the packs CLI
worker/   Cloudflare Worker: POST /feedback files GitHub issues; op1.fun upload and file proxy
shared/   pack and feedback schemas and validation, shared by the app, CLI and Worker
packs/    community patch packs, one folder each
plugins/  community firmware plugins, one folder each
docs/     design spec, implementation plans, pack, plugin and Worker guides
vendor/   gitignored clones of third-party firmware repos, see vendor/README.md
```

`packs/index.json` is never committed: the build
validates the packs and writes the index into `web/dist/packs/`, and the dev
server builds it on the fly.

## Test fixtures

`web/test/fixtures/*.aif` are synthetic files generated by
`pnpm --filter @co-op/web fixtures`. A test fails if `meta.ts` changes
without regenerating them. `web/test/fixtures/real/` holds files copied
from an OP-1 in disk mode and is used for byte-for-byte round-trip tests;
see the README there for provenance.

## Contributing

Conventional commit subjects (`feat(codec): ...`, `fix(profiles): ...`,
`docs: ...`) keep the history readable. Keep `pnpm lint && pnpm typecheck && pnpm test`
green. To share patches, follow the pack guide in
[docs/contributing-packs.md](docs/contributing-packs.md); to share a
plugin, the plugin guide in
[docs/contributing-plugins.md](docs/contributing-plugins.md).

## Licence

MIT, see `LICENSE`. Packs and plugins carry their own licences, given in
each `pack.json` and `plugin.json`.
