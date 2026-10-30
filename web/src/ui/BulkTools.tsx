import { useState, type FormEvent, type ReactNode } from 'react';
import { forEachPath, type BulkResult } from '../core/library/bulk';
import { copyPatch, deletePatch, ensureFolder, movePatch } from '../core/library/operations';
import { joinPath } from '../core/store/store';
import { messageOf } from '../core/util';
import { ConfirmDialog, Dialog } from './Dialog';
import type { BulkGroup, Place } from './places';

const NEW_FOLDER = '\u0000new';
/** The most file names a delete confirmation lists. */
const MAX_LISTED = 20;

const count = (n: number): string => `${n} ${n === 1 ? 'patch' : 'patches'}`;

/** Where one group of patches goes: an existing folder, or a new one to make under its root. */
export interface FolderTarget {
  name: string;
  isNew: boolean;
}

interface BulkFolderDialogProps {
  title: string;
  groups: readonly Pick<BulkGroup, 'key' | 'label' | 'root' | 'folders'>[];
  submitLabel: string;
  onCancel: () => void;
  onSubmit: (targets: ReadonlyMap<string, FolderTarget>) => void;
}

/** Picks a destination for each group, with a box for a new folder. */
export function BulkFolderDialog({ title, groups, submitLabel, onCancel, onSubmit }: BulkFolderDialogProps) {
  const [choices, setChoices] = useState<Record<string, string>>({});
  const [names, setNames] = useState<Record<string, string>>({});
  const choiceOf = (group: BulkFolderDialogProps['groups'][number]): string => choices[group.key] ?? group.folders[0] ?? NEW_FOLDER;

  function submit(event: FormEvent): void {
    event.preventDefault();
    const targets = new Map<string, FolderTarget>();
    for (const group of groups) {
      const choice = choiceOf(group);
      targets.set(group.key, choice === NEW_FOLDER ? { name: (names[group.key] ?? '').trim(), isNew: true } : { name: choice, isNew: false });
    }
    onSubmit(targets);
  }

  return (
    <Dialog title={title} onClose={onCancel}>
      <form onSubmit={submit}>
        {groups.map((group) => (
          <div key={group.key}>
            <label>
              {group.label ? `${group.label} go to` : 'Folder'}
              <select value={choiceOf(group)} onChange={(e) => setChoices({ ...choices, [group.key]: e.target.value })}>
                {group.folders.map((folder) => (
                  <option key={folder} value={folder}>
                    {joinPath(group.root, folder) || 'Top of the folder'}
                  </option>
                ))}
                <option value={NEW_FOLDER}>New folder…</option>
              </select>
            </label>
            {choiceOf(group) === NEW_FOLDER && (
              <label>
                New folder name{group.root ? ` (in ${group.root})` : ''}
                <input type="text" value={names[group.key] ?? ''} onChange={(e) => setNames({ ...names, [group.key]: e.target.value })} required />
              </label>
            )}
          </div>
        ))}
        <div className="dialog-buttons">
          <button type="button" onClick={onCancel}>
            Cancel
          </button>
          <button type="submit" className="primary">
            {submitLabel}
          </button>
        </div>
      </form>
    </Dialog>
  );
}

interface BulkToolsProps {
  place: Place;
  /** The ticked patches that are shown. */
  selected: readonly string[];
  /** Clears the selection. */
  onClear: () => void;
  /** After a change: lets the page clear its selection and stop a preview. */
  onDone: () => void;
  /** Actions only this page has, such as Send to OP-1. */
  children?: ReactNode;
}

type Verb = 'Moved' | 'Copied' | 'Deleted';

