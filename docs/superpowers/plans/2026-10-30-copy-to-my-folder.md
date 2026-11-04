# Pack auditions and Copy to My folder Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Patches in a pack audition like the Patches tab, and community patches and packs can be copied to the person's local folder, or downloaded as a zip where a browser cannot open a folder.

**Architecture:** A pure `copy-to-folder` core module does the copying. `useCopyToFolder` adds folder permission, progress and the result, and three places in the UI share it. A shared `PatchRow` replaces the Patches tab's row and the pack page's table row.

**Tech Stack:** React, TypeScript, Vitest, Playwright, fflate, File System Access API.

**Spec:** `docs/superpowers/specs/2026-10-30-copy-to-my-folder-design.md`

**Rules for this repo:** Work on `dev`. Commits are by `coldsoldr` only, with no Co-Authored-By or any other attribution line. Run commands from the repository root.

---

## Files

| File | Change |
| --- | --- |
| `web/src/core/library/copy-to-folder.ts` | Create: `copyPatches`, `zipPatches` |
| `web/src/core/library/copy-to-folder.test.ts` | Create |
| `web/src/core/library/local-library.ts` | Export `readLocalPatch`, add `packPatches` |
| `web/src/ui/useLocalLibrary.ts` | Add `writableFolder`, `add` |
| `web/src/ui/useCopyToFolder.ts` | Create |
| `web/src/ui/CopyOutcome.tsx` | Create |
| `web/src/ui/PatchRow.tsx` | Create |
| `web/src/ui/PatchesTab.tsx` | Use `PatchRow`, add selection and copy |
| `web/src/ui/Packs.tsx` | Use `PatchRow`, add audition and copy |
| `web/src/App.tsx` | Pass `local` and `audition` to `Packs` |
| `web/e2e/copy.spec.ts` | Create |

---

### Task 1: Core copy and zip

**Files:**
- Create: `web/src/core/library/copy-to-folder.ts`
- Test: `web/src/core/library/copy-to-folder.test.ts`

- [ ] **Step 1: Write the failing test**

```ts
import { describe, expect, it } from 'vitest';
import { unzipSync } from 'fflate';
import { MemoryStore } from '../store/memory-store';
import { sha256Hex } from '../util';
import { copyPatches, zipPatches, type CopyPatch } from './copy-to-folder';

const one = Uint8Array.of(1, 2, 3);
const two = Uint8Array.of(4, 5, 6);

const item = async (path: string, bytes: Uint8Array): Promise<CopyPatch> => ({ path, pack: { sha256: await sha256Hex(bytes) } });
const fetchOf = (bytes: Uint8Array) => async () => bytes;

describe('copyPatches', () => {
  it('writes a new patch at its pack path', async () => {
    const store = new MemoryStore({});
    const result = await copyPatches(store, [await item('kit/drum/a.aif', one)], fetchOf(one));
    expect(result).toEqual({ copied: ['kit/drum/a.aif'], already: [], failed: [] });
    expect(await store.read('kit/drum/a.aif')).toEqual(one);
  });

  it('skips a patch that is already there', async () => {
    const store = new MemoryStore({ 'kit/drum/a.aif': one });
    const result = await copyPatches(store, [await item('kit/drum/a.aif', one)], fetchOf(one));
    expect(result).toEqual({ copied: [], already: ['kit/drum/a.aif'], failed: [] });
  });

  it('keeps both when a different file has the name', async () => {
    const store = new MemoryStore({ 'kit/drum/a.aif': two });
    const result = await copyPatches(store, [await item('kit/drum/a.aif', one)], fetchOf(one));
    expect(result.copied).toEqual(['kit/drum/a-2.aif']);
    expect(await store.read('kit/drum/a.aif')).toEqual(two);
    expect(await store.read('kit/drum/a-2.aif')).toEqual(one);
  });

  it('finds an identical copy under a numbered name', async () => {
    const store = new MemoryStore({ 'kit/drum/a.aif': two, 'kit/drum/a-2.aif': one });
    const result = await copyPatches(store, [await item('kit/drum/a.aif', one)], fetchOf(one));
    expect(result).toEqual({ copied: [], already: ['kit/drum/a.aif'], failed: [] });
  });

  it('carries on after a failed patch and reports it', async () => {
    const store = new MemoryStore({});
    const patches = [await item('kit/drum/bad.aif', one), await item('kit/drum/good.aif', two)];
    const seen: number[] = [];
    const result = await copyPatches(
      store,
      patches,
      async (patch) => {
        if (patch.path.endsWith('bad.aif')) throw new Error('no network');
        return two;
      },
      (done) => seen.push(done),
    );
    expect(result.copied).toEqual(['kit/drum/good.aif']);
    expect(result.failed).toEqual([{ path: 'kit/drum/bad.aif', message: 'no network' }]);
    expect(seen).toEqual([1, 2]);
  });
});

describe('zipPatches', () => {
  it('keeps the layout and reports failures', async () => {
    const patches = [await item('kit/drum/a.aif', one), await item('kit/drum/bad.aif', two)];
    const { zip, result } = await zipPatches(patches, async (patch) => {
      if (patch.path.endsWith('bad.aif')) throw new Error('no network');
      return one;
    });
    expect(Object.keys(unzipSync(zip))).toEqual(['kit/drum/a.aif']);
    expect(result).toEqual({ copied: ['kit/drum/a.aif'], already: [], failed: [{ path: 'kit/drum/bad.aif', message: 'no network' }] });
  });
});
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pnpm --filter @co-op/web exec vitest run src/core/library/copy-to-folder.test.ts`
Expected: FAIL, cannot resolve `./copy-to-folder`.

