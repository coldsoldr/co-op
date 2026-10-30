# Contributing a plugin

co-op's **Plugins** tab lists community plugins for OP-1 custom firmware:
synth engines, effects and features (sequencers and tools) that live in this
repository, in the [`plugins/`](../plugins) folder. Anyone can read their
manuals in the browser, download them and install them. You add a plugin by
opening a pull request; a check runs on the pull request to make sure the
plugin is valid, and a maintainer decides whether to merge it.

## A plugin runs as firmware code

Read this before anything else. A plugin is not a sound or a setting: it is
machine code that the OP-1 runs inside its firmware, with full access to the
device. A broken plugin can stop the OP-1 starting; a malicious one could do
anything the firmware can.

So the bar for plugins is higher than for packs:

- Maintainers merge a plugin only from an author they trust, or when they
  have looked at its source.
- A link to the source (`source` in `plugin.json`) is strongly encouraged.
  A plugin without one is unlikely to be merged unless the maintainers know
  you.
- Say in the pull request how you built the file and from which commit, so
  someone else can build it again and compare.

The validator checks that a file is well formed. It cannot tell whether the
code inside is safe.

## What a plugin folder holds

One folder per plugin, named after its id, with exactly four files:

```
plugins/<id>/plugin.json      describes the plugin
plugins/<id>/<id>.knl         the plugin file (or <id>.op1p, or <name>.opmod for opus)
plugins/<id>/manual.md        the manual, shown in the Plugins tab
plugins/<id>/screenshot.png   a picture of its screen
```

- Nothing else may be in the folder, and every file must be spelled exactly
  as `plugin.json` names it, including upper and lower case.
- Symbolic links are not allowed; add the real files.
- `.DS_Store`, `Thumbs.db` and `desktop.ini` are ignored and never
  published.

The [`bitcrusher`](../plugins/bitcrusher) folder is a complete example.

### The id

For hundekraft the id is the name the OP-1 stores in a sound, with spaces
as `_`: a plugin whose saved name is "tape echo" has the id `tape_echo`, the
folder `plugins/tape_echo/` and the file `tape_echo.knl`. It is 1 to 15
lower-case letters, digits, `_` or `-`. The validator reads the saved name
from the file and refuses a mismatch.

For opus the id is 1 to 40 lower-case letters, digits, `_`, `.` or `-`, and
`plugin.json` gives the `opus.<id>` name that sounds made with it store as
`savedName`.

## plugin.json

```json
{
  "format": 1,
  "id": "bitcrusher",
  "name": "bitcrusher",
  "firmware": "op1/hundekraft",
  "kind": "fx",
  "author": "coldsoldr",
  "version": "0.1",
  "description": "Sample-rate and bit-depth reduction with a jittery clock.",
  "license": "GPL-3.0-or-later",
  "file": "bitcrusher.knl",
  "screenshot": "screenshot.png",
  "manual": "manual.md",
  "source": "https://github.com/coldsoldr/te-op1-cfw",
  "requires": "hundekraft 3.0.3",
  "tags": ["effect", "lo-fi"]
}
```

| Field | Rules |
| --- | --- |
| `format` | Always `1`. |
| `id` | Equals the folder name; see above. |
| `name` | 1 to 60 characters, as the OP-1 shows it. |
| `firmware` | `op1/hundekraft` or `op1/opus`. |
| `kind` | `synth`, `fx` or `feature`. For hundekraft it must match the kind in the file. An `.op1p` is a synth or an fx. |
| `author` | 1 to 60 characters. |
| `version` | 1 to 20 characters. If the file carries a version, use the same one (a mismatch is a warning). |
| `description` | 1 to 500 characters. |
| `license` | One of `GPL-3.0-or-later`, `GPL-3.0-only`, `GPL-2.0-or-later`, `MIT`, `BSD-3-Clause`, `Apache-2.0`, `MPL-2.0`, or `LicenseRef-Freeware` for a closed plugin that is free to use. A plugin built on an SDK must follow that SDK's licence (hundekraft's is GPL-3.0-or-later). |
| `file` | hundekraft: `<id>.knl` or `<id>.op1p`. opus: a name ending in `.opmod`. |
| `screenshot` | A `.png` file name in the folder, usually `screenshot.png`. |
| `manual` | Always `manual.md`. |
| `source` | Optional but strongly encouraged: an `https://` link to the source, at most 200 characters. |
| `requires` | Optional, up to 60 characters, such as `hundekraft 3.0.3`. |
| `credits` | Optional, up to 200 characters: whose code or algorithms it ports, with their licence. |
| `tags` | Optional, up to 8 tags of 1 to 20 lower-case letters, digits or hyphens. |
| `savedName` | opus only: `opus.<id>`. |

