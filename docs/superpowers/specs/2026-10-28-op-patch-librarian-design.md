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
| op1.fun | Browse and pull directly from the browser; upload via the Worker |
| Firmware model | `DeviceProfile` registry; stock OP-1 is the base profile |
| Reference firmware repos | `vendor/hundekraft`, `vendor/opus`, gitignored |
| Web stack | Vite, React, TypeScript |

## 4. Repository layout

```
web/        Vite + React + TypeScript app, built to static files for Pages
worker/     Cloudflare Worker: feedback endpoint and op1.fun upload proxy
shared/     Types and validators used by web, worker and CI
packs/      Community packs, one folder each (index.json is generated at build time)
docs/       Specs, contributor guide, pack submission guide
vendor/     Gitignored reference clones of hundekraft and opus
.github/    Actions workflows, issue templates, PR template
```

The repo is a pnpm workspace with `web`, `worker` and `shared` as packages.
`shared` holds the pack manifest schema and the feedback payload schema.

## 5. Web app architecture

The app is layered so that browser APIs, device and firmware specifics, and
UI never mix.

```
ui/          React screens and components
features/    device-library, catalogue, op1fun, feedback
core/
  codec/     AIFF reader/writer, OP-1 APPL JSON chunk, patch model
  profiles/  DeviceProfile registry: op1/stock, op1/hundekraft, op1/opus
  store/     DeviceStore interface and adapters (fs-access, drag-drop)
  services/  packs index client, op1.fun client, feedback client
```

`core/` is pure TypeScript with no React and no direct browser globals
except inside the store adapters. It is unit tested with Vitest against
fixture files.

### 5.1 Patch model and codec

A `Patch` is the in-memory representation of one patch file:

- `kind`: `synth`, `drum` or `unknown`. Firmware-specific kinds (for example
  hundekraft `gin` drum kits) arrive with the profile that needs them, in
  phase 7.
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
  patchKinds: PatchKindSpec[];    // kind, extensions; parser/validator hooks deferred to phase 7
  detect(root: DirectoryListing): Promise<DetectResult>; // folder-level; confidence 0..1 plus reason
  compatibility(patch: Patch): Compat;          // ok | warn | unsupported
}
```

- `op1/stock` is written first from the real device layout, which the
  implementation plan verifies against a mounted OP-1. Expected top-level
  folders are `album`, `drum`, `synth` and `tape`, with `synth` and `drum`
  holding category subfolders of patches.
- `op1/hundekraft` and `op1/opus` extend the stock profile. Their extra
  patch kinds, metadata fields and folder differences are read from the
  clones in `vendor/` during their own implementation phases. They are not
  specified further here.
- Detection runs on every mount. The UI shows the detected profile and lets
  the user override it. Folder-level detection cannot tell stock from
  hundekraft (plugins live on the internal volume, so disk mode looks
  identical) or from OP-1 Field, so stock caps its confidence below 1 and
  the hundekraft profile adds a content-sniff phase over file bytes. Every patch shown anywhere carries a compatibility
  badge for the active profile.

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

- Settings screen stores email and API token in `localStorage` only after
  the user opts in, with a clear note that nothing is sent to us.
- Browse own and other users' packs and patches, preview via the MP3 link,
  pull a patch or a pack onto the device.
- Upload a device patch: the app sends the request to the Worker, which
  forwards it to op1.fun with the user's headers and returns the response.
  The Worker never logs or stores credentials or file contents.

Feedback

- Form with category (bug, idea, pack request, other), message, and an
  optional contact field.
- The app attaches: app version and commit, active profile, store type,
  browser user agent, and whether a device was mounted.
- Submission goes to the Worker, which opens a GitHub Issue labelled
  `feedback` plus the category. The user gets the Issue URL back.

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
most 64 characters, unique ignoring case; every listed file present with that
exact spelling and nothing unlisted; every file parses with the codec as the
kind its folder names. Limits: 2 MB per patch, 20 MB of patches per pack,
64 KB each for `pack.json` and `README.md`.

CI runs `pnpm --filter @co-op/web packs:validate` on every pull request;
merge is blocked on failure. **`index.json` is not committed.** The Pages
build (`pnpm --filter @co-op/web build`) validates the packs again, copies
each pack's `pack.json`, `README.md` and listed patches into
`web/dist/packs/<slug>/`, and writes `web/dist/packs/index.json` (format 1:
each pack's fields, total size, and per patch its kind, name, engine, size
and SHA-256), so the index can never drift from the files. The dev server
builds the same index on the fly. The app fetches `packs/index.json` from
its own origin, revalidating each time, and checks every downloaded patch
against its SHA-256 before playing or installing it.

The contributor guide (`docs/contributing-packs.md`) walks through forking,
adding a folder (with git or in the GitHub web UI), validating and opening
the pull request. The pull request template asks for licence confirmation.

## 7. Worker

Cloudflare Worker, TypeScript, deployed with wrangler.

Endpoints

- `POST /feedback` Validates the payload against the shared schema, applies
  a per-IP rate limit, verifies a Cloudflare Turnstile token, then creates
  an Issue through the GitHub REST API using a fine-grained token scoped to
  Issues on this repo. Returns the Issue URL.
- `POST /op1fun/patches` Validates that the body matches op1.fun's create
  patch shape and size limit, forwards it with the caller's `X-User-Email`
  and `X-User-Token` headers to `https://api.op1.fun/v1/patches`, and
  returns the upstream status and body unchanged.