- [ ] **Step 3: Write the implementation**

```ts
import { zipSync, type Zippable } from 'fflate';
import { writeAtomic } from '../store/atomic';
import { basename, dirname, joinPath, type DeviceStore } from '../store/store';
import { messageOf, sha256Hex } from '../util';

/** What copying needs to know of a community patch. `path` is `<pack slug>/<file in the pack>`. */
export interface CopyPatch {
  path: string;
  pack: { sha256: string };
}

export interface CopyResult {
  /** Where each new file was written. */
  copied: string[];
  /** Patches that were in the folder already. */
  already: string[];
  failed: { path: string; message: string }[];
}

type Progress = (done: number, total: number) => void;

/** The name to write `path` under, or null when a file with this hash is already there. */
async function targetFor(store: DeviceStore, path: string, sha256: string): Promise<string | null> {
  const folder = dirname(path);
  const name = basename(path);
  const dot = name.lastIndexOf('.');
  for (let n = 1; ; n++) {
    const candidate = n === 1 ? path : joinPath(folder, `${name.slice(0, dot)}-${n}${name.slice(dot)}`);
    if (!(await store.exists(candidate))) return candidate;
    if ((await sha256Hex(await store.read(candidate))) === sha256.toLowerCase()) return null;
  }
}

/** Copies patches into a folder in turn. Nothing is overwritten, and a failure does not stop the rest. */
export async function copyPatches<T extends CopyPatch>(
  store: DeviceStore,
  patches: readonly T[],
  fetchBytes: (patch: T) => Promise<Uint8Array>,
  onProgress?: Progress,
): Promise<CopyResult> {
  const result: CopyResult = { copied: [], already: [], failed: [] };
  let done = 0;
  for (const patch of patches) {
    try {
      const target = await targetFor(store, patch.path, patch.pack.sha256);
      if (target === null) {
        result.already.push(patch.path);
      } else {
        await writeAtomic(store, target, await fetchBytes(patch));
        result.copied.push(target);
      }
    } catch (error) {
      result.failed.push({ path: patch.path, message: messageOf(error) });
    }
    onProgress?.(++done, patches.length);
  }
  return result;
}

/** The same copy as a zip, for browsers that cannot open a folder. Audio is stored, not compressed. */
export async function zipPatches<T extends CopyPatch>(
  patches: readonly T[],
  fetchBytes: (patch: T) => Promise<Uint8Array>,
  onProgress?: Progress,
): Promise<{ zip: Uint8Array; result: CopyResult }> {
  const result: CopyResult = { copied: [], already: [], failed: [] };
  const files: Zippable = {};
  let done = 0;
  for (const patch of patches) {
    try {
      files[patch.path] = [await fetchBytes(patch), { level: 0 }];
      result.copied.push(patch.path);
    } catch (error) {
      result.failed.push({ path: patch.path, message: messageOf(error) });
    }
    onProgress?.(++done, patches.length);
  }
  return { zip: zipSync(files), result };
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `pnpm --filter @co-op/web exec vitest run src/core/library/copy-to-folder.test.ts`
Expected: PASS, 6 tests.

- [ ] **Step 5: Commit**

```bash
git add web/src/core/library/copy-to-folder.ts web/src/core/library/copy-to-folder.test.ts
git commit -m "feat(library): copy community patches to a folder or a zip"
```

### Task 2: Local library writes

**Files:**
- Modify: `web/src/core/library/local-library.ts`
- Modify: `web/src/ui/useLocalLibrary.ts`
- Test: `web/src/core/library/local-library.test.ts`

- [ ] **Step 1: Write the failing test**

Add `packPatches` to the import in `local-library.test.ts`, and append:

```ts
describe('packPatches', () => {
  it('lists one pack as community patches', () => {
    const pack = {
      slug: 'kit',
      name: 'Kit',
      patches: [{ file: 'drum/a.aif', name: 'a', kind: 'drum', engine: 'drum', sizeBytes: 3, sha256: 'ab' }],
    } as unknown as Parameters<typeof packPatches>[0];
    expect(packPatches(pack)).toMatchObject([{ path: 'kit/drum/a.aif', folder: 'Kit', pack: { slug: 'kit', file: 'drum/a.aif', sha256: 'ab' } }]);
  });
});
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pnpm --filter @co-op/web exec vitest run src/core/library/local-library.test.ts`
Expected: FAIL, `packPatches` is not a function.

- [ ] **Step 3: Change `local-library.ts`**

Change the import on line 1 to `import type { PackIndex, PackIndexEntry } from '@co-op/shared/packs';`, change `async function readLocalPatch` to `export async function readLocalPatch`, and replace `communityPatches` with:

```ts
/** One pack's patches, in the form the patch lists show. */
export function packPatches(pack: PackIndexEntry): LocalPatch[] {
  return pack.patches.map((patch) => ({
    path: `${pack.slug}/${patch.file}`,
    fileName: basename(patch.file),
    folder: pack.name,
    name: patch.name || stem(basename(patch.file)),
    kind: patch.kind,
    engine: patch.engine,
    fx: null,
    sizeBytes: patch.sizeBytes,
    error: null,
    preview: null,
    pack: { slug: pack.slug, file: patch.file, sha256: patch.sha256 },
  }));
}

