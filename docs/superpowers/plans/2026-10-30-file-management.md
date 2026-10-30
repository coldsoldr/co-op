# File Management Implementation Plan

**Goal:** Tick patches and move, copy or delete them on every page that manages patches, with the same tools for the OP-1 and My folder.

**Architecture:** `core/library/bulk.ts` runs one operation over many paths. `ui/places.ts` gives the OP-1 and My folder one interface, and `ui/BulkTools.tsx` is the toolbar and dialogs built on it. `PatchActions` takes either place.

**Tech Stack:** React, TypeScript, Vitest, Playwright, File System Access API.

**Spec:** `docs/superpowers/specs/2026-10-30-file-management-design.md`

**Rules:** Work on `dev`. Commits are by `coldsoldr` only, with no attribution lines.

---

### Task 1: Core

- `bulk.ts`: `forEachPath(paths, op, onProgress?)` returns `{ done, failed }` and never stops on a failure. Tests: all work; a failure partway.
- `operations.ts`: `removeFolderTree(store, folder)` removes a folder and all inside it, and refuses `''`. Tests: nested folders; empty path.
- `local-library.ts`: `findFolders(store)` lists every folder, skipping dot entries, so empty folders show. `folderTree(patches, folders)` includes them with a count of 0. Tests for both.
- `copy-to-folder.ts`: `copyFiles(store, items, onProgress?)` for files with no known hash, such as OP-1 patches. `item.read()` supplies the bytes, and identical files are skipped. Tests: new, identical, name clash, failed read.

### Task 2: Local library writes

`useLocalLibrary` gains `folders`, and `apply(op, touched)`, which gets write permission, runs `op`, then syncs `touched` and any paths `op` returned. `add` becomes that sync: paths that exist are re-read, paths that are gone are dropped, and `folders` is listed again.

### Task 3: Places and bulk tools

- `places.ts`: `Place` with `busy`, `staged`, `groups(paths)` and `run(op, touched)`, and `drivePlace(device)` and `localPlace(local)`.
- `BulkTools.tsx`: the toolbar, `BulkFolderDialog`, the delete confirmation listing up to 20 names, and the result line.
- e2e: drive move, copy, delete, and a staged drive.

### Task 4: Per-row actions

`PatchActions` takes a small entry type, an optional `root`, optional upload props and the drive name. `LocalActions.tsx` wraps it for My folder.

### Task 5: Pages

- Library drive view: `BulkTools` over shown ticked rows, plus `CopyFromDrive.tsx`.
- `LocalPatches`: checkboxes with no OP-1 attached, `BulkTools`, row actions. Send still sends only readable patches.
- Patches tab, My folder: the same.
- `Folders`: add and remove local folders, with empty folders shown.
- e2e: each page in the spec's table, new and removed folders.

### Task 6: Finish

Run lint, typecheck, unit and e2e tests, push to `dev`, and wait for CI and the deploy.
