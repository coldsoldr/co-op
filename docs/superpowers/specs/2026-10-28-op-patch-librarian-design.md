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
    feedback/  feedback client
    util.ts    small shared helpers
```

`core/` is pure TypeScript with no React and no direct browser globals
except inside the store adapters. It is unit tested with Vitest against
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
  validation, build and the Playwright smoke test on every PR and push.
  On pull requests it also warns when a PR changes files under `packs/`
  and elsewhere (diffing the merge commit against its first parent, with
  two commits of history). There is no separate packs workflow: packs are validated in
  `ci.yml` and indexed by the Pages build.

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
  opus plugin listing. Feedback is covered only in its GitHub-issue fallback:
  the Worker-mode form (Turnstile and the feedback endpoint) is unit-tested
  only, because the e2e build has no Turnstile site key.
- Vendor data: tests that read `vendor/` (the hundekraft factory presets and
  firmware name table) skip when it is absent, as in CI. `CO_OP_PRESETS_ZIP`
  and `CO_OP_FIRMWARE_TABLE` point them at other copies.
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

Each phase is independently shippable and ends with a deploy.

## 12. Open items for the implementation plan

- Still open: confirm the exact stock OP-1 drive layout and patch limits
  per folder against a mounted device. The stock profile was built from
  the owner's real device files and the firmware notes, but no mounted
  drive has been walked end to end, so per-folder patch limits are not
  enforced.
- Decide the pack size limit for `packs/` once the first real packs are in,
  and whether Git LFS is needed.
- Settled: op1.fun's API docs put files, previews and pack zips in the
  `op1fun` S3 bucket, which sends no CORS headers (its preflight is a 403),
  so installs need the Worker file proxy. Still unverified against the live
  API: the exact list, pack and create response shapes, pagination, and
  where the profile page shows the API token.