/** Every patch of every community pack, as one flat list. */
export function communityPatches(index: PackIndex): LocalPatch[] {
  return index.packs.flatMap(packPatches);
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `pnpm --filter @co-op/web exec vitest run src/core/library/local-library.test.ts`
Expected: PASS.

- [ ] **Step 5: Change `useLocalLibrary.ts`**

Change the first local import to `import { readLocalPatch, scanLocalFolder, type LocalPatch } from '../core/library/local-library';`.

Add before `export interface LocalLibrary`:

```ts
/** A folder that may be written to, or why there is none. */
export type WritableFolder = { store: FsAccessStore } | { refused: 'cancelled' | 'denied' };
```

Add to the `LocalLibrary` interface:

```ts
  /** The folder with write permission, choosing or reconnecting it first. Needs a click. */
  writableFolder(): Promise<WritableFolder>;
  /** Reads files that are new in the folder into the list. */
  add(store: FsAccessStore, paths: readonly string[]): Promise<void>;
```

Add before the `return` of `useLocalLibrary`:

```ts
  const writableFolder = useCallback(async (): Promise<WritableFolder> => {
    setError(null);
    const handle = folder?.root ?? remembered;
    if (handle) {
      if (!(await ensurePermission(handle, 'readwrite'))) return { refused: 'denied' };
      if (folder) return { store: folder };
      const store = new FsAccessStore(handle);
      await open(store);
      return { store };
    }
    const store = await chooseLocalFolder();
    if (!store) return { refused: 'cancelled' };
    if (!(await ensurePermission(store.root, 'readwrite'))) return { refused: 'denied' };
    await rememberHandle(store.root, LOCAL_KEY);
    await open(store);
    return { store };
  }, [folder, open, remembered]);

  // Takes the store because writableFolder may have just opened it, after this render.
  const add = useCallback(async (store: FsAccessStore, paths: readonly string[]) => {
    const read = await Promise.all(paths.map((path) => readLocalPatch(store, path)));
    setPatches((prev) => [...prev, ...read]);
  }, []);
```

Change the return to include both:

```ts
  return { supported, folder, remembered, patches, progress, error, choose, reconnect, refresh, forget, writableFolder, add };
```

- [ ] **Step 6: Typecheck**

Run: `pnpm typecheck`
Expected: Done with no errors. If `store.root` is private, confirm: `chooseLocalFolder`'s caller at `useLocalLibrary.ts:84` already reads it.

- [ ] **Step 7: Commit**

```bash
git add web/src/core/library/local-library.ts web/src/core/library/local-library.test.ts web/src/ui/useLocalLibrary.ts
git commit -m "feat(library): ask for write access to the local folder when needed"
```

### Task 3: The copy hook and its result

**Files:**
- Create: `web/src/ui/useCopyToFolder.ts`
- Create: `web/src/ui/CopyOutcome.tsx`

- [ ] **Step 1: Create `useCopyToFolder.ts`**

```ts
import { useState } from 'react';
import { fetchPatchBytes } from '../core/catalogue/catalogue';
import { copyPatches, zipPatches, type CopyResult } from '../core/library/copy-to-folder';
import type { LocalPatch } from '../core/library/local-library';
import { messageOf } from '../core/util';
import { saveBlob, zipBlob } from './download';
import type { LocalLibrary } from './useLocalLibrary';

/** A community patch: it has a pack to download from. */
type Community = LocalPatch & { pack: NonNullable<LocalPatch['pack']> };

const isCommunity = (patch: LocalPatch): patch is Community => patch.pack !== undefined;
const download = (patch: Community): Promise<Uint8Array> =>
  fetchPatchBytes(import.meta.env.BASE_URL, patch.pack.slug, patch.pack.file, patch.pack.sha256);

export type CopyState = { kind: 'result'; zip: boolean; result: CopyResult } | { kind: 'error'; message: string };

export interface CopyToFolder {
  busy: boolean;
  outcome: CopyState | null;
  /** The button's text for `n` patches. */
  label(n: number): string;
  copy(patches: readonly LocalPatch[]): Promise<void>;
}

/** Copies community patches to the local folder, or into a zip where a browser cannot open one. */
export function useCopyToFolder(local: LocalLibrary): CopyToFolder {
  const [progress, setProgress] = useState<{ done: number; total: number } | null>(null);
  const [outcome, setOutcome] = useState<CopyState | null>(null);
  const onProgress = (done: number, total: number): void => setProgress({ done, total });

  async function copy(patches: readonly LocalPatch[]): Promise<void> {
    const community = patches.filter(isCommunity);
    if (progress !== null || community.length === 0) return;
    setOutcome(null);
    try {
      if (!local.supported) {
        setProgress({ done: 0, total: community.length });
        const { zip, result } = await zipPatches(community, download, onProgress);
        if (result.copied.length > 0) saveBlob(zipBlob(zip), 'co-op-patches.zip');
        setOutcome({ kind: 'result', zip: true, result });
        return;
      }
      const folder = await local.writableFolder();
      if ('refused' in folder) {
        if (folder.refused === 'denied') {
          setOutcome({ kind: 'error', message: 'Nothing was copied: the browser was not allowed to write to the folder.' });
        }
        return;
      }
      setProgress({ done: 0, total: community.length });
      const result = await copyPatches(folder.store, community, download, onProgress);
      await local.add(folder.store, result.copied);
      setOutcome({ kind: 'result', zip: false, result });
    } catch (e) {
      setOutcome({ kind: 'error', message: messageOf(e) });
    } finally {
      setProgress(null);
    }
  }

  function label(n: number): string {
    if (progress) return `Copying ${progress.done} of ${progress.total}`;
    return local.supported ? `Copy to My folder (${n})` : `Download zip (${n})`;
  }

  return { busy: progress !== null, outcome, label, copy };
}
```

- [ ] **Step 2: Create `CopyOutcome.tsx`**

```tsx
import type { CopyState } from './useCopyToFolder';

const summary = (copied: number, already: number, zip: boolean): string =>
  [`${zip ? 'Added' : 'Copied'} ${copied}${zip ? ' to the zip' : ''}`, ...(already > 0 ? [`${already} already there`] : [])].join(', ');

/** What the last copy did. */
export function CopyOutcome({ outcome }: { outcome: CopyState | null }) {
  if (!outcome) return null;
  if (outcome.kind === 'error') {
    return (
      <p role="alert" className="error">
        {outcome.message}
      </p>
    );
  }
  const { copied, already, failed } = outcome.result;
  return (
    <div role={failed.length > 0 ? 'alert' : 'status'} className={failed.length > 0 ? 'error' : 'notice'}>
      <p>{summary(copied.length, already.length, outcome.zip)}.</p>
      {failed.length > 0 && (
        <ul className="file-list">
          {failed.map((f) => (
            <li key={f.path}>
              <code>{f.path}</code>: {f.message}
            </li>
          ))}
        </ul>
      )}
    </div>
  );
}
```

- [ ] **Step 3: Typecheck**

Run: `pnpm typecheck`
Expected: Done with no errors.

- [ ] **Step 4: Commit**

```bash
git add web/src/ui/useCopyToFolder.ts web/src/ui/CopyOutcome.tsx
git commit -m "feat(ui): add the shared copy hook and result"
```

### Task 4: Shared patch row

**Files:**
- Create: `web/src/ui/PatchRow.tsx`
- Modify: `web/src/ui/PatchesTab.tsx`

- [ ] **Step 1: Create `PatchRow.tsx`**

```tsx
import type { LocalPatch } from '../core/library/local-library';
import { clickedControl, transferRow } from './AuditionBar';
import { formatSize } from './format';
import type { Audition } from './useAudition';
import type { usePatchPreview } from './usePatchPreview';

/** Worked out from the list, before a patch is read. */
const playsOnOp1 = (patch: LocalPatch): boolean => (patch.kind === 'synth' || patch.engine === 'drum' || patch.engine === 'dbox') && patch.error === null;

interface PatchRowProps {
  patch: LocalPatch;
  audition: Audition;
  preview: ReturnType<typeof usePatchPreview>;
  load: () => Promise<Uint8Array>;
  /** The text under the name; the patch's path unless given. */
  file?: string;
  notes?: string;
  /** Shows a checkbox cell first. */
  select?: { label: string; checked: boolean; onChange: (on: boolean) => void };
}

/** One patch in a list: click to audition on the OP-1, or preview it in the browser. */
export function PatchRow({ patch, audition, preview, load, file, notes, select }: PatchRowProps) {
  const onOp1 = audition.ready && playsOnOp1(patch);
  const playing = preview.playing === patch.path;
  const noPreview = patch.preview === false;
  const play = (): void => void audition.play(patch.path, patch.name, { load });
  return (
    <tr
      key={patch.path}
      data-path={patch.path}
      {...(onOp1 ? transferRow(audition, patch.path) : {})}
      onClick={(e) => {
        if (onOp1 && !clickedControl(e.target)) play();
      }}
    >
      {select && (
        <td className="select">
          <input type="checkbox" aria-label={select.label} checked={select.checked} onChange={(e) => select.onChange(e.target.checked)} />
        </td>
      )}
      <th scope="row" className="name">
        {onOp1 ? (
          <button type="button" className="row-pick" onClick={play} aria-label={`Audition ${patch.name}`}>
            {patch.name}
          </button>
        ) : (
          patch.name
        )}
        <code className="file">{file ?? patch.path}</code>
        {(patch.engine || patch.fx) && <span className="sound">{[patch.engine, patch.fx].filter(Boolean).join(' · ')}</span>}
        {noPreview && <span className="sound">no preview</span>}
        {patch.error && <span className="row-error">{patch.error}</span>}
        {notes && <span className="sound">{notes}</span>}
      </th>
      <td className="kind">{patch.kind}</td>
      <td className="size">{patch.error ? '' : formatSize(patch.sizeBytes)}</td>
      <td className="play">
        <button
          type="button"
          className={`play-key ${patch.kind}`}
          disabled={noPreview || patch.error !== null}
          title={noPreview ? 'No preview: the OP-1 only stored its voice clip' : 'Preview in the browser'}
          aria-label={`${playing ? 'Stop' : 'Preview'} ${patch.name}`}
          aria-pressed={playing}
          onClick={() => void preview.toggle(patch.path, patch.name, load)}
        >
          <svg aria-hidden="true" viewBox="0 0 24 24">
            <path d={playing ? 'M6 5h12v14H6z' : 'M7 4l13 8-13 8z'} />
          </svg>
        </button>
      </td>
    </tr>
  );
}
```

If `clickedControl` does not treat a checkbox as a control, clicking one would audition: confirm it in `AuditionBar.tsx`, and add `input` to its selector if it is missing.

- [ ] **Step 2: Use it in `PatchesTab.tsx`**

Remove the `playsOnOp1` constant, the `auditionPatch` function, and the `clickedControl, transferRow` and `formatSize` imports (keep `AuditionBar`). Add `import { PatchRow } from './PatchRow';`. Replace the `<tbody>` contents (lines 242-287) with:

```tsx
{shown.map((patch) => (
  <PatchRow key={patch.path} patch={patch} audition={audition} preview={preview} load={() => bytesOf(patch)} />
))}
```

- [ ] **Step 3: Check nothing changed**

Run: `pnpm typecheck && pnpm --filter @co-op/web e2e e2e/plugins.spec.ts`
Expected: both pass.

- [ ] **Step 4: Commit**

```bash
git add web/src/ui/PatchRow.tsx web/src/ui/PatchesTab.tsx web/src/ui/AuditionBar.tsx
git commit -m "refactor(ui): share the patch row"
```

### Task 5: Select and copy on the Patches tab

**Files:**
- Modify: `web/src/ui/PatchesTab.tsx`

- [ ] **Step 1: Add selection and the hook**

Add imports:

```ts
import { CopyOutcome } from './CopyOutcome';
import { useCopyToFolder } from './useCopyToFolder';
```

After `const preview = usePatchPreview(active);` add:

```ts
  const copier = useCopyToFolder(local);
  const [selected, setSelected] = useState<ReadonlySet<string>>(() => new Set());
```

In `pickSource`, add `setSelected(new Set());` after `setFilter({});`.

After `const shown = filterLocal(patches, filter);` add:

```ts
  const copying = source === 'community';
  const chosen = shown.filter((p) => selected.has(p.path));
  const allOn = shown.length > 0 && chosen.length === shown.length;

  function toggle(path: string, on: boolean): void {
    const next = new Set(selected);
    if (on) next.add(path);
    else next.delete(path);
    setSelected(next);
  }

  async function copyChosen(): Promise<void> {
    await copier.copy(chosen);
    setSelected(new Set());
  }
```

- [ ] **Step 2: Change the page text**

Replace "Nothing needs to be plugged in, and co-op only reads the folder." with "Nothing needs to be plugged in, and co-op writes to the folder only when you copy patches into it."

- [ ] **Step 3: Add the button**

After the `<p className="muted" role="status">` block add:

```tsx
{copying && (
  <p>
    <button type="button" className="primary" onClick={() => void copyChosen()} disabled={copier.busy || chosen.length === 0}>
      {copier.label(chosen.length)}
    </button>
  </p>
)}
<CopyOutcome outcome={copier.outcome} />
```

- [ ] **Step 4: Add the checkboxes**

In `<thead>`, before the Name column:

```tsx
{copying && (
  <th scope="col" className="select">
    <input
      type="checkbox"
      aria-label="Select all shown"
      checked={allOn}
      onChange={(e) => setSelected(new Set(e.target.checked ? shown.map((p) => p.path) : []))}
    />
  </th>
)}
```

Add this prop to `PatchRow`:

```tsx
{...(copying ? { select: { label: `Select ${patch.name}`, checked: selected.has(patch.path), onChange: (on: boolean) => toggle(patch.path, on) } } : {})}
```

- [ ] **Step 5: Typecheck**

Run: `pnpm typecheck`
Expected: Done with no errors.

- [ ] **Step 6: Commit**

```bash
git add web/src/ui/PatchesTab.tsx
git commit -m "feat(ui): copy community patches to My folder from the Patches tab"
```

### Task 6: Audition and copy on the Packs tab

**Files:**
- Modify: `web/src/ui/Packs.tsx`
- Modify: `web/src/App.tsx:154`

- [ ] **Step 1: Pass the props in `App.tsx`**

```tsx
<Packs device={device} local={local} audition={audition} active={tab === 'packs'} onOpenLibrary={openLibrary} />
```

- [ ] **Step 2: Change the imports and props in `Packs.tsx`**

Remove the imports of `decodePcm, readPatch` and `playPcm`. Add:

```ts
import { packPatches } from '../core/library/local-library';
import { AuditionBar } from './AuditionBar';
import { CopyOutcome } from './CopyOutcome';
import { PatchKeyboard } from './PatchKeyboard';
import { PatchRow } from './PatchRow';
import type { Audition } from './useAudition';
import { useCopyToFolder, type CopyToFolder } from './useCopyToFolder';
import type { LocalLibrary } from './useLocalLibrary';
import { usePatchPreview } from './usePatchPreview';
```

Add to `PacksProps`:

```ts
  local: LocalLibrary;
  audition: Audition;
```

Change the component to `export function Packs({ device, local, audition, active, onOpenLibrary }: PacksProps)`, and add as its first line:

```ts
  const copier = useCopyToFolder(local);
```

- [ ] **Step 3: Pass them down**

In `Packs`, give `PackDetail` the props `audition={audition}` and `copier={copier}`, give `PackList` the prop `copier={copier}`, and add `copier: CopyToFolder;` to `PackListProps`. In `PackDetailProps` add `audition: Audition;` and `copier: CopyToFolder;`. Add both to `PackDetail`'s and `copier` to `PackList`'s destructuring.

- [ ] **Step 4: Copy from the card**

In `PackList`, add `<CopyOutcome outcome={copier.outcome} />` above `<div className="pack-grid">`, and pass `copier={copier}` to `PackCard`. Change `PackCard`'s props to include `copier: CopyToFolder`, and add after the View button:

```tsx
<button type="button" aria-label={`Copy ${pack.name} to My folder`} disabled={copier.busy} onClick={() => void copier.copy(packPatches(pack))}>
  {copier.label(pack.patches.length)}
</button>
```

- [ ] **Step 5: Replace play code in `PackDetail`**

Delete the state `playing`, `playError`, `stopRef`, `playingRef`, `playToken`, `activeRef`, both effects that use them (lines 283-296, keeping the heading focus effect), and the `play` function. Add:

```ts
  const preview = usePatchPreview(active);
  const patches = useMemo(() => packPatches(pack), [pack]);
  const notes = useMemo(() => new Map(pack.patches.map((p) => [p.file, p.notes])), [pack]);
  const chosenPatches = patches.filter((p) => p.pack && selected.has(p.pack.file));
  const loadOf = (patch: (typeof patches)[number]) => () => {
    const { slug, file, sha256 } = patch.pack as NonNullable<typeof patch.pack>;
    return fetchPatchBytes(base(), slug, file, sha256);
  };
```

- [ ] **Step 6: Replace the table**

Add `<AuditionBar audition={audition} />` directly before the table, and replace everything from `<table className="library pack">` through the `playError` block with:

```tsx
{preview.error && (
  <p role="alert" className="error">
    {preview.error}
  </p>
)}
{preview.open && active && (
  <PatchKeyboard name={preview.open.name} player={preview.open.player} kit={preview.open.kit} onClose={preview.close} />
)}
<table className="library pack patches">
  <caption>Patches in {pack.name}</caption>
  <thead>
    <tr>
      <th scope="col" className="select">
        <span className="visually-hidden">Select</span>
      </th>
      <th scope="col">Name</th>
      <th scope="col">Kind</th>
      <th scope="col">Size</th>
      <th scope="col" className="play">
        <span className="visually-hidden">Preview</span>
      </th>
    </tr>
  </thead>
  <tbody>
    {patches.map((patch) => {
      const file = patch.pack?.file ?? patch.path;
      const note = notes.get(file);
      return (
        <PatchRow
          key={patch.path}
          patch={patch}
          audition={audition}
          preview={preview}
          load={loadOf(patch)}
          file={file}
          {...(note ? { notes: note } : {})}
          select={{
            label: `Select ${file}`,
            checked: selected.has(file),
            onChange: (on) => {
              const next = new Set(selected);
              if (on) next.add(file);
              else next.delete(file);
              setSelected(next);
            },
          }}
        />
      );
    })}
  </tbody>
</table>
```

The file column moves under the name, as in the Patches tab, because the shared row has one place for it.

- [ ] **Step 7: Add the copy buttons**

Before `<div className="panel">` add:

```tsx
<div className="panel">
  <h4>Copy to My folder</h4>
  <p>
    <button type="button" aria-label="Copy selected to My folder" onClick={() => void copier.copy(chosenPatches)} disabled={copier.busy || chosenPatches.length === 0}>
      {copier.label(chosenPatches.length)}
    </button>{' '}
    <button type="button" aria-label="Copy all to My folder" className="primary" onClick={() => void copier.copy(patches)} disabled={copier.busy}>
      {copier.label(patches.length)}
    </button>
  </p>
  <CopyOutcome outcome={copier.outcome} />
</div>
```

Keep `CopyOutcome` out of the list view while a pack is open: `PackList` is not rendered then, so only one shows at a time.

- [ ] **Step 8: Run the checks**

Run: `pnpm typecheck && pnpm lint && pnpm --filter @co-op/web e2e e2e/packs.spec.ts`
Expected: all pass. `'Install all (3)'` and `'Select synth/tone-cluster.aif'` keep working because their names did not change; the note text stays inside `table.pack`.

- [ ] **Step 9: Commit**

```bash
git add web/src/ui/Packs.tsx web/src/App.tsx
git commit -m "feat(ui): audition pack patches and copy packs to My folder"
```

### Task 7: End-to-end tests

**Files:**
- Create: `web/e2e/copy.spec.ts`

- [ ] **Step 1: Write the tests**

```ts
import { test, expect, type Page } from '@playwright/test';

/** Makes an empty folder in the browser's private storage the one the folder picker returns. */
async function emptyFolder(page: Page): Promise<void> {
  await page.evaluate(async () => {
    const opfs = await navigator.storage.getDirectory();
    await opfs.removeEntry('mine', { recursive: true }).catch(() => {});
    const folder = await opfs.getDirectoryHandle('mine', { create: true });
    Object.defineProperty(window, 'showDirectoryPicker', { value: async () => folder, configurable: true });
  });
}

const fileExists = (page: Page, path: string): Promise<boolean> =>
  page.evaluate(async (target) => {
    const parts = target.split('/');
    const name = parts.pop() as string;
    let dir = await (await navigator.storage.getDirectory()).getDirectoryHandle('mine');
    try {
      for (const part of parts) dir = await dir.getDirectoryHandle(part);
      await dir.getFileHandle(name);
      return true;
    } catch {
      return false;
    }
  }, path);

test('copies selected community patches from the Patches tab, then skips them', async ({ page }) => {
  await page.goto('./');
  await emptyFolder(page);
  await page.getByRole('tab', { name: 'Patches' }).click();
  await page.getByRole('button', { name: 'Community' }).click();
  const panel = page.locator('#panel-patches');
  await panel.getByRole('checkbox', { name: 'Select all shown' }).check();
  await panel.getByRole('button', { name: 'Copy to My folder (3)' }).click();
  await expect(panel.getByRole('status').filter({ hasText: 'Copied 3' })).toBeVisible();
  expect(await fileExists(page, 'test-tones/synth/tone-cluster.aif')).toBe(true);
  expect(await fileExists(page, 'test-tones/drum/tone-drum.aif')).toBe(true);

  await panel.getByRole('checkbox', { name: 'Select all shown' }).check();
  await panel.getByRole('button', { name: 'Copy to My folder (3)' }).click();
  await expect(panel.getByRole('status').filter({ hasText: 'Copied 0, 3 already there' })).toBeVisible();

  await panel.getByRole('button', { name: 'My folder' }).click();
  await expect(page.locator('#panel-patches tr[data-path]')).toHaveCount(3);
});

test('copies a whole pack from its card', async ({ page }) => {
  await page.goto('./');
  await emptyFolder(page);
  await page.getByRole('tab', { name: 'Packs' }).click();
  await page.getByRole('button', { name: 'Copy Test Tones to My folder' }).click();
  await expect(page.locator('#panel-packs').getByRole('status').filter({ hasText: 'Copied 3' })).toBeVisible();
  expect(await fileExists(page, 'test-tones/synth/tone-sampler.aif')).toBe(true);
});

test('previews and copies from the pack page', async ({ page }) => {
  await page.goto('./');
  await emptyFolder(page);
  await page.getByRole('tab', { name: 'Packs' }).click();
  await page.getByRole('button', { name: 'View Test Tones' }).click();
  await page.locator('tr[data-path="test-tones/synth/tone-sampler.aif"]').getByRole('button', { name: /^Preview/ }).click();
  await expect(page.getByRole('region', { name: /^Keyboard for/ })).toBeVisible();

  await page.getByRole('checkbox', { name: 'Select synth/tone-cluster.aif' }).check();
  await page.getByRole('button', { name: 'Copy selected to My folder' }).click();
  await expect(page.locator('#panel-packs').getByRole('status').filter({ hasText: 'Copied 1' })).toBeVisible();
  expect(await fileExists(page, 'test-tones/synth/tone-cluster.aif')).toBe(true);
  expect(await fileExists(page, 'test-tones/drum/tone-drum.aif')).toBe(false);
});

test('downloads a zip in a browser that cannot open a folder', async ({ page }) => {
  await page.addInitScript(() => {
    delete (window as { showDirectoryPicker?: unknown }).showDirectoryPicker;
    delete (Object.getPrototypeOf(window) as { showDirectoryPicker?: unknown }).showDirectoryPicker;
  });
  await page.goto('./');
  await page.getByRole('tab', { name: 'Packs' }).click();
  const [download] = await Promise.all([page.waitForEvent('download'), page.getByRole('button', { name: 'Copy Test Tones to My folder' }).click()]);
  expect(download.suggestedFilename()).toBe('co-op-patches.zip');
  await expect(page.locator('#panel-packs').getByRole('status').filter({ hasText: 'Added 3 to the zip' })).toBeVisible();
});
```

`showDirectoryPicker` lives on `Window.prototype`, not the instance, so the zip test deletes it from both. If `isFsAccessSupported` still returns true, change the script to `delete (Window.prototype as { showDirectoryPicker?: unknown }).showDirectoryPicker;`.

- [ ] **Step 2: Run the new tests**

Run: `pnpm --filter @co-op/web e2e e2e/copy.spec.ts`
Expected: 4 passed.

- [ ] **Step 3: Run everything**

Run: `pnpm lint && pnpm typecheck && pnpm test && pnpm --filter @co-op/web e2e`
Expected: all pass.

- [ ] **Step 4: Commit**

```bash
git add web/e2e/copy.spec.ts
git commit -m "test(e2e): cover pack previews and Copy to My folder"
```

- [ ] **Step 5: Push to dev**

```bash
git push origin dev
```

Wait for CI, then ask the owner to try it at `https://coldsoldr.github.io/co-op/dev/`, including an audition from a pack on the OP-1.
