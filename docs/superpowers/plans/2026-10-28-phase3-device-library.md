# Phase 3: Device Library Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Let a user connect their OP-1 (disk mode) in the browser, see every patch with its firmware compatibility, preview audio, and rename, move, copy, delete, back up and restore patches. Chromium gets direct read/write; other browsers get a read-only folder drop with changes staged into a downloadable zip.

**Architecture:** A `DeviceStore` interface extends the existing `DirectoryListing` with byte read/write and file operations. Three implementations: `MemoryStore` (tests), `FsAccessStore` (File System Access API) and `DropStore` (read-only, from a dropped or picked folder). Pure functions in `core/library` build the library view, run operations, and produce and apply backups. Writes are atomic per file (temp name then move). For a read-only store, operations go into a `ChangeSet` that is exported as a zip. The React UI sits on top with a library screen next to the existing inspector.

**Tech stack:** existing workspace; adds `fflate` (zip), `@playwright/test` (smoke test), `eslint` + `typescript-eslint` (lint gate required by spec section 8).

**Spec:** sections 5.3, 5.4 (Device library), 8 (lint and Playwright in CI), 9 (atomic writes, unreadable files shown), 10.

**Facts established in phases 1-2 that this plan relies on:**
- `Patch.raw` is the bytes to copy for any unedited operation. Call `writePatch` only after `withMeta`. Non-canonical files (`op1Canonical === false`) must never be re-serialised by copy, move, backup or restore.
- `DirectoryListing.list(path)` returns `[]` for missing paths and files, sorted by code point.
- `scanDrive(root, profile)` returns `ScannedPatch { path, folder, fileName, kind }` with `kind` from the folder; `Patch.kind` comes from metadata. They can disagree.
- TypeScript's DOM lib has `FileSystemDirectoryHandle.entries()` but not `requestPermission`, `queryPermission` or `window.showDirectoryPicker`; declare them in `web/src/types/fs-access.d.ts`.
- `exactOptionalPropertyTypes` is on.

---

## File structure

```
web/src/types/fs-access.d.ts          ambient declarations for the missing FSA members
web/src/core/store/store.ts           DeviceStore, StoreCapabilities, StoreError, joinPath/dirname/basename
web/src/core/store/memory-store.ts    writable in-memory DeviceStore (tests, and staging for DropStore)
web/src/core/store/fs-access-store.ts FsAccessStore over a FileSystemDirectoryHandle
web/src/core/store/drop-store.ts      DropStore over a File[] with webkitRelativePath
web/src/core/store/atomic.ts          writeAtomic(store, path, bytes)
web/src/core/library/library.ts       LibraryEntry, loadLibrary, loadEntryPatch
web/src/core/library/operations.ts    renameFile, setPatchName, movePatch, copyPatch, deletePatch, freeName
web/src/core/library/changeset.ts     ChangeSet recorder store wrapper, exportChangeSet (zip)
web/src/core/library/backup.ts        createBackup, readBackup, diffBackup, applyRestore
web/src/ui/Library.tsx                connect/drop, profile picker, patch table, actions
web/src/ui/PatchActions.tsx           rename/move/copy/delete dialogs for one entry
web/src/ui/BackupPanel.tsx            backup download, restore upload with diff and apply
web/src/ui/useDevice.ts               React hook holding store, profile, entries, refresh
web/src/ui/audio.ts                   playPcm(pcm, sampleRate) shared by Inspector and Library
web/src/App.tsx                       tabs: Library | Inspector
web/e2e/library.spec.ts               Playwright smoke test
web/playwright.config.ts
eslint.config.js                      root flat config
```

Every `core` module has a sibling `*.test.ts`. Use `MemoryStore` in tests; never touch real browser APIs in unit tests except via small fakes for `FsAccessStore` and `DropStore`.

---

### Task 1: Lint gate