## The checks

`pnpm --filter @co-op/web plugins:validate` checks every plugin folder. Beyond
the fields above it checks:

- **`.knl` (hundekraft kennel, format 1).** The `KNPL` header, format 1 and
  plugin API 1, the size in the header against the real size, the CRC-32,
  the kind, the saved name (which must give the id), a size that is a
  multiple of 4 and at most 512 KB, the screen SVG fields, and the extension
  table. The author and version in the table, if present, are compared with
  `plugin.json` (a difference is a warning, not an error).
- **`.op1p` (op1r).** The trailer, the binary's offset, length and CRC-32,
  its header (format 1, API 1, kind, saved name) and the size limits.
- **The boot hazard.** The OP-1's SVG parser hangs at boot on a number with
  more than four decimals in a plugin's screen SVG. A file with one is
  refused; round the numbers in your SVG.
- **`.opmod` (opus).** The `OPMD` magic and a 2 MB limit; nothing more is
  known about the format.
- **screenshot.png.** A PNG of 64 to 2048 pixels each way, at most 1 MB.
  Use 960 by 480 (the screen at 3x), like the other plugins.
- **manual.md.** UTF-8 text, not empty, at most 64 KB.

It prints every problem grouped by plugin and ends with
`validated N plugins` when all is well. The same check runs on the pull
request. To see your plugin in the app, run `pnpm --filter @co-op/web dev`
and open the Plugins tab.

## Preparing the folder

Press **Submit a plugin** in the Plugins tab, or open
[plugin-prompt.md](plugin-prompt.md). It is a prompt you can paste into your
coding agent, which reads the source and prepares `plugin.json`, the
screenshot and the manual so your plugin looks like every other in the
library; check every line against how the plugin really behaves. The seed
plugins are examples.

## How people install a plugin

- **hundekraft, from co-op over USB.** The Plugins tab sends the file to the
  OP-1 over USB MIDI in Chrome or Edge. The OP-1 asks for T1 to install,
  then restarts.
- **hundekraft, from the hundekraft workshop.** Download the file from the
  Plugins tab and drop it onto **Your plugins** in the
  [hundekraft workshop](https://hundekraft.github.io/op1-workshop/), then
  press Update OP-1 and T1 on the OP-1.
- **opus, by copying.** With the OP-1 in disk mode, copy the `.opmod` file
  into the drive's `mods/` folder (co-op can do it from the Plugins tab when
  the drive is open). opus loads plugins in file-name order.

If a hundekraft OP-1 fails to start after a plugin is added, switching it off
and on starts it once without plugin files, so the plugin can be removed in
the workshop.

## Updating a plugin

1. Replace the file in the plugin folder with the new build, keeping its
   name.
2. Bump `version` in `plugin.json` (and in the file's own extension table,
   if you set one there).
3. Update `manual.md` and `screenshot.png` if the controls or the screen
   changed.
4. Open a pull request saying what changed.

Tell users in the manual or the pull request that on hundekraft a plugin is
replaced by removing the old one first, letting the OP-1 restart, then
installing the new one: installing a file of the same name straight over the
old one can hang the restart.

## Submitting it

1. Fork [coldsoldr/co-op](https://github.com/coldsoldr/co-op) and create a
   branch: `git checkout -b plugin/<id>`.
2. Add `plugins/<id>/` with the four files.
3. Run the validator (above).
4. Commit, push and open a pull request against `main`. Fill in the plugin
   checklist in the description, including the source link and how you built
   the file.

You can also upload the folder in the browser, as described for packs in
[contributing-packs.md](contributing-packs.md#in-the-browser-without-installing-anything).

GitHub does not run checks on a first-time contributor's pull request until a
maintainer approves the run, so the check may wait a while. Once merged, the
plugin appears in the Plugins tab after the next deploy.
