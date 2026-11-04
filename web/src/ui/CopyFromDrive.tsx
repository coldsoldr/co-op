import { useState } from 'react';
import { copyFiles, type CopyResult } from '../core/library/copy-to-folder';
import { ensureFolder } from '../core/library/operations';
import { basename, joinPath } from '../core/store/store';
import { messageOf } from '../core/util';
import { BulkFolderDialog, type FolderTarget } from './BulkTools';
import { CopyOutcome } from './CopyOutcome';
import type { BulkGroup, Place } from './places';
import type { CopyState } from './useCopyToFolder';
import type { Device } from './useDevice';
import type { LocalLibrary } from './useLocalLibrary';

interface CopyFromDriveProps {
  device: Device;
  local: LocalLibrary;
  place: Place;
  paths: readonly string[];
}

/** Saves ticked OP-1 patches to My folder. The patches stay on the OP-1. */
export function CopyFromDrive({ device, local, place, paths }: CopyFromDriveProps) {
  const [choosing, setChoosing] = useState(false);
  const [progress, setProgress] = useState<{ done: number; total: number } | null>(null);
  const [outcome, setOutcome] = useState<CopyState | null>(null);
  const groups: BulkGroup[] = place.groups(paths).map((group) => ({
    ...group,
    root: '',
    folders: [...new Set([`op-1/${group.root}`, '', ...local.folders])],
  }));

  async function copy(targets: ReadonlyMap<string, FolderTarget>): Promise<void> {
    setChoosing(false);
    setOutcome(null);
    const drive = device.source?.store;
    if (!drive) return;
    try {
      const folder = await local.writableFolder();
      if ('refused' in folder) {
        if (folder.refused === 'denied') {
          setOutcome({ kind: 'error', message: 'Nothing was copied: the browser was not allowed to write to the folder.' });
        }
        return;
      }
      setProgress({ done: 0, total: paths.length });
      const items = [];
      for (const group of groups) {
        const target = targets.get(group.key) as FolderTarget;
        const into = target.isNew ? await ensureFolder(folder.store, '', target.name) : target.name;
        for (const path of group.paths) {
          items.push({ path: joinPath(into, basename(path)), read: () => device.exclusive(() => drive.read(path)) });
        }
      }
      const result: CopyResult = await copyFiles(folder.store, items, (done, total) => setProgress({ done, total }));
      await local.add(folder.store, result.copied);
      setOutcome({ kind: 'result', zip: false, result });
    } catch (e) {
      setOutcome({ kind: 'error', message: messageOf(e) });
    } finally {
      setProgress(null);
    }
  }

  return (
    <>
      <button type="button" onClick={() => setChoosing(true)} disabled={progress !== null || place.busy}>
        {progress ? `Copying ${progress.done} of ${progress.total}` : `Copy to My folder (${paths.length})`}
      </button>
      <CopyOutcome outcome={outcome} />
      {choosing && (
        <BulkFolderDialog
          title={`Copy ${paths.length} to My folder`}
          groups={groups}
          submitLabel="Copy"
          onCancel={() => setChoosing(false)}
          onSubmit={(targets) => void copy(targets)}
        />
      )}
    </>
  );
}
