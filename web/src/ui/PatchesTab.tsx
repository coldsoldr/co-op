import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { fetchIndex, fetchPatchBytes } from '../core/catalogue/catalogue';
import { decodePcm, readPatch } from '../core/codec/patch';
import type { PatchKind } from '../core/codec/op1-meta';
import { hasPreview, keyPlan } from '../core/codec/preview';
import {
  communityPatches,
  filterLocal,
  localEngines,
  localFolders,
  type LocalFilter,
  type LocalPatch,
} from '../core/library/local-library';
import { messageOf } from '../core/util';
import { formatSize } from './format';
import { KeyboardPlayer } from './keyboard-player';
import { PatchKeyboard } from './PatchKeyboard';
import type { LocalLibrary } from './useLocalLibrary';

const KINDS: readonly PatchKind[] = ['synth', 'drum'];
/** A path can never be this, so it stands for no folder filter. */
const ANY_FOLDER = '/';
const NO_PREVIEW = 'This patch holds no preview: the OP-1 only stored its voice clip here.';

type Source = 'local' | 'community';

interface Open {
  patch: LocalPatch;
  player: KeyboardPlayer;
  kit: boolean;
}

/** Community patches and the person's own folder. It needs no OP-1: nothing has to be plugged in. */
export function PatchesTab({ local, active }: { local: LocalLibrary; active: boolean }) {
  const [source, setSource] = useState<Source>('local');
  const [filter, setFilter] = useState<LocalFilter>({});
  const [community, setCommunity] = useState<LocalPatch[] | null>(null);
  const [communityError, setCommunityError] = useState<string | null>(null);
  const [open, setOpen] = useState<Open | null>(null);
  const [playing, setPlaying] = useState<string | null>(null);
  const [playError, setPlayError] = useState<string | null>(null);
  const openRef = useRef<Open | null>(null);
  /** Bumped by every play click, so a read that finishes late never starts. */
  const playToken = useRef(0);

  const loadCommunity = useCallback(() => {
    fetchIndex(import.meta.env.BASE_URL)
      .then((index) => setCommunity(communityPatches(index)))
      .catch((e: unknown) => setCommunityError(messageOf(e)));
  }, []);

  useEffect(() => {
    if (active && source === 'community' && community === null) loadCommunity();
  }, [active, community, loadCommunity, source]);

  const closePlayer = useCallback(() => {
    playToken.current++;
    openRef.current?.player.stopWhole();
    openRef.current?.player.close();
    openRef.current = null;
    setOpen(null);
    setPlaying(null);
  }, []);

  // Hiding the tab only stops the sound: the keyboard is not shown and the patch stays open.
  useEffect(() => {
    if (!active) openRef.current?.player.stopWhole();
  }, [active]);
  useEffect(
    () => () => {
      openRef.current?.player.close();
    },
    [],
  );

  const patches = useMemo(() => (source === 'local' ? local.patches : (community ?? [])), [community, local.patches, source]);
  const folders = useMemo(() => localFolders(patches), [patches]);
  const engines = useMemo(() => localEngines(patches), [patches]);
  const shown = filterLocal(patches, filter);

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
    closePlayer();
  }

  async function bytesOf(patch: LocalPatch): Promise<Uint8Array> {
    if (patch.pack) return fetchPatchBytes(import.meta.env.BASE_URL, patch.pack.slug, patch.pack.file, patch.pack.sha256);
    if (!local.folder) throw new Error('the folder is no longer open');
    return local.folder.read(patch.path);
  }

  async function play(patch: LocalPatch): Promise<void> {
    const wasPlaying = playing === patch.path;
    const token = ++playToken.current;
    setPlayError(null);
    openRef.current?.player.stopWhole();
    setPlaying(null);
    if (wasPlaying) return;
    try {
      let current = openRef.current?.patch.path === patch.path ? openRef.current : null;
      if (!current) {
        const parsed = readPatch(await bytesOf(patch));
        if (!(await hasPreview(parsed))) throw new Error(NO_PREVIEW);
        if (token !== playToken.current) return;
        closePlayer();
        const player = new KeyboardPlayer(decodePcm(parsed), parsed.comm.sampleRate, keyPlan(parsed));
        current = { patch, player, kit: parsed.meta?.type === 'drum' };
        playToken.current = token;
        openRef.current = current;
        setOpen(current);
      }
      setPlaying(patch.path);
      current.player.playWhole(() => setPlaying((now) => (now === patch.path ? null : now)));
    } catch (e) {
      if (token === playToken.current) setPlayError(`${patch.name}: ${messageOf(e)}`);
    }
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

          {playError && (
            <p role="alert" className="error">
              {playError}
            </p>
          )}

          {open && active && (
            <PatchKeyboard name={open.patch.name} player={open.player} kit={open.kit} onClose={closePlayer} />
          )}

          {!(source === 'local' && local.progress) && patches.length === 0 && <p className="empty">No patches found here.</p>}

          {shown.length > 0 && (
            <div className="table-scroll">
              <table className="library patches">
                <thead>
                  <tr>
                    <th scope="col" className="play">
                      <span className="visually-hidden">Play</span>
                    </th>
                    <th scope="col">Name</th>
                    <th scope="col">Kind</th>
                    <th scope="col">Size</th>
                  </tr>
                </thead>
                <tbody>
                  {shown.map((patch) => (
                    <tr key={patch.path} data-path={patch.path}>
                      <td className="play">
                        <button
                          type="button"
                          className={`play-key ${patch.kind}`}
                          disabled={patch.preview === false || patch.error !== null}
                          title={patch.preview === false ? 'No preview: the OP-1 only stored its voice clip' : undefined}
                          aria-label={`${playing === patch.path ? 'Stop' : 'Play'} ${patch.name}`}
                          aria-pressed={playing === patch.path}
                          onClick={() => void play(patch)}
                        >
                          <svg aria-hidden="true" viewBox="0 0 24 24">
                            <path d={playing === patch.path ? 'M6 5h12v14H6z' : 'M7 4l13 8-13 8z'} />
                          </svg>
                        </button>
                      </td>
                      <th scope="row" className="name">
                        {patch.name}
                        <code className="file">{patch.path}</code>
                        {(patch.engine || patch.fx) && (
                          <span className="sound">{[patch.engine, patch.fx].filter(Boolean).join(' · ')}</span>
                        )}
                        {patch.preview === false && <span className="sound">no preview</span>}
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
          {patches.length > 0 && shown.length === 0 && <p>No patches match these filters.</p>}
        </>
      )}
    </section>
  );
}
