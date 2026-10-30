import { useCallback, useEffect, useMemo, useState } from 'react';
import { fetchIndex, fetchPatchBytes } from '../core/catalogue/catalogue';
import type { PatchKind } from '../core/codec/op1-meta';
import {
  communityPatches,
  filterLocal,
  localEngines,
  localFolders,
  type LocalFilter,
  type LocalPatch,
} from '../core/library/local-library';
import { messageOf } from '../core/util';
import { PatchKeyboard } from './PatchKeyboard';
import { AuditionBar } from './AuditionBar';
import { CopyOutcome } from './CopyOutcome';
import { PatchRow } from './PatchRow';
import { useCopyToFolder } from './useCopyToFolder';
import type { Audition } from './useAudition';
import { usePatchPreview } from './usePatchPreview';
import type { LocalLibrary } from './useLocalLibrary';

const KINDS: readonly PatchKind[] = ['synth', 'drum'];
/** A path can never be this, so it stands for no folder filter. */
const ANY_FOLDER = '/';

type Source = 'local' | 'community';

/** Community patches and the person's own folder. It needs no OP-1: nothing has to be plugged in. */
export function PatchesTab({ local, audition, active }: { local: LocalLibrary; audition: Audition; active: boolean }) {
  const [source, setSource] = useState<Source>('local');
  const [filter, setFilter] = useState<LocalFilter>({});
  const [community, setCommunity] = useState<LocalPatch[] | null>(null);
  const [communityError, setCommunityError] = useState<string | null>(null);
  const preview = usePatchPreview(active);
  const copier = useCopyToFolder(local);
  const [selected, setSelected] = useState<ReadonlySet<string>>(() => new Set());

  const loadCommunity = useCallback(() => {
    fetchIndex(import.meta.env.BASE_URL)
      .then((index) => setCommunity(communityPatches(index)))
      .catch((e: unknown) => setCommunityError(messageOf(e)));
  }, []);

  useEffect(() => {
    if (active && source === 'community' && community === null) loadCommunity();
  }, [active, community, loadCommunity, source]);

  const patches = useMemo(() => (source === 'local' ? local.patches : (community ?? [])), [community, local.patches, source]);
  const folders = useMemo(() => localFolders(patches), [patches]);
  const engines = useMemo(() => localEngines(patches), [patches]);
  const shown = filterLocal(patches, filter);
  const copying = source === 'community';
  const chosen = shown.filter((p) => selected.has(p.path));
  const allOn = shown.length > 0 && chosen.length === shown.length;

  /** Sets one criterion; undefined clears it. */
  function set<K extends keyof LocalFilter>(key: K, value: LocalFilter[K] | undefined): void {
    const next = { ...filter };
    if (value === undefined) delete next[key];
    else next[key] = value;
    setFilter(next);
  }

  function pickSource(next: Source): void {
    setSource(next);
    setFilter({});
    setSelected(new Set());
    preview.close();
  }

  async function bytesOf(patch: LocalPatch): Promise<Uint8Array> {
    if (patch.pack) return fetchPatchBytes(import.meta.env.BASE_URL, patch.pack.slug, patch.pack.file, patch.pack.sha256);
    if (!local.folder) throw new Error('the folder is no longer open');
    return local.folder.read(patch.path);
  }

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

  const needsFolder = source === 'local' && !local.folder;
  const loadingCommunity = source === 'community' && community === null && communityError === null;
  const where = source === 'local' ? (local.folder?.label ?? '') : 'community packs';

  return (
    <section aria-labelledby="patches-heading">
      <div className="packs-header">
        <h2 id="patches-heading">Patches</h2>
        {source === 'local' && local.folder && (
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

      <AuditionBar audition={audition} />

      <div role="group" aria-label="Patch source" className="source-switch">
        <button type="button" aria-pressed={source === 'local'} onClick={() => pickSource('local')}>
          My folder
        </button>
        <button type="button" aria-pressed={source === 'community'} onClick={() => pickSource('community')}>
          Community
        </button>
      </div>

      {source === 'local' && !local.supported && (
        <p className="notice">This browser cannot open a folder. Chrome, Edge and Vivaldi can.</p>
      )}

      {needsFolder && local.supported && (
        <div className="connect">
          <p>
            Keep your patches in a folder on your computer and browse them here. Nothing needs to be plugged in, and co-op
            writes to the folder only when you copy patches into it.
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

      {source === 'local' && local.error && (
        <p role="alert" className="error">
          {local.error}
        </p>
      )}
      {source === 'community' && communityError && (
        <div role="alert" className="error">
          <p>Could not load the community patches: {communityError}</p>
          <button
            type="button"
            onClick={() => {
              setCommunityError(null);
              loadCommunity();
            }}
          >
            Try again
          </button>
        </div>
      )}
      {loadingCommunity && <p role="status">Loading community patches…</p>}

      {!needsFolder && !loadingCommunity && !communityError && (
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
              Engine{' '}
              <select value={filter.engine ?? ''} onChange={(e) => set('engine', e.target.value === '' ? undefined : e.target.value)}>
                <option value="">any</option>
                {engines.map((engine) => (
                  <option key={engine} value={engine}>
                    {engine}
                  </option>
                ))}
              </select>
            </label>
            <label>
              {source === 'local' ? 'Folder' : 'Pack'}{' '}
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
            {source === 'local' && local.progress
              ? `Reading patches: ${local.progress.done} of ${local.progress.total}`
              : shown.length === patches.length
                ? `${patches.length} patch file(s) in ${where}`
                : `${shown.length} of ${patches.length} patch file(s) in ${where}`}
          </p>

          {copying && (
            <p>
              <button type="button" className="primary" onClick={() => void copyChosen()} disabled={copier.busy || chosen.length === 0}>
                {copier.label(chosen.length)}
              </button>
            </p>
          )}
          <CopyOutcome outcome={copier.outcome} />

          {preview.error && (
            <p role="alert" className="error">
              {preview.error}
            </p>
          )}

          {preview.open && active && (
            <PatchKeyboard name={preview.open.name} player={preview.open.player} kit={preview.open.kit} onClose={preview.close} />
          )}

          {!(source === 'local' && local.progress) && patches.length === 0 && <p className="empty">No patches found here.</p>}

          {shown.length > 0 && (
            <div className="table-scroll">
              <table className="library patches">
                <thead>
                  <tr>
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
                    <th scope="col">Name</th>
                    <th scope="col">Kind</th>
                    <th scope="col">Size</th>
                    <th scope="col" className="play">
                      <span className="visually-hidden">Preview</span>
                    </th>
                  </tr>
                </thead>
                <tbody>
                  {shown.map((patch) => (
                    <PatchRow
                      key={patch.path}
                      patch={patch}
                      audition={audition}
                      preview={preview}
                      load={() => bytesOf(patch)}
                      {...(copying
                        ? { select: { label: `Select ${patch.name}`, checked: selected.has(patch.path), onChange: (on: boolean) => toggle(patch.path, on) } }
                        : {})}
                    />
                  ))}
                </tbody>
              </table>
            </div>
          )}
          {patches.length > 0 && shown.length === 0 && <p>No patches match these filters.</p>}
        </>
      )}
    </section>
  );
}
