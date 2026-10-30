import { memo, useId, useRef, useState, type FormEvent } from 'react';
import {
  MAX_PATCH_NAME,
  copyPatch,
  deletePatch,
  ensureFolder,
  movePatch,
  renamePatch,
} from '../core/library/operations';
import { joinPath, type DeviceStore } from '../core/store/store';
import { ConfirmDialog, Dialog } from './Dialog';
import { folderOf, rootOf } from './format';
import { messageOf } from '../core/util';

type Mode = 'rename' | 'move' | 'copy' | 'delete';

const NEW_FOLDER = '\u0000new';

/** What the actions need of a patch, on the OP-1 or in My folder. */
interface ActionEntry {
  path: string;
  fileName: string;
  info: { name: string; hasMeta: boolean } | null;
}

interface PatchActionsProps {
  entry: ActionEntry;
  /** The folder `folders` are in; the entry's patch root unless given. */
  root?: string;
  /** The folder the entry is in; worked out from its path unless given. */
  current?: string;
  /** Folder names under the root. */
  folders: readonly string[];
  run: (op: (store: DeviceStore) => Promise<unknown>, touched?: readonly string[]) => Promise<void>;
  /** True while any action is queued or running; actions wait for it. */
  busy: boolean;
  /** True when changes are staged for download rather than written to the drive. */
  staged: boolean;
  /** Named in the delete warning. */
  driveName?: string;
  /** Opens the op1.fun upload dialog; the action is hidden without it. */
  onUpload?: () => void;
  /** Why uploading is unavailable (shown as the button's description), or null. */
  uploadUnavailable?: string | null;
  /** The id of an element holding uploadUnavailable, for aria-describedby. */
  uploadNoteId?: string;
  /** Shows a failure in the row, or clears it with null. */
  onError: (message: string | null) => void;
}

/** Rename, move, copy, upload and delete for one library row, behind a round key. */
export const PatchActions = memo(function PatchActions({
  entry,
  root: givenRoot,
  current,
  folders,
  run,
  staged,
  busy,
  driveName = 'the OP-1 drive',
  onUpload,
  uploadUnavailable = null,
  uploadNoteId,
  onError,
}: PatchActionsProps) {
  const [mode, setMode] = useState<Mode | null>(null);
  const menuId = useId();
  const moreRef = useRef<HTMLButtonElement>(null);
  const menuRef = useRef<HTMLDivElement>(null);
  const file = entry.fileName;
  const canName = entry.info?.hasMeta === true;
  const root = givenRoot ?? rootOf(entry);

  async function perform(op: (store: DeviceStore) => Promise<unknown>): Promise<void> {
    setMode(null);
    onError(null);
    try {
      await run(op, [entry.path]);
    } catch (e) {
      onError(messageOf(e));
    }
  }

  /** The menu sits in the top layer, so the table's scroll box cannot clip it; it opens below the key, or above near the bottom. */
  function placeMenu(): void {
    const key = moreRef.current?.getBoundingClientRect();
    const menu = menuRef.current;
    if (!key || !menu) return;
    const above = key.top > window.innerHeight / 2;
    menu.style.top = above ? 'auto' : `${key.bottom + 6}px`;
    menu.style.bottom = above ? `${window.innerHeight - key.top + 6}px` : 'auto';
    menu.style.right = `${document.documentElement.clientWidth - key.right}px`;
  }

  function choose(next: Mode): void {
    menuRef.current?.hidePopover();
    setMode(next);
  }

  const close = (): void => setMode(null);

  return (
    <div className="actions">
      <button
        ref={moreRef}
        type="button"
        className="more"
        popoverTarget={menuId}
        aria-label={`More actions for ${file}`}
        onClick={placeMenu}
      >
        <span aria-hidden="true">···</span>
      </button>
      <div ref={menuRef} id={menuId} popover="auto" className="menu">
        <button type="button" onClick={() => choose('rename')} disabled={busy} aria-label={`Rename ${file}`}>
          Rename
        </button>
        <button type="button" onClick={() => choose('move')} disabled={busy} aria-label={`Move ${file} to a folder`}>
          Move to a folder
        </button>
        <button type="button" onClick={() => choose('copy')} disabled={busy} aria-label={`Copy ${file} to a folder`}>
          Copy to a folder
        </button>
        {onUpload && (
          <button
            type="button"
            onClick={() => {
              menuRef.current?.hidePopover();
              onUpload();
            }}
            disabled={uploadUnavailable !== null}
            aria-label={`Upload ${file} to op1.fun`}
            aria-describedby={uploadUnavailable !== null ? uploadNoteId : undefined}
            title={uploadUnavailable ?? undefined}
          >
            Upload to op1.fun
          </button>
        )}
        <button
          type="button"
          className="danger"
          onClick={() => choose('delete')}
          disabled={busy}
          aria-label={`Delete ${file}`}
        >
          Delete
        </button>
      </div>

      {mode === 'rename' && (
        <TextDialog
          title={`Rename ${file}`}
          label="New name (for the patch and its file)"
          initial={entry.info?.name || file.slice(0, file.lastIndexOf('.'))}
          {...(canName ? { maxLength: MAX_PATCH_NAME } : {})}
          submitLabel="Rename"
          onCancel={close}
          onSubmit={(name) => perform((store) => renamePatch(store, entry.path, name))}
        />
      )}
      {(mode === 'move' || mode === 'copy') && (
        <FolderDialog
          title={`${mode === 'move' ? 'Move' : 'Copy'} ${file}`}
          root={root}
          current={current ?? folderOf(entry)}
          folders={folders}
          submitLabel={mode === 'move' ? 'Move' : 'Copy'}
          onCancel={close}
          onSubmit={(target) =>
            perform(async (store) => {
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
              : `This file will be removed from ${driveName}. It cannot be undone unless you have a backup:`
          }
          files={[entry.path]}
          confirmLabel="Delete"
          onCancel={close}
          onConfirm={() => void perform((store) => deletePatch(store, entry.path))}
        />
      )}
    </div>
  );
});

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
                {joinPath(root, folder) || 'Top of the folder'}
                {joinPath(root, folder) === current ? ' (current)' : ''}
              </option>
            ))}
            <option value={NEW_FOLDER}>New folder…</option>
          </select>
        </label>
        {isNew && (
          <label>
            New folder name{root ? ` (in ${root})` : ''}
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
