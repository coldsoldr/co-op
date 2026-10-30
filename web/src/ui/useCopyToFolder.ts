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