- [ ] Add dev deps at the root: `pnpm add -Dw eslint @eslint/js typescript-eslint eslint-plugin-react-hooks globals`.
- [ ] Create `eslint.config.js` (flat config): `@eslint/js` recommended, `typescript-eslint` recommended (not type-checked, to stay fast and independent of TypeScript 7's API), `react-hooks` recommended for `web/src/**/*.tsx`, browser globals for `web/src`, node globals for `web/test` and config files; ignore `**/dist`, `**/node_modules`, `vendor/**`.
- [ ] Root script `"lint": "eslint ."`. Fix any existing findings (expect few). If `typescript-eslint` refuses to load with TypeScript 7, pin `typescript-eslint`'s parser to use `typescript@5` via a root devDependency alias only for linting, or drop to `@eslint/js` + `eslint-plugin-react-hooks` with a TS parser that works, and record the choice in the commit message.
- [ ] Add `- run: pnpm lint` to `.github/workflows/ci.yml` and to the build job of `pages.yml`, before typecheck.
- [ ] `pnpm lint && pnpm typecheck && pnpm test` green. Commit `ci: add eslint gate`.

### Task 2: DeviceStore interface and MemoryStore

`web/src/core/store/store.ts`:
```ts
import type { DirectoryListing } from './listing';

export interface StoreCapabilities {
  /** False for read-only sources such as a dropped folder. */
  write: boolean;
}

export class StoreError extends Error {
  override name = 'StoreError';
}

/** A device drive. Paths are '/'-separated, relative to the drive root, never starting with '/'. */
export interface DeviceStore extends DirectoryListing {
  readonly capabilities: StoreCapabilities;
  /** Human label for the source, e.g. the picked folder name. */
  readonly label: string;
  /** Throws StoreError if the path is missing or is a folder. */
  read(path: string): Promise<Uint8Array>;
  /** Size in bytes without reading the file. Throws StoreError if missing. */
  size(path: string): Promise<number>;
  exists(path: string): Promise<boolean>;
  /** Creates parent folders as needed; overwrites. Throws StoreError when capabilities.write is false. */
  write(path: string, bytes: Uint8Array): Promise<void>;
  /** Moves a file; throws if `to` exists. */
  move(from: string, to: string): Promise<void>;
  /** Removes a file (not folders). */
  remove(path: string): Promise<void>;
  /** Creates a folder and its parents; no-op if present. */
  mkdir(path: string): Promise<void>;
}

export function joinPath(...parts: string[]): string;   // joins, collapses duplicate '/', strips leading/trailing '/'
export function dirname(path: string): string;          // '' for top-level
export function basename(path: string): string;
```
Implement the three helpers. Tests: `joinPath('synth', 'user', 'a.aif') === 'synth/user/a.aif'`, `joinPath('', 'a')`, `joinPath('a/', '/b')`, `dirname('a.aif') === ''`, `dirname('synth/user/a.aif') === 'synth/user'`, `basename`.

`MemoryStore` (`memory-store.ts`): constructor takes `Record<string, Uint8Array>` keyed by file path plus optional `{ write?: boolean; label?: string }`. Folders are implied by file paths and by explicit `mkdir`. Implements every method with the contracts above, including code-point sorted `list`, `[]` for missing paths, `StoreError` messages naming the path. Expose `snapshot(): Record<string, Uint8Array>` for tests. Tests cover each method, the read-only rejection, move onto an existing path throwing, and that `list` shows folders created only via `mkdir`.

Commit `feat(store): DeviceStore interface and MemoryStore`.

### Task 3: Atomic write

`atomic.ts`: `writeAtomic(store, path, bytes)` writes to `${dirname}/.co-op-tmp-${basename}`, then removes an existing target and moves the temp into place. If the move fails, it removes the temp and rethrows. The scanner already skips dot files so a leftover temp never shows as a patch. Tests with a `MemoryStore` subclass whose `move` throws once: target untouched, temp removed, error propagated. Commit `feat(store): atomic per-file writes`.

### Task 4: FsAccessStore

`web/src/types/fs-access.d.ts` declares, as global interface augmentations: `FileSystemHandle.queryPermission(desc?: { mode?: 'read' | 'readwrite' }): Promise<PermissionState>`, `requestPermission(...)` likewise, optional `FileSystemFileHandle.move?(destination: FileSystemDirectoryHandle, name: string): Promise<void>`, and `Window.showDirectoryPicker?(options?: { id?: string; mode?: 'read' | 'readwrite'; startIn?: string }): Promise<FileSystemDirectoryHandle>`. `showDirectoryPicker` is optional so feature detection type-checks.

`fs-access-store.ts`: `class FsAccessStore implements DeviceStore` constructed from a root `FileSystemDirectoryHandle`; `capabilities.write = true`; `label = root.name`. Resolve paths by walking `getDirectoryHandle` per segment; map `NotFoundError` and `TypeMismatchError` (`DOMException.name`) to `[]` in `list`, `false` in `exists`, and `StoreError` elsewhere. `list` uses `entries()` and sorts by code point. `write` uses `createWritable()` then `write(bytes)` and `close()`, creating parent directories. `move` uses `FileSystemFileHandle.move` when present, otherwise read, write, remove. `remove` uses `removeEntry(name)` on the parent.

Also export `connectDevice(): Promise<FsAccessStore | null>` that calls `window.showDirectoryPicker({ id: 'op1', mode: 'readwrite' })`, returns `null` if the user cancels (`AbortError`), and `isFsAccessSupported(): boolean`. Persisting the handle in IndexedDB: `rememberHandle(handle)` and `restoreHandle(): Promise<FileSystemDirectoryHandle | null>` using a tiny raw IndexedDB wrapper (db `co-op`, store `handles`, key `device`), and `ensurePermission(handle): Promise<boolean>` that calls `queryPermission` then `requestPermission` with `readwrite`. Wrap all IndexedDB calls in try/catch returning null on failure.

Tests: write a small in-memory fake of `FileSystemDirectoryHandle`/`FileSystemFileHandle` in `web/test/fake-fs-access.ts` (only the members the store uses, throwing `DOMException('...', 'NotFoundError')` like Chromium) and run the same behavioural assertions as for MemoryStore against it. Do not unit-test IndexedDB or the picker. Commit `feat(store): File System Access store`.

### Task 5: DropStore

`drop-store.ts`: `class DropStore implements DeviceStore`, `capabilities.write = false`. Constructed from `File[]` whose `webkitRelativePath` starts with the dropped folder name (strip that first segment; `label` is that name). Also export `filesFromDataTransfer(dt: DataTransfer): Promise<File[]>` that walks `webkitGetAsEntry()` directory entries recursively, setting a relative path (store paths in a `Map<File, string>` rather than mutating `webkitRelativePath`, which is read-only; have the constructor accept `Array<{ path: string; file: File }>` and provide `fromInputFiles(files: FileList)` for the `webkitdirectory` input). Writes throw `StoreError('read-only source: ...')`. Tests use `new File([...], name)` objects (Node 22 has `File`). Commit `feat(store): read-only DropStore for dropped folders`.

### Task 6: Library model

`library.ts`:
```ts
export interface LibraryEntry extends ScannedPatch {
  sizeBytes: number;
  /** Filled by loadEntryPatch; null until loaded. */
  patch: Patch | null;
  /** Set when reading or parsing failed; the entry stays visible as unreadable. */
  error: string | null;
  compat: Compat | null;
  /** True when the folder kind (synth/drum) disagrees with the metadata kind. */
  kindMismatch: boolean;
}
export async function loadLibrary(store: DeviceStore, profile: DeviceProfile): Promise<LibraryEntry[]>;
export async function loadEntryPatch(store: DeviceStore, profile: DeviceProfile, entry: LibraryEntry): Promise<LibraryEntry>;
export async function loadAllPatches(store, profile, entries, onProgress?: (done: number, total: number) => void, concurrency = 4): Promise<LibraryEntry[]>;
```
`loadLibrary` scans and fills `sizeBytes` via `store.size`, leaving `patch` null. `loadEntryPatch` reads, parses with `readPatch`, sets `compat = profile.compatibility(patch)`, and `kindMismatch = patch.kind !== 'unknown' && patch.kind !== entry.kind`; on any error sets `error` to the message and leaves the entry in the list. Folder kind wins for where the patch lives; the UI shows the mismatch as a warning. `loadAllPatches` runs with bounded concurrency and preserves order. Tests: a MemoryStore built from the committed fixtures (`synth/user/cluster.aif`, `drum/user/boombap.aif`, a drum file placed in `synth/user`, a garbage file `synth/user/bad.aif`) cover every field and the mismatch and error paths, plus order preservation with concurrency 2. Commit `feat(library): library model with lazy patch loading`.

### Task 7: Operations

`operations.ts` (all take `store`, all use `writeAtomic` for writes, all refuse with `StoreError` on read-only stores; the ChangeSet wrapper in Task 8 is what makes them usable on a DropStore):
- `freeName(store, folderPath, fileName): Promise<string>`: returns `fileName` or `name-2.aif`, `name-3.aif` ... that does not exist yet.
- `renameFile(store, path, newFileName)`: same folder, rejects names containing `/` or starting with `.`, rejects an existing target; returns the new path.
- `setPatchName(store, path, newName)`: reads, `withMeta` with the new `name` (reject empty names and names over 24 characters, the length the OP-1 shows; this is a UI guard, not a device spec), writes `raw` atomically. Rejects files with no metadata.
- `movePatch(store, path, targetFolder)`: `targetFolder` like `synth/user`; uses `freeName`; returns new path.
- `copyPatch(store, path, targetFolder)`: copies raw bytes unchanged; uses `freeName`; returns new path.
- `deletePatch(store, path)`.
- `ensureFolder(store, rootPath, name)`: creates `synth/<name>`; rejects names with `/` or a leading `.`.
Tests on MemoryStore for each, including byte-identical copies of the real non-canonical-safe path (copy `real/drum.aif` and compare bytes), collision naming, and read-only rejection. Commit `feat(library): rename, move, copy, delete and patch name edits`.

### Task 8: ChangeSet for read-only sources

`changeset.ts`: `class StagedStore implements DeviceStore` wrapping a read-only base store. Reads fall through to staged bytes first, then the base. `write`/`move`/`remove`/`mkdir` record into an overlay; `capabilities.write = true`. `changes(): Change[]` where `Change = { op: 'write'; path; bytes } | { op: 'remove'; path }` reduced to final state (a write then remove of the same path yields just a remove when the base had it, nothing when it did not). `exportChangeSet(changes): Uint8Array` builds a zip with fflate: every written file at its path, plus `CHANGES.txt` listing removals and plain instructions ("copy these folders onto the OP-1 in disk mode, merging; then delete the files listed below"). Tests: operations from Task 7 run against a StagedStore over a read-only MemoryStore, `changes()` is minimal, and the zip unzips (fflate `unzipSync`) to the expected entries. Commit `feat(library): staged changes and zip export for read-only sources`.

### Task 9: Backup and restore

`backup.ts`:
- `createBackup(store, profile, onProgress?)`: zip every file under the profile's patch roots (all files, not only patches, so nothing is lost; skip dot files), raw bytes, plus `co-op-backup.json` `{ format: 1, createdAt: ISO string, profile: id, label: store.label, files: [{ path, size, sha256 }] }`. Use `crypto.subtle.digest` (available in Node 22 and browsers). Returns `Uint8Array`. The `createdAt` value comes from an injectable `now = () => new Date()` parameter so tests are deterministic.
- `readBackup(zip: Uint8Array)`: validates the manifest (format 1, every listed file present, sha matches); throws `BackupError` with a clear message otherwise; returns `{ manifest, files: Map<path, bytes> }`.
- `diffBackup(store, backup)`: returns `[{ path, status: 'new' | 'changed' | 'same' }]` comparing bytes, plus `onDeviceOnly: string[]` for patch-root files the backup lacks.
- `applyRestore(store, backup, paths: string[])`: writes each selected path atomically; never deletes anything.
Tests: back up a MemoryStore, change one file, add one, delete one, diff shows changed/new/onDeviceOnly correctly, restore of the selected paths brings bytes back exactly, a tampered zip is rejected. Commit `feat(library): backup and restore`.

### Task 10: Library UI

- `ui/audio.ts`: `playPcm(pcm: Float32Array, sampleRate: number): () => void` (returns stop). Use `getChannelData(0).set(pcm)`. Make the Inspector use it.
- `ui/useDevice.ts`: holds `store`, `profile` (detected via `detectProfiles`, overridable with any id from `profiles`), `entries`, `loading` and `error`; `connect()` (FSA picker, remembers the handle), `reconnect()` (restore handle + permission), `useDrop(files)`, `refresh()`, and `run(op)` which runs an operation, then refreshes. When the store is a DropStore, wrap it in a StagedStore and expose `changes`.
- `ui/Library.tsx`: when nothing is connected, show "Connect OP-1" (only if `isFsAccessSupported()`), "Reconnect" if a remembered handle exists, and a drop zone plus a `webkitdirectory` file input for every browser, with one line explaining disk mode (shift + COM, then DISK). When connected: source label, detected profile with reason and a select to override, read-only notice for drop sources, a table grouped by folder with columns Name (metadata name, falling back to file name), File, Kind, Size, Compatibility badge (ok, warn with reason as title, unsupported, unreadable with error), and a kind-mismatch warning. Load patches lazily with `loadAllPatches` and a progress line. Row actions via `PatchActions.tsx`: Play, Rename file, Set patch name, Move to folder, Copy to folder (folder selects list existing folders under the same root plus "new folder..."), Delete with a confirm dialog naming the file. For staged sources, show a "Download changes (N)" button that saves `exportChangeSet` output.
- `ui/BackupPanel.tsx`: "Download backup" (filename `op1-backup-YYYY-MM-DD.zip`) and "Restore from backup" (file input, then a checklist of new/changed files with all changed and new checked by default, onDeviceOnly listed for information, then Apply).
- `App.tsx`: two tabs, Library (default) and Inspector, with a heading and a footer link to the GitHub repo.
- Keep styling in `index.css` with a handful of classes; no UI library. Accessible labels on every control; buttons, not clickable divs.
- Typecheck, lint, test, build. Commit `feat(web): device library UI`.

### Task 11: Playwright smoke test

- Add `@playwright/test` to `web` dev deps, `web/playwright.config.ts` running `pnpm build && pnpm preview --port 4173` as `webServer`, Chromium only, base URL `http://localhost:4173/co-op/`.
- `web/e2e/library.spec.ts`: on the Library tab, set the `webkitdirectory` input to a fixture folder built at test time in a temp dir (`synth/user/cluster.aif`, `drum/user/boombap.aif`, `tape/`), assert two rows, kinds, compat `ok`, the read-only notice; delete one patch, assert "Download changes (1)", click it and assert a download named `*.zip`. Switch to Inspector and assert the heading.
- Script `"e2e": "playwright test"` in `web/package.json`; exclude `e2e/` from vitest. CI: new job `e2e` in `ci.yml` running `pnpm exec playwright install --with-deps chromium` then `pnpm --filter @co-op/web e2e`.
- Commit `test(web): Playwright smoke test for the library`.

### Task 12: Docs and deploy

- README "What exists today" updated for phase 3; spec section 5.3 interface synced with the implemented `DeviceStore` (adds `label`, `size`, `exists`, `mkdir`; drops `watch`).
- Push, watch `ci` and `pages`, verify the live bundle contains "Connect OP-1".
- Commit `docs: phase 3 device library`.

---

## Self-review

- Spec 5.3: interface, both adapters, persisted handle, staged zip for read-only: Tasks 2, 4, 5, 8.
- Spec 5.4 device library: mount, scan, profile, tree, preview, rename/move/copy/delete with confirm, backup, restore with diff: Tasks 6, 7, 9, 10.
- Spec 8 lint and Playwright: Tasks 1, 11. Spec 9 atomic writes and unreadable files visible: Tasks 3, 6.
- Raw-bytes rule: Tasks 7 (copy, move), 9 (backup, restore) use raw bytes; only `setPatchName` uses `withMeta`.
- Type names used across tasks: `DeviceStore`, `StoreError`, `MemoryStore`, `FsAccessStore`, `DropStore`, `StagedStore`, `LibraryEntry`, `BackupError`; consistent.
