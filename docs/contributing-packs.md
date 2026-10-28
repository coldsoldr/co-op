# Contributing a patch pack

co-op's **Packs** tab lists community patch packs that live in this
repository, in the [`packs/`](../packs) folder. Anyone can preview them in the
browser and install them onto an OP-1. You add a pack by opening a pull
request; a check runs on the pull request to make sure the pack is valid, and
maintainers merge only when the check passes.

## What a pack is

A pack is one folder of OP-1 synth and drum patches (`.aif` files saved by an
OP-1, or made by a tool that writes OP-1 metadata) plus a small `pack.json`
that describes it.

```
packs/<slug>/pack.json
packs/<slug>/README.md          optional
packs/<slug>/synth/<file>.aif   synth patches
packs/<slug>/drum/<file>.aif    drum patches
```

- Patches sit directly in `synth/` or `drum/`. Subfolders are not allowed.
- The folders are spelled in lower case: `synth/` and `drum/`.
- Nothing else may be in the folder. Every patch must be listed in
  `pack.json`, and every file listed must be there with exactly that
  spelling, including upper and lower case.
- Symbolic links are not allowed; add the real files.
- `.DS_Store` (macOS Finder), `Thumbs.db` and `desktop.ini` (Windows
  Explorer) are ignored if they end up in the folder, and never published,
  so you do not need to hunt them down.

## Getting patches off your OP-1

1. On the OP-1, press **shift + COM**, then choose **DISK**. Connect it to
   your computer with USB; it shows up as a drive (like a USB stick).
2. Synth patches are in the drive's `synth/` folder and drum kits in
   `drum/`, each inside a subfolder such as `user/`. Copy the `.aif`
   files you want onto your computer. Copying does not change them.
3. Eject the drive on your computer before unplugging the cable.

Rename the copies if you like (the name shown on the OP-1 comes from inside
the file, not the file name), keeping the `.aif` ending.

The [`test-tones`](../packs/test-tones) pack is a complete small example.

## pack.json

Create `pack.json` with any plain-text editor. On Windows, Notepad saves
"Text Documents" as `pack.json.txt` by default, and Explorer hides the
`.txt` ending: in the Save dialog, set **Save as type** to **All files** and
type `pack.json`. (A file saved as UTF-8 with a byte order mark, which
Notepad sometimes adds, is fine.) On a Mac, TextEdit must be in plain-text
mode (**Format → Make Plain Text**).

```json
{
  "format": 1,
  "name": "Dusty Keys",
  "slug": "dusty-keys",
  "author": "your name or handle",
  "description": "Lo-fi electric piano patches sampled from my own Rhodes.",
  "license": "CC-BY-4.0",
  "firmware": ["op1/stock"],
  "tags": ["keys", "lofi"],
  "patches": [
    { "file": "synth/rhodes-soft.aif", "notes": "Velocity feels best with the knob at noon." },
    { "file": "drum/brushes.aif" }
  ]
}
```

| Field | Rules |
| --- | --- |
| `format` | Always `1`. |
| `name` | 1 to 60 characters. |
| `slug` | Must equal the folder name. Lower-case letters and digits in words joined by single hyphens (`dusty-keys`), at most 40 characters. |
| `author` | 1 to 60 characters. |
| `description` | 1 to 500 characters. |
| `license` | One of `CC0-1.0`, `CC-BY-4.0`, `CC-BY-SA-4.0`, `CC-BY-NC-4.0`, `CC-BY-NC-SA-4.0`. See below. |
| `firmware` | At least one device profile the patches are made for. Today only `op1/stock` exists. |
| `tags` | Optional, up to 8 tags of 1 to 20 lower-case letters, digits or hyphens. |
| `patches` | 1 to 100 entries. `file` is `synth/<name>.aif` or `drum/<name>.aif` (`.aif` or `.aiff`, any case); `notes` is optional, up to 200 characters. |

Patch file names must work on the OP-1's drive and on every computer: at
most 64 characters, no `/ \ : * ? " < > |` or control characters, not
starting with a dot, not ending in a dot or a space, and not a name Windows
reserves (`con`, `prn`, `aux`, `nul`, `com1` to `com9`, `lpt1` to `lpt9`,
with any extension). Two files may not differ only in upper and lower case.

Each patch must open as an OP-1 patch, and its kind must match its folder:
synth patches in `synth/`, drum kits in `drum/`.

Size limits: each patch at most 2 MB, all patches together at most 20 MB,
and `pack.json` and `README.md` at most 64 KB each.

## Licences

You can only share sounds you made yourself, or sounds whose owner has given
you the right to share them under the licence you choose.

- **Samples from commercial sample packs, sound libraries, records, films or
  other people's patches are not allowed**, even if you changed them, unless
  their licence explicitly lets you redistribute them under one of the
  licences below. Most commercial sample licences do not.
- Pick one licence for the whole pack:
  - `CC0-1.0`: anyone can use the sounds for anything, no credit needed.
  - `CC-BY-4.0`: anyone can use them for anything, with credit to you.
  - `CC-BY-SA-4.0`: as CC-BY, and changed versions must use the same licence.
  - `CC-BY-NC-4.0`: credit, and no commercial use.
  - `CC-BY-NC-SA-4.0`: credit, no commercial use, same licence for changes.
- "All rights reserved" packs are not accepted: the point of the catalogue is
  that people can install and use what they find.

If you are unsure whether you can share something, ask in the pull request
before adding it.

## Checking your pack

If you have Node 22 and pnpm installed (see the main README), run from the
repository root:

```bash
pnpm install
pnpm --filter @co-op/web packs:validate
```

It prints every problem it finds, grouped by pack, and ends with
`validated N packs` when everything is fine. The same check runs on your
pull request, so you can also skip this step and read the result there.

GitHub does not run checks on a first-time contributor's pull request until a
maintainer approves the run, so the check may say it is waiting for approval
for a while. That is normal; a maintainer starts it when they look at the
pull request.

To see your pack in the app before submitting, run
`pnpm --filter @co-op/web dev` and open the Packs tab.

## Submitting it

### With git

1. Fork [coldsoldr/co-op](https://github.com/coldsoldr/co-op) on GitHub and
   clone your fork.
2. Create a branch: `git checkout -b pack/<slug>`.
3. Add `packs/<slug>/` with `pack.json`, your patches and optionally a
   `README.md` (how the sounds were made, credits, tips).
4. Run the validator (above).
5. Commit, push the branch and open a pull request against `main`. Fill in
   the checklist in the pull request description.

### In the browser, without installing anything

1. On your computer, make the pack folder: a folder named after the slug
   holding `pack.json`, an optional `README.md`, and `synth/` and `drum/`
   folders with your patches.
2. Open [coldsoldr/co-op](https://github.com/coldsoldr/co-op) and click
   **Fork** (top right), then **Create fork**.
3. In your fork, open the `packs` folder, choose **Add file → Upload
   files**, and drag your whole pack folder onto the page. GitHub keeps the
   folders inside it. Choose **Create a new branch** and commit.
4. Click **Compare & pull request** (or **Contribute → Open pull request** on
   the branch page) and fill in the checklist. The check on the pull request
   tells you if anything needs fixing; you can keep uploading to the same
   branch until it passes.

A maintainer will start the check if needed, listen to the pack, check the
licence, and merge it once the check passes. Once merged, it appears in the
Packs tab after the next deploy, usually within a few minutes.