/** The toolbar shown once patches are ticked: move, copy and delete many at a time. */
export function BulkTools({ place, selected, onClear, onDone, children }: BulkToolsProps) {
  const [mode, setMode] = useState<'move' | 'copy' | 'delete' | null>(null);
  const [working, setWorking] = useState(false);
  const [outcome, setOutcome] = useState<{ verb: Verb; result: BulkResult } | null>(null);
  const [error, setError] = useState<string | null>(null);
  const groups = place.groups(selected);
  const blocked = working || place.busy;

  async function perform(verb: Verb, op: (run: Parameters<Parameters<Place['run']>[0]>[0]) => Promise<BulkResult>): Promise<void> {
    setMode(null);
    setWorking(true);
    setOutcome(null);
    setError(null);
    let result: BulkResult | null = null;
    try {
      await place.run(async (store) => {
        result = await op(store);
        return result.done;
      }, selected);
      if (result) setOutcome({ verb, result });
      onDone();
    } catch (e) {
      setError(messageOf(e));
    } finally {
      setWorking(false);
    }
  }

  function transfer(move: boolean, targets: ReadonlyMap<string, FolderTarget>): void {
    void perform(move ? 'Moved' : 'Copied', async (store) => {
      const folderFor = new Map<string, string>();
      for (const group of groups) {
        const target = targets.get(group.key) as FolderTarget;
        const folder = target.isNew ? await ensureFolder(store, group.root, target.name) : joinPath(group.root, target.name);
        for (const path of group.paths) folderFor.set(path, folder);
      }
      return forEachPath(selected, (path) => (move ? movePatch : copyPatch)(store, path, folderFor.get(path) as string));
    });
  }

  function remove(): void {
    void perform('Deleted', (store) =>
      forEachPath(selected, async (path) => {
        await deletePatch(store, path);
        return path;
      }),
    );
  }

  const listed = selected.length > MAX_LISTED ? [...selected.slice(0, MAX_LISTED), `and ${selected.length - MAX_LISTED} more`] : [...selected];

  return (
    <>
      {selected.length > 0 && (
        <div className="plugin-buttons local-actions" role="group" aria-label="Selected patches">
          <span className="muted">{selected.length} selected</span>
          <button type="button" onClick={() => setMode('move')} disabled={blocked}>
            Move
          </button>
          <button type="button" onClick={() => setMode('copy')} disabled={blocked}>
            Copy
          </button>
          <button type="button" className="danger" onClick={() => setMode('delete')} disabled={blocked}>
            Delete
          </button>
          {children}
          <button type="button" onClick={onClear} disabled={working}>
            Clear
          </button>
        </div>
      )}
      {working && <p role="status">Working…</p>}
      {error && (
        <p role="alert" className="error">
          {error}
        </p>
      )}
      {outcome && (
        <div role={outcome.result.failed.length > 0 ? 'alert' : 'status'} className={outcome.result.failed.length > 0 ? 'error' : 'notice'}>
          <p>
            {outcome.verb} {count(outcome.result.done.length)}
            {outcome.result.failed.length > 0 ? `, ${outcome.result.failed.length} failed` : ''}.
          </p>
          {outcome.result.failed.length > 0 && (
            <ul className="file-list">
              {outcome.result.failed.map((f) => (
                <li key={f.path}>
                  <code>{f.path}</code>: {f.message}
                </li>
              ))}
            </ul>
          )}
        </div>
      )}
      {(mode === 'move' || mode === 'copy') && (
        <BulkFolderDialog
          title={`${mode === 'move' ? 'Move' : 'Copy'} ${count(selected.length)}`}
          groups={groups}
          submitLabel={mode === 'move' ? 'Move' : 'Copy'}
          onCancel={() => setMode(null)}
          onSubmit={(targets) => transfer(mode === 'move', targets)}
        />
      )}
      {mode === 'delete' && (
        <ConfirmDialog
          title={`Delete ${count(selected.length)}`}
          message={
            place.staged
              ? 'These files will be listed for deletion in the changes you download:'
              : `These files will be permanently removed from ${place.name}. This cannot be undone:`
          }
          files={listed}
          confirmLabel="Delete"
          onCancel={() => setMode(null)}
          onConfirm={remove}
        />
      )}
    </>
  );
}
