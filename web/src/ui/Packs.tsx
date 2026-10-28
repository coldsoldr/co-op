import { useCallback, useEffect, useId, useMemo, useRef, useState } from 'react';
import { PACK_KINDS, PACK_LICENSES, type PackIndex, type PackIndexEntry, type PackKind } from '@co-op/shared/packs';
import {
  InstallError,
  compatibilityWarning,
  fetchIndex,
  fetchPatchBytes,
  filterPacks,
  installPatches,
  type PackFilter,
} from '../core/catalogue/catalogue';
import { decodePcm, readPatch } from '../core/codec/patch';
import { getProfile, profiles } from '../core/profiles/registry';
import { joinPath } from '../core/store/store';
import { playPcm } from './audio';
import { formatSize } from './format';
import type { Device } from './useDevice';

/** The contributor guide, linked from "Share a pack". */
export const SHARE_URL = 'https://github.com/coldsoldr/co-op/blob/main/docs/contributing-packs.md';

const messageOf = (error: unknown): string => (error instanceof Error ? error.message : String(error));
const firmwareLabel = (id: string): string => getProfile(id)?.label ?? id;
const plural = (n: number, one: string, many: string): string => `${n} ${n === 1 ? one : many}`;

interface PacksProps {
  /** The connection shared with the Library tab. */
  device: Device;
  /** False while the tab is hidden: the index is not fetched yet and playback stops. */
  active: boolean;
  /** Switches to the Library tab, to connect a device. */
  onOpenLibrary: () => void;
}

/** The community pack catalogue: browse, preview and install packs published with the app. */
export function Packs({ device, active, onOpenLibrary }: PacksProps) {
  const [index, setIndex] = useState<PackIndex | null>(null);
  const [loadError, setLoadError] = useState<string | null>(null);
  const requested = useRef(false);
  const [filter, setFilter] = useState<PackFilter>({});
  const [openSlug, setOpenSlug] = useState<string | null>(null);

  const load = useCallback(() => {
    fetchIndex(import.meta.env.BASE_URL)
      .then((next) => {
        setIndex(next);
        setLoadError(null);
      })
      .catch((e: unknown) => setLoadError(messageOf(e)));
  }, []);

  // Fetched the first time the tab is shown, not on page load.
  useEffect(() => {
    if (!active || requested.current) return;
    requested.current = true;
    load();
  }, [active, load]);

  const open = index?.packs.find((p) => p.slug === openSlug) ?? null;

  return (
    <section aria-labelledby="packs-heading">
      <div className="packs-header">
        <h2 id="packs-heading">Community packs</h2>
        <a className="button" href={SHARE_URL} target="_blank" rel="noreferrer">
          Share a pack
        </a>
      </div>
      {loadError && (
        <div role="alert" className="error">
          <p>Could not load the pack catalogue: {loadError}</p>
          <button type="button" onClick={load}>
            Try again
          </button>
        </div>
      )}
      {!index && !loadError && <p role="status">Loading packs…</p>}
      {index && open && (
        <PackDetail
          key={open.slug}
          pack={open}
          device={device}
          active={active}
          onBack={() => setOpenSlug(null)}
          onOpenLibrary={onOpenLibrary}
        />
      )}
      {index && !open && <PackList index={index} filter={filter} onFilter={setFilter} onOpen={setOpenSlug} />}
    </section>
  );
}

interface PackListProps {
  index: PackIndex;
  filter: PackFilter;
  onFilter: (filter: PackFilter) => void;
  onOpen: (slug: string) => void;
}

function PackList({ index, filter, onFilter, onOpen }: PackListProps) {
  const shown = filterPacks(index, filter);
  const tags = useMemo(() => [...new Set(index.packs.flatMap((p) => p.tags))].sort(), [index]);
  const firmware = useMemo(
    () => [...new Set([...profiles.map((p) => p.id), ...index.packs.flatMap((p) => p.firmware)])],
    [index],
  );
  /** Sets one criterion; the empty string clears it. */
  function set<K extends keyof PackFilter>(key: K, value: string): void {
    const next = { ...filter };
    if (value === '') delete next[key];
    else next[key] = value as PackFilter[K];
    onFilter(next);
  }

  return (
    <>
      <div className="toolbar">
        <label>
          Search packs{' '}
          <input
            type="search"
            value={filter.text ?? ''}
            onChange={(e) => set('text', e.target.value)}
            placeholder="name, author, tag"
          />
        </label>
        <label>
          Kind{' '}
          <select value={filter.kind ?? ''} onChange={(e) => set('kind', e.target.value)}>
            <option value="">any</option>
            <option value="synth">synth</option>
            <option value="drum">drum</option>
          </select>
        </label>
        <label>
          Firmware{' '}
          <select value={filter.firmware ?? ''} onChange={(e) => set('firmware', e.target.value)}>
            <option value="">any</option>
            {firmware.map((id) => (
              <option key={id} value={id}>
                {firmwareLabel(id)}
              </option>
            ))}
          </select>
        </label>
        <label>
          Licence{' '}
          <select value={filter.license ?? ''} onChange={(e) => set('license', e.target.value)}>
            <option value="">any</option>
            {PACK_LICENSES.map((license) => (
              <option key={license} value={license}>
                {license}
              </option>
            ))}
          </select>
        </label>
        {tags.length > 0 && (
          <label>
            Tag{' '}
            <select value={filter.tag ?? ''} onChange={(e) => set('tag', e.target.value)}>
              <option value="">any</option>
              {tags.map((tag) => (
                <option key={tag} value={tag}>
                  {tag}
                </option>
              ))}
            </select>
          </label>
        )}
      </div>
      <p className="muted">
        {shown.length === index.packs.length
          ? plural(index.packs.length, 'pack', 'packs')
          : `${shown.length} of ${plural(index.packs.length, 'pack', 'packs')}`}
      </p>
      {shown.length === 0 && <p>No packs match these filters.</p>}
      <div className="pack-grid">
        {shown.map((pack) => (
          <PackCard key={pack.slug} pack={pack} onOpen={onOpen} />
        ))}
      </div>
    </>
  );
}

