# File management on every patch page

Agreed with the owner on 2026-10-30. Built on `dev`, and merged to `main` only when the owner says
so.

## Goal

People tick several patches and move, copy or delete them on any page that manages patches, with
the same tools for the OP-1 and for their own folder.

Today the OP-1 Library has move, copy and delete for one file at a time, and its checkboxes only
feed Back up selected. My folder has no file tools at all.

## Where the tools appear

| Page | Bulk tools | Per-row tools |
| --- | --- | --- |
| Library, OP-1 folders | Move, Copy, Delete, Copy to My folder | Unchanged |
| Library, My folder group | Move, Copy, Delete, Send to OP-1 | Rename, Set name, Move, Copy, Delete |
| Patches tab, My folder | Move, Copy, Delete | Rename, Set name, Move, Copy, Delete |
| Patches and Packs, Community | Copy to My folder | None |

Community files are read-only, so they only get the copy they already have.

A toolbar appears once a row is ticked: the count, the actions and Clear. Only ticked rows that are
still shown count.

## Actions

- **Delete:** a dialog lists the file names, up to 20 and then "and n more", and says this cannot
  be undone. A web page cannot use the Recycle Bin, so delete is permanent everywhere.
- **Move and Copy:** a dialog lists existing folders and has a box for a new folder. On the OP-1, a
  mix of synth and drum gets one picker per kind, because each kind stays under its own root.
- **Name clashes:** both take a free name such as `name-2.aif`. Nothing is overwritten.
- **Failures:** files are handled in turn and a failure does not stop the rest. The result reads
  like `Moved 18, 2 failed`, and names each failure and its reason.

## Between the OP-1 and My folder

Copy only, so a transfer never removes its source. Send to OP-1 stays as it is.

Copy to My folder from the OP-1 picks a destination, by default `op-1/synth` and `op-1/drum`.
Identical files are skipped, as for community copies. Files are read through the OP-1's queue.

## My folder

- Rename, set patch name, move, copy and delete use the core operations the OP-1 uses.
- New folder and remove folder sit in the Library's left column. Folders may hold folders here,
  so removing one includes them, after a confirmation with the patch count.
- Write permission is asked for on the first change, as Copy to My folder does.
- Lists update without a full rescan: removed paths go and new paths are read in.

## OP-1 rules

- A read-only dropped drive stages changes in the changes zip, like every other write.
- The `user` preset folder always stays, as it does today.
- Deleting or moving a patch does not change what is playing on the OP-1, so an audition continues
  and Put my sound back still works. A browser preview of a removed file closes.

## Units

- `core/library/bulk.ts`: runs one operation over many paths and collects what worked and failed.
- `ui/places.ts`: the OP-1 and My folder as one interface of folders and operations.
- `ui/BulkBar.tsx`, `ui/BulkDialogs.tsx`: the shared toolbar and dialogs.
- `ui/PatchActions.tsx`: per-row actions that work with either place.

## Testing

- Unit tests for a failure partway through, name clashes and removing nested folders.
- e2e for each page in the table above, and for a staged drive.
