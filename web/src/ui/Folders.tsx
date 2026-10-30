import { useState, type FormEvent } from 'react';
import { freePercent, OP1_FLASH_BYTES } from '../core/library/drive-space';
import { localFolders } from '../core/library/local-library';
import { ensureFolder, isPresetFolder, removeFolder } from '../core/library/operations';
import { joinPath } from '../core/store/store';
import { formatSize } from './format';
import type { LocalLibrary } from './useLocalLibrary';
import type { Device } from './useDevice';
import { messageOf } from '../core/util';

/** The longest folder name offered here; a UI guard, like MAX_PATCH_NAME. */
const MAX_FOLDER_NAME = 24;

const patchCount = (n: number): string => (n === 1 ? '1 patch' : `${n} patches`);

interface FoldersProps {
  device: Device;
  /** The folder shown, for example 'synth/pads'; null for all patches. */
  selected: string | null;
  onSelect: (folder: string | null) => void;
  /** The person's own patch folder; its group shows once it is open. */
  local: LocalLibrary;
  /** The local folder shown, relative to the local folder; null when a drive folder is. */
  localSelected: string | null;
  onSelectLocal: (folder: string) => void;
}

/** The left column of the library: the patch folders, adding and removing them, and free space. */
export function Folders({ device, selected, onSelect, local, localSelected, onSelectLocal }: FoldersProps) {
  const [adding, setAdding] = useState<string | null>(null);
  const [draft, setDraft] = useState('');
  const [confirming, setConfirming] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const staged = device.source?.staged != null;

  const rowsIn = (folder: string) => device.rows.filter((r) => r.path.startsWith(`${folder}/`));

  function startAdd(root: string): void {
    setAdding(root);
    setDraft('');
    setConfirming(null);
    setError(null);
  }

  function pick(folder: string | null): void {
    onSelect(folder);
    setConfirming(null);
  }

  async function add(event: FormEvent, root: string): Promise<void> {
    event.preventDefault();
    setError(null);
    let made = '';
    try {
      await device.run(async (store) => {
        made = await ensureFolder(store, root, draft.trim());
      });
      setAdding(null);
      onSelect(made);
    } catch (e) {
      setError(messageOf(e));
    }
  }

  async function remove(folder: string): Promise<void> {
    setError(null);
    setConfirming(null);
    try {
      await device.run((store) => removeFolder(store, folder));
      onSelect(null);
    } catch (e) {
      setError(messageOf(e));
    }
  }

  return (
    <aside className="folders" aria-label="Folders">
      <div className="eyebrow rule">02 / FOLDERS</div>
      <button type="button" className="folder-pick all" aria-current={selected === null && localSelected === null} onClick={() => pick(null)}>
        <span>All patches</span>
        <span className="mono">{device.rows.length}</span>
      </button>

      {device.profile.layout.patchRoots.map(({ path: root }) => (
        <section key={root} aria-label={`${root} folders`} className="folder-group">
          <div className="folder-group-head">
            <span className="mono">{root}</span>
            <button
              type="button"
              className="round-hit"
              aria-label={`Add a ${root} folder`}
              onClick={() => startAdd(root)}
              disabled={device.busy}
            >
              <span className="ring">
                <svg aria-hidden="true" viewBox="0 0 24 24">
                  <path d="M12 5v14M5 12h14" />
                </svg>
              </span>
            </button>
          </div>

          {(device.folders[root] ?? []).map((name) => {
            const folder = joinPath(root, name);
            const count = rowsIn(folder).length;
            const on = selected === folder && localSelected === null;
            return (
              <div key={folder} className={`folder${on ? ' on' : ''}`}>
                <div className="folder-row">
                  <button
                    type="button"
                    className="folder-pick"
                    aria-current={on}
                    title={isPresetFolder(name) ? 'Holds the OP–1’s 8 quick access presets' : undefined}
                    onClick={() => pick(folder)}
                  >
                    <span>{name}</span>
                    <span className="mono">{count}</span>
                  </button>
                  {on && !isPresetFolder(name) && (
                    <button
                      type="button"
                      className="round-hit"
                      aria-label={`Remove folder ${name}`}
                      disabled={device.busy}
                      onClick={() => (count === 0 ? void remove(folder) : setConfirming(folder))}
                    >
                      <svg aria-hidden="true" viewBox="0 0 24 24">
                        <path d="M6 6l12 12M18 6L6 18" />
                      </svg>
                    </button>
                  )}
                </div>
                {confirming === folder && (
                  <div className="folder-confirm" role="alert">
                    <p>
                      {staged
                        ? `List ${name} and its ${patchCount(count)} for deletion in the changes you download?`
                        : `Remove ${name} and its ${patchCount(count)} from the OP–1? This cannot be undone without a backup.`}
                    </p>
                    <div>
                      <button type="button" className="solid-danger" onClick={() => void remove(folder)}>
                        Remove
                      </button>
                      <button type="button" className="on-ink" onClick={() => setConfirming(null)}>
                        Keep
                      </button>
                    </div>
                  </div>
                )}
              </div>
            );
          })}

          {adding === root && (
            <form className="folder-add" onSubmit={(e) => void add(e, root)}>
              <input
                type="text"
                value={draft}
                onChange={(e) => setDraft(e.target.value)}
                maxLength={MAX_FOLDER_NAME}
                placeholder="folder name"
                aria-label={`New ${root} folder name`}
                required
                autoFocus
              />
              <button type="submit" className="round-hit filled" aria-label="Add folder" disabled={device.busy}>
                <svg aria-hidden="true" viewBox="0 0 24 24">
                  <path d="M5 13l4 4L19 7" />
                </svg>
              </button>
              <button type="button" className="round-hit" aria-label="Cancel" onClick={() => setAdding(null)}>
                <svg aria-hidden="true" viewBox="0 0 24 24">
                  <path d="M6 6l12 12M18 6L6 18" />
                </svg>
              </button>
            </form>
          )}
        </section>
      ))}

      {local.folder && (
        <section aria-label="local folders" className="folder-group">
          <div className="folder-group-head">
            <span className="mono">local</span>
          </div>
          {localFolders(local.patches).map((name) => {
            const on = localSelected === name;
            return (
              <div key={name} className={`folder${on ? ' on' : ''}`}>
                <div className="folder-row">
                  <button type="button" className="folder-pick" aria-current={on} onClick={() => onSelectLocal(name)}>
                    <span>{name || local.folder?.label}</span>
                    <span className="mono">{local.patches.filter((p) => p.folder === name).length}</span>
                  </button>
                </div>
              </div>
            );
          })}
        </section>
      )}

      {error && (
        <p role="alert" className="error">
          {error}
        </p>
      )}
      {device.usedBytes !== null && <FreeSpace usedBytes={device.usedBytes} />}
    </aside>
  );
}

/** An estimate, and says so: see freePercent. */
function FreeSpace({ usedBytes }: { usedBytes: number }) {
  const free = freePercent(usedBytes);
  return (
    <div
      className="free-space"
      title={`An estimate: ${formatSize(usedBytes)} of files on the drive out of ${formatSize(OP1_FLASH_BYTES)} of flash. The OP-1’s system files are not counted, so the real figure is lower.`}
    >
      <span className="free-fill" style={{ width: `${100 - free}%` }} />
      <span className="free-label">free space</span>
      <span className="free-value">≈{free}%</span>
    </div>
  );
}