Rules

- CORS allows only the Pages origin and localhost for development.
- No persistent storage. No logging of request bodies or auth headers.
- Secrets: `GITHUB_TOKEN`, `TURNSTILE_SECRET`. Set through wrangler, never
  committed. `.dev.vars` is gitignored.

Why a proxy for uploads: api.op1.fun answers preflight with
`Access-Control-Allow-Methods: GET, OPTIONS` only, so browsers block a
cross-origin POST. GET requests with the auth headers are allowed and go
direct.

## 8. Deployment

- `pages.yml`: on push to `main`, check, run the Playwright test, build `web/`
  (which validates the packs and writes `dist/packs/index.json`), upload to
  GitHub Pages.
- `worker.yml`: on push to `main` touching `worker/` or `shared/`, deploy
  with wrangler using `CLOUDFLARE_API_TOKEN` and `CLOUDFLARE_ACCOUNT_ID`
  repo secrets.
- `ci.yml`: typecheck, lint, unit tests, pack validation, build and the
  Playwright smoke test on every PR and push. There is no separate packs
  workflow: packs are validated in `ci.yml` and indexed by the Pages build.

The owner supplies a Cloudflare account and the GitHub fine-grained token.
Until the Worker is deployed the app shows feedback and op1.fun upload as
unavailable rather than failing.

## 9. Error handling

- Device operations are atomic per file: write to a temp name, then move.
  A failed batch reports which files succeeded and stops.
- The codec rejects containers it cannot parse (truncated chunks, missing
  pad bytes, trailing bytes, more than one op-1 chunk) and the UI marks them
  as unreadable rather than hiding them. Metadata it would re-serialise
  differently is flagged via `op1Canonical`, not rejected; unchanged
  operations always copy `raw`.
- Network features degrade independently: a Worker outage disables feedback
  and op1.fun upload while the catalogue and device library keep working.
- op1.fun 401 responses prompt the user to re-check credentials; other
  upstream errors are shown with the upstream message.

## 10. Testing

- Vitest: codec round trip on fixtures for every supported kind; profile
  detection on synthetic directory listings; manifest validation; Worker
  handlers with mocked fetch.
- Playwright: mount a fixture folder through drag-and-drop, browse, preview,
  stage a change, download the zip; feedback form against a mocked Worker.
- Fixtures: a small set of real stock patches contributed under a permissive
  license, checked into `web/test/fixtures`.

## 11. Phasing

1. Scaffold the workspace, CI and Pages deploy with a placeholder page.
2. Codec and stock profile with fixtures and tests.
3. Device stores and the device library UI.
4. Packs validator, index generation, catalogue UI, contributor guide.
5. Worker with feedback endpoint, feedback UI.
6. op1.fun client, browse and pull, Worker upload proxy, upload UI.
7. hundekraft profile from `vendor/hundekraft`.
8. opus profile from `vendor/opus`.

Each phase is independently shippable and ends with a deploy.

## 12. Open items for the implementation plan

- Confirm the exact stock OP-1 drive layout and patch limits per folder
  against a mounted device before writing the stock profile.
- Decide the pack size limit for `packs/` once the first real packs are in,
  and whether Git LFS is needed.
- Confirm op1.fun's S3 file links allow cross-origin `fetch`; if not, the
  pull path also goes through the Worker.
