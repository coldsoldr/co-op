import { useMemo, useState } from 'react';
import type { PatchKind } from '../core/codec/op1-meta';
import { filterLocal, localFolders, type LocalFilter } from '../core/library/local-library';
import { formatSize } from './format';
import type { LocalLibrary } from './useLocalLibrary';

const KINDS: readonly PatchKind[] = ['synth', 'drum'];
/** A path can never be this, so it stands for no folder filter. */
const ANY_FOLDER = '/';

/** Patches on the person's computer. It needs no OP-1: this tab works with nothing plugged in. */
export function PatchesTab({ local }: { local: LocalLibrary }) {
  const [filter, setFilter] = useState<LocalFilter>({});
  const folders = useMemo(() => localFolders(local.patches), [local.patches]);
  const shown = filterLocal(local.patches, filter);

  /** Sets one criterion; undefined clears it. */
  function set<K extends keyof LocalFilter>(key: K, value: LocalFilter[K] | undefined): void {
    const next = { ...filter };
    if (value === undefined) delete next[key];
    else next[key] = value;
    setFilter(next);
  }

  return (
    <section aria-labelledby="patches-heading">
      <div className="packs-header">
        <h2 id="patches-heading">Patches</h2>
        {local.folder && (
          <div className="plugin-buttons">
            <button type="button" onClick={() => void local.refresh()} disabled={local.progress !== null}>
              Refresh
            </button>
            <button type="button" onClick={() => void local.choose()}>
              Change folder
            </button>
            <button type="button" onClick={() => void local.forget()}>
              Forget folder
            </button>
          </div>
        )}
      </div>

      {!local.supported && (
        <p className="notice">This browser cannot open a folder. Chrome, Edge and Vivaldi can.</p>
      )}

      {local.supported && !local.folder && (
        <div className="connect">
          <p>
            Keep your patches in a folder on your computer and browse them here. Nothing needs to be plugged in, and co-op
            only reads the folder.
          </p>
          <div className="plugin-buttons">
            {local.remembered && (
              <button type="button" className="primary" onClick={() => void local.reconnect()}>
                Reconnect {local.remembered.name}
              </button>
            )}
            <button type="button" className={local.remembered ? undefined : 'primary'} onClick={() => void local.choose()}>
              Choose a folder
            </button>
          </div>
        </div>
      )}

      {local.error && (
        <p role="alert" className="error">
          {local.error}
        </p>
      )}

      {local.folder && (
        <>
          <div className="toolbar">
            <label>
              Search patches{' '}
              <input
                type="search"
                value={filter.text ?? ''}
                onChange={(e) => set('text', e.target.value === '' ? undefined : e.target.value)}
                placeholder="name, engine, folder"
              />
            </label>
            <label>
              Kind{' '}
              <select
                value={filter.kind ?? ''}
                onChange={(e) => set('kind', e.target.value === '' ? undefined : (e.target.value as PatchKind))}
              >
                <option value="">any</option>
                {KINDS.map((kind) => (
                  <option key={kind} value={kind}>
                    {kind}
                  </option>
                ))}
              </select>
            </label>
            <label>
              Folder{' '}
              <select
                value={filter.folder ?? ANY_FOLDER}
                onChange={(e) => set('folder', e.target.value === ANY_FOLDER ? undefined : e.target.value)}
              >
                <option value={ANY_FOLDER}>any</option>
                {folders.map((folder) => (
                  <option key={folder} value={folder}>
                    {folder || local.folder?.label}
                  </option>
                ))}
              </select>
            </label>
          </div>

          <p className="muted" role="status">
            {local.progress
              ? `Reading patches: ${local.progress.done} of ${local.progress.total}`
              : shown.length === local.patches.length
                ? `${local.patches.length} patch file(s) in ${local.folder.label}`
                : `${shown.length} of ${local.patches.length} patch file(s) in ${local.folder.label}`}
          </p>

          {!local.progress && local.patches.length === 0 && <p className="empty">No patches found in this folder.</p>}

          {shown.length > 0 && (
            <div className="table-scroll">
              <table className="library patches">
                <thead>
                  <tr>
                    <th scope="col">Name</th>
                    <th scope="col">Kind</th>
                    <th scope="col">Size</th>
                  </tr>
                </thead>
                <tbody>
                  {shown.map((patch) => (
                    <tr key={patch.path} data-path={patch.path}>
                      <th scope="row" className="name">
                        {patch.name}
                        <code className="file">{patch.path}</code>
                        {(patch.engine || patch.fx) && (
                          <span className="sound">{[patch.engine, patch.fx].filter(Boolean).join(' · ')}</span>
                        )}
                        {patch.error && <span className="row-error">{patch.error}</span>}
                      </th>
                      <td className="kind">{patch.kind}</td>
                      <td className="size">{patch.error ? '' : formatSize(patch.sizeBytes)}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          )}
          {local.patches.length > 0 && shown.length === 0 && <p>No patches match these filters.</p>}
        </>
      )}
    </section>
  );
}
