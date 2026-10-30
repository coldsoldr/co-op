# Pack auditions and Copy to My folder

Agreed with the owner on 2026-10-30. Built on `dev`, and merged to `main` only when the owner says
so.

## Goal

People browse community patches and packs with no OP-1 attached, hear them, and keep the ones they
like in their own folder. Patches inside a pack audition the same way as on the Patches tab.

Browsing without a device already works. Two things are missing: auditioning on the pack page, and
any way of writing to the local folder, which co-op opens read-only today.

## Auditioning in packs

The pack page behaves like the Patches tab:

- The audition bar is shown, with Put my sound back.
- Clicking a row loads the patch on the OP-1 once the plugin has answered.
- The preview button plays in the browser and opens the on-screen keyboard. It works with no OP-1.

The row moves from `PatchesTab.tsx` into a shared `PatchRow` component, and `playsOnOp1` moves with
it. The pack page's own play code is removed. Its checkboxes, notes and file column stay. Packs
receives `local` and `audition` from `App.tsx`, as the Patches tab does.

## Copying

| Where | Controls |
| --- | --- |
| Patches tab, Community source | A checkbox on each row, select all, and Copy to My folder (n) |
| Pack page | Copy selected and Copy all, next to Install |
| Pack card | Copy to My folder, for the whole pack |

The My folder source has no copy controls, because those files are already there.

### Where files go

Each patch goes to `<pack slug>/<pack file>` in the local folder, which mirrors the pack:
`gl1tch/drum/breaks-17763.aif`. This is the `path` a community `LocalPatch` already has. Only
patches are copied, not `README.md` or `pack.json`. Files are written unchanged, and
`fetchPatchBytes` checks each download against the pack's SHA-256.

### Files already there

For a patch whose target is `name.aif`, co-op looks at `name.aif`, `name-2.aif`, `name-3.aif` and so
on, in that order:

- A file with the same SHA-256 means the patch is already there, and it is skipped.
- The first name that does not exist is where the patch is written.

An existing file is never overwritten, and copying the same patch twice never adds a duplicate.

### Progress and results

- Patches are copied in turn. The button shows `Copying 3 of 20` and a second copy cannot start
  while one is running.
- The result says what happened, for example `Copied 18, 2 already there`.
- A failed patch does not stop the rest. The result names each failure and its reason.
- New files are read and added to the local list, so they appear in My folder and the Library with
  no rescan of the whole folder.

### Folder access

- With no folder open, Copy opens the folder picker and then copies. A cancelled picker copies
  nothing and shows no error.
- With a remembered folder that needs permission again, Copy asks for it in the same click.
- The folder is still opened read-only. The first copy asks for write permission. If that is
  refused, nothing is written and the message says so.
- The Patches tab's "co-op only reads the folder" text changes to say that co-op writes only when
  asked to copy.

### Browsers that cannot open a folder

In Firefox and Safari the same controls read Download zip. They save `co-op-patches.zip` with the
same layout, stored without compression because patches are audio. Duplicate checks do not apply,
as there is no folder to compare with.

## Units

- `core/library/copy-to-folder.ts`: `copyPatches(store, patches, fetchBytes, onProgress)` and
  `zipPatches(patches, fetchBytes, onProgress)`. They know nothing of React and return what was
  copied, skipped and failed.
- `ui/useCopyToFolder.ts`: the folder and permission steps, the running state and the result. The
  three places that copy share it, along with one `CopyResult` display.
- `useLocalLibrary` gains `writableFolder()`, which returns a store it may write to or null, and
  `add(paths)`, which reads new files into the list.
- `ui/PatchRow.tsx`: the shared row.

## Testing

- Unit tests for `copyPatches`: a new file, an identical file, a name clash, a `-2` that is
  identical, a failed patch partway through, and refused permission.
- A unit test that `zipPatches` keeps the layout.
- e2e: auditioning on the pack page, copying from each of the three places, and the zip path.
- The existing e2e checks of the pack page's Play button change to the preview button.

Auditioning from a pack uses the same transfer as the Patches tab, and the owner tries it on the
OP-1 on the dev site.
