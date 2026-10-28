import { useState, type FormEvent } from 'react';
import type { LibraryEntry } from '../core/library/library';
import {
  MAX_PATCH_NAME,
  copyPatch,
  deletePatch,
  ensureFolder,
  movePatch,
  renameFile,
  setPatchName,
} from '../core/library/operations';
import { joinPath, type DeviceStore } from '../core/store/store';
import { ConfirmDialog, Dialog } from './Dialog';
import { folderOf, rootOf } from './format';

type Mode = 'rename' | 'name' | 'move' | 'copy' | 'delete';

const NEW_FOLDER = '\u0000new';

interface PatchActionsProps {
  entry: LibraryEntry;
  /** Folder names under the entry's patch root. */
  folders: readonly string[];
  /** True while this entry is playing. */
  playing: boolean;
  /** Starts or stops playback; may throw (for example, audio the browser cannot decode). */
  onTogglePlay: () => void;
  run: (op: (store: DeviceStore) => Promise<unknown>) => Promise<void>;
  /** True when changes are staged for download rather than written to the drive. */
  staged: boolean;
}

const messageOf = (error: unknown): string => (error instanceof Error ? error.message : String(error));

/** Play, rename, set name, move, copy and delete for one library row. Errors show in the row. */
export function PatchActions({ entry, folders, playing, onTogglePlay, run, staged }: PatchActionsProps) {
  const [mode, setMode] = useState<Mode | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const file = entry.fileName;
  const canName = entry.patch?.meta != null;

  async function perform(op: (store: DeviceStore) => Promise<unknown>): Promise<void> {
    setMode(null);
    setBusy(true);
    setError(null);
    try {
      await run(op);
    } catch (e) {
      setError(messageOf(e));
    } finally {
      setBusy(false);
    }
  }

  function play(): void {
    setError(null);
    try {
      onTogglePlay();
    } catch (e) {
      setError(`cannot play: ${messageOf(e)}`);
    }
  }

  const close = (): void => setMode(null);

  return (
    <div className="actions">
      <button
        type="button"
        onClick={play}
        disabled={!entry.patch}
        aria-label={`${playing ? 'Stop' : 'Play'} ${file}`}
        aria-pressed={playing}
      >
        {playing ? 'Stop' : 'Play'}
      </button>
      <button type="button" onClick={() => setMode('rename')} disabled={busy} aria-label={`Rename file ${file}`}>
        Rename
      </button>
      <button
        type="button"
        onClick={() => setMode('name')}
        disabled={busy || !canName}
        aria-label={`Set patch name of ${file}`}
        title={canName ? undefined : 'no op-1 metadata to hold a name'}
      >
        Name
      </button>
      <button type="button" onClick={() => setMode('move')} disabled={busy} aria-label={`Move ${file} to a folder`}>
        Move
      </button>
      <button type="button" onClick={() => setMode('copy')} disabled={busy} aria-label={`Copy ${file} to a folder`}>
        Copy
      </button>
      <button
        type="button"
        className="danger"
        onClick={() => setMode('delete')}
        disabled={busy}
        aria-label={`Delete ${file}`}
      >
        Delete
      </button>
      {error && (
        <p role="alert" className="row-error">
          {error}
        </p>
      )}

      {mode === 'rename' && (
        <TextDialog
          title={`Rename ${file}`}
          label="New file name"
          initial={file}
          submitLabel="Rename"
          onCancel={close}
          onSubmit={(name) => perform((store) => renameFile(store, entry.path, name))}
        />
      )}
      {mode === 'name' && (
        <TextDialog
          title={`Set the patch name of ${file}`}
          label="Patch name (shown on the OP-1)"
          initial={entry.patch?.name ?? ''}
          maxLength={MAX_PATCH_NAME}
          submitLabel="Set name"
          onCancel={close}
          onSubmit={(name) => perform((store) => setPatchName(store, entry.path, name))}
        />
      )}
      {(mode === 'move' || mode === 'copy') && (
        <FolderDialog
          title={`${mode === 'move' ? 'Move' : 'Copy'} ${file}`}
          root={rootOf(entry)}
          current={folderOf(entry)}
          folders={folders}
          submitLabel={mode === 'move' ? 'Move' : 'Copy'}
          onCancel={close}
          onSubmit={(target) =>
            perform(async (store) => {
              const root = rootOf(entry);
              const folder = target.isNew ? await ensureFolder(store, root, target.name) : joinPath(root, target.name);
              return mode === 'move' ? movePatch(store, entry.path, folder) : copyPatch(store, entry.path, folder);
            })
          }
        />
      )}
      {mode === 'delete' && (
        <ConfirmDialog
          title="Delete patch"
          message={
            staged
              ? 'This file will be listed for deletion in the changes you download:'
              : 'This file will be removed from the OP-1 drive. It cannot be undone unless you have a backup:'
          }
          files={[entry.path]}
          confirmLabel="Delete"
          onCancel={close}
          onConfirm={() => void perform((store) => deletePatch(store, entry.path))}
        />
      )}
    </div>
  );
}

interface TextDialogProps {
  title: string;
  label: string;
  initial: string;
  maxLength?: number;
  submitLabel: string;
  onCancel: () => void;
  onSubmit: (value: string) => void;
}

function TextDialog({ title, label, initial, maxLength, submitLabel, onCancel, onSubmit }: TextDialogProps) {
  const [value, setValue] = useState(initial);
  function submit(event: FormEvent): void {
    event.preventDefault();
    onSubmit(value);
  }
  return (
    <Dialog title={title} onClose={onCancel}>
      <form onSubmit={submit}>
        <label>
          {label}
          <input
            type="text"
            value={value}
            onChange={(e) => setValue(e.target.value)}
            maxLength={maxLength}
            required
            autoFocus
          />
        </label>
        <div className="dialog-buttons">
          <button type="button" onClick={onCancel}>
            Cancel
          </button>
          <button type="submit">{submitLabel}</button>
        </div>
      </form>
    </Dialog>
  );
}

interface FolderTarget {
  name: string;
  isNew: boolean;
}

interface FolderDialogProps {
  title: string;
  root: string;
  current: string;
  folders: readonly string[];
  submitLabel: string;
  onCancel: () => void;
  onSubmit: (target: FolderTarget) => void;
}

function FolderDialog({ title, root, current, folders, submitLabel, onCancel, onSubmit }: FolderDialogProps) {
  const [choice, setChoice] = useState(() => folders.find((f) => joinPath(root, f) !== current) ?? NEW_FOLDER);
  const [newName, setNewName] = useState('');
  const isNew = choice === NEW_FOLDER;
  function submit(event: FormEvent): void {
    event.preventDefault();
    onSubmit(isNew ? { name: newName.trim(), isNew: true } : { name: choice, isNew: false });
  }
  return (
    <Dialog title={title} onClose={onCancel}>
      <form onSubmit={submit}>
        <label>
          Folder
          <select value={choice} onChange={(e) => setChoice(e.target.value)} autoFocus>
            {folders.map((folder) => (
              <option key={folder} value={folder}>
                {joinPath(root, folder)}
                {joinPath(root, folder) === current ? ' (current)' : ''}
              </option>
            ))}
            <option value={NEW_FOLDER}>New folder…</option>
          </select>
        </label>
        {isNew && (
          <label>
            New folder name (in {root})
            <input type="text" value={newName} onChange={(e) => setNewName(e.target.value)} required />
          </label>
        )}
        <div className="dialog-buttons">
          <button type="button" onClick={onCancel}>
            Cancel
          </button>
          <button type="submit">{submitLabel}</button>
        </div>
      </form>
    </Dialog>
  );
}