function PackCard({ pack, onOpen }: { pack: PackIndexEntry; onOpen: (slug: string) => void }) {
  const headingId = useId();
  return (
    <article className="pack-card" aria-labelledby={headingId}>
      <h3 id={headingId}>{pack.name}</h3>
      <p className="muted">by {pack.author}</p>
      <p className="badges">
        <span className="badge licence">{pack.license}</span>
        {pack.firmware.map((id) => (
          <span key={id} className="badge firmware">
            {firmwareLabel(id)}
          </span>
        ))}
      </p>
      <p>
        {plural(pack.patches.length, 'patch', 'patches')}, {formatSize(pack.sizeBytes)}
      </p>
      <p>{pack.description}</p>
      <button type="button" aria-label={`View ${pack.name}`} onClick={() => onOpen(pack.slug)}>
        View
      </button>
    </article>
  );
}

interface PackDetailProps {
  pack: PackIndexEntry;
  device: Device;
  active: boolean;
  onBack: () => void;
  onOpenLibrary: () => void;
}

type Outcome = { ok: true; message: string; paths: string[] } | { ok: false; message: string; paths: string[] };

function PackDetail({ pack, device, active, onBack, onOpenLibrary }: PackDetailProps) {
  const [selected, setSelected] = useState<ReadonlySet<string>>(() => new Set());
  const [targets, setTargets] = useState<Partial<Record<PackKind, string>>>({});
  const [working, setWorking] = useState(false);
  const [outcome, setOutcome] = useState<Outcome | null>(null);
  const [playing, setPlaying] = useState<string | null>(null);
  const [playError, setPlayError] = useState<string | null>(null);
  const stopRef = useRef<(() => void) | null>(null);
  const playToken = useRef(0);
  const activeRef = useRef(active);

  useEffect(() => {
    activeRef.current = active;
    if (!active) {
      playToken.current++;
      stopRef.current?.();
    }
  }, [active]);
  useEffect(
    () => () => {
      playToken.current++;
      stopRef.current?.();
    },
    [],
  );

  const connected = device.source !== null;
  const staged = device.source?.staged != null;
  const warning = connected ? compatibilityWarning(pack, device.profile.id) : null;
  const kinds = kindsIn(pack);

  /** The folders offered for a kind: `<root>/<slug>` first, then the folders already on the drive. */
  function folderOptions(kind: PackKind): string[] {
    const root = device.profile.layout.patchRoots.find((r) => r.kind === kind)?.path ?? kind;
    const existing = (device.folders[root] ?? []).map((name) => joinPath(root, name));
    return [...new Set([joinPath(root, pack.slug), ...existing])];
  }
  const targetFor = (kind: PackKind): string => targets[kind] ?? (folderOptions(kind)[0] as string);

  async function play(file: string, sha256: string): Promise<void> {
    const token = ++playToken.current;
    const wasPlaying = stopRef.current !== null && playing === file;
    stopRef.current?.();
    setPlayError(null);
    if (wasPlaying) return;
    try {
      const bytes = await fetchPatchBytes(import.meta.env.BASE_URL, pack.slug, file, sha256);
      if (token !== playToken.current || !activeRef.current) return;
      const patch = readPatch(bytes);
      const stop = playPcm(decodePcm(patch), patch.comm.sampleRate, () => {
        if (stopRef.current === stop) {
          stopRef.current = null;
          setPlaying(null);
        }
      });
      stopRef.current = stop;
      setPlaying(file);
    } catch (e) {
      if (token === playToken.current) setPlayError(`cannot play ${file}: ${messageOf(e)}`);
    }
  }

  async function install(files: readonly string[]): Promise<void> {
    const profileId = device.profile.id;
    const chosen = Object.fromEntries(kinds.map((kind) => [kind, targetFor(kind)])) as Record<PackKind, string>;
    setWorking(true);
    setOutcome(null);
    let installed: string[] = [];
    try {
      const entries = pack.patches.filter((p) => files.includes(p.file));
      const downloads = await Promise.all(
        entries.map(async (p) => ({
          file: p.file,
          bytes: await fetchPatchBytes(import.meta.env.BASE_URL, pack.slug, p.file, p.sha256),
        })),
      );
      // Queued with the Library's own actions, so an install never overlaps another change.
      await device.run(async (store) => {
        installed = (await installPatches(store, profileId, pack.slug, downloads, (kind) => chosen[kind])).installed;
      });
      const where = staged ? ' Download them from the Library tab to copy them onto the OP-1.' : '';
      setOutcome({ ok: true, message: `Installed ${installed.length} patch(es).${where}`, paths: installed });
      setSelected(new Set());
    } catch (e) {
      const paths = e instanceof InstallError ? e.installed : installed;
      setOutcome({ ok: false, message: `Install failed: ${messageOf(e)}`, paths });
    } finally {
      setWorking(false);
    }
  }

  const blocked = !connected || device.busy || working;
  const selectedFiles = pack.patches.filter((p) => selected.has(p.file)).map((p) => p.file);

  return (
    <div className="pack-detail">
      <p>
        <button type="button" onClick={onBack}>
          Back to packs
        </button>
      </p>
      <h3>{pack.name}</h3>
      <p className="muted">
        by {pack.author} · <span className="badge licence">{pack.license}</span> ·{' '}
        {plural(pack.patches.length, 'patch', 'patches')}, {formatSize(pack.sizeBytes)}
      </p>
      <p>{pack.description}</p>
      {warning && (
        <p className="warning" role="note">
          {warning}
        </p>
      )}

      <table className="library pack">
        <caption>Patches in {pack.name}</caption>
        <thead>
          <tr>
            <th scope="col" className="select">
              <span className="visually-hidden">Select</span>
            </th>
            <th scope="col">Name</th>
            <th scope="col">File</th>
            <th scope="col">Kind</th>
            <th scope="col">Engine</th>
            <th scope="col">Size</th>
            <th scope="col">Preview</th>
          </tr>
        </thead>
        <tbody>
          {pack.patches.map((p) => (
            <tr key={p.file}>
              <td className="select">
                <input
                  type="checkbox"
                  aria-label={`Select ${p.file}`}
                  checked={selected.has(p.file)}
                  onChange={(e) => {
                    const next = new Set(selected);
                    if (e.target.checked) next.add(p.file);
                    else next.delete(p.file);
                    setSelected(next);
                  }}
                />
              </td>
              <th scope="row" className="name">
                {p.name || p.file}
              </th>
              <td className="file">
                <code>{p.file}</code>
              </td>
              <td className="kind">{p.kind}</td>
              <td>{p.engine}</td>
              <td className="size">{formatSize(p.sizeBytes)}</td>
              <td>
                <button type="button" aria-label={`${playing === p.file ? 'Stop' : 'Play'} ${p.file}`} onClick={() => void play(p.file, p.sha256)}>
                  {playing === p.file ? 'Stop' : 'Play'}
                </button>
              </td>
            </tr>
          ))}
        </tbody>
      </table>
      {playError && (
        <p role="alert" className="error">
          {playError}
        </p>
      )}

      <div className="panel">
        <h4>Install</h4>
        {kinds.map((kind) => (
          <p key={kind}>
            <label>
              Install {kind} patches into{' '}
              <select value={targetFor(kind)} onChange={(e) => setTargets({ ...targets, [kind]: e.target.value })} disabled={!connected}>
                {folderOptions(kind).map((folder) => (
                  <option key={folder} value={folder}>
                    {folder}
                  </option>
                ))}
              </select>
            </label>
          </p>
        ))}
        <p>
          <button type="button" onClick={() => void install(selectedFiles)} disabled={blocked || selectedFiles.length === 0}>
            Install selected ({selectedFiles.length})
          </button>{' '}
          <button type="button" className="primary" onClick={() => void install(pack.patches.map((p) => p.file))} disabled={blocked}>
            Install all ({pack.patches.length})
          </button>
        </p>
        {!connected && (
          <p className="notice">
            Connect an OP-1 or open its drive folder on the Library tab to install these patches.{' '}
            <button type="button" onClick={onOpenLibrary}>
              Go to Library
            </button>
          </p>
        )}
        {connected && staged && (
          <p className="muted">
            The open drive is a read-only copy: installed patches are added to the changes to download from the Library
            tab.
          </p>
        )}
        {working && <p role="status">Installing…</p>}
        {outcome && (
          <div role={outcome.ok ? 'status' : 'alert'} className={outcome.ok ? 'notice' : 'error'}>
            <p>{outcome.message}</p>
            {outcome.paths.length > 0 && (
              <ul className="file-list">
                {outcome.paths.map((path) => (
                  <li key={path}>
                    <code>{path}</code>
                  </li>
                ))}
              </ul>
            )}
          </div>
        )}
      </div>
    </div>
  );
}

/** The kinds a pack has patches of, in synth, drum order. */
function kindsIn(pack: PackIndexEntry): PackKind[] {
  return PACK_KINDS.filter((kind) => pack.patches.some((p) => p.kind === kind));
}
