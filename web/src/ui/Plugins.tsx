import { useCallback, useEffect, useId, useMemo, useRef, useState, type ReactNode } from 'react';
import { PLUGIN_FIRMWARE, PLUGIN_KINDS, type PluginIndex, type PluginIndexEntry } from '@co-op/shared/plugins';
import {
  fetchManual,
  fetchPluginBytes,
  fetchPluginIndex,
  filterPlugins,
  pluginFileUrl,
  pluginFolderUrl,
  type PluginFilter,
} from '../core/plugins/catalogue';
import { getProfile } from '../core/profiles/registry';
import { messageOf } from '../core/util';
import { saveBlob } from './download';
import { formatSize } from './format';
import { Markdown } from './Markdown';

/** Where hundekraft plugins are installed and removed. */
export const WORKSHOP_URL = 'https://hundekraft.github.io/op1-workshop/';

const firmwareLabel = (id: string): string => getProfile(id)?.label ?? id;
const plural = (n: number, one: string, many: string): string => `${n} ${n === 1 ? one : many}`;
const base = (): string => import.meta.env.BASE_URL;

interface PluginsProps {
  /** False while the tab is hidden: the index is not fetched until the tab is first shown. */
  active: boolean;
  /**
   * More actions for a plugin, shown beside Download on its card and manual: "Install over USB"
   * for hundekraft (phase 12, task 9) and "Copy to mods/" for opus (task 10). Nothing yet.
   */
  renderActions?: (plugin: PluginIndexEntry) => ReactNode;
}

/** The plugin library published with the app: browse plugins, read their manuals and download them. */
export function Plugins({ active, renderActions }: PluginsProps) {
  const [index, setIndex] = useState<PluginIndex | null>(null);
  const [loadError, setLoadError] = useState<string | null>(null);
  const requested = useRef(false);
  const [filter, setFilter] = useState<PluginFilter>({});
  const [openId, setOpenId] = useState<string | null>(null);
  /** The plugin whose manual button takes focus when returning to the list. */
  const [returnTo, setReturnTo] = useState<string | null>(null);

  const load = useCallback(() => {
    fetchPluginIndex(base())
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

  const open = index?.plugins.find((p) => p.id === openId) ?? null;

  return (
    <section aria-labelledby="plugins-heading">
      <div className="packs-header">
        <h2 id="plugins-heading">Plugins</h2>
      </div>
      {loadError && (
        <div role="alert" className="error">
          <p>Could not load the plugin library: {loadError}</p>
          <button type="button" onClick={load}>
            Try again
          </button>
        </div>
      )}
      {!index && !loadError && <p role="status">Loading plugins…</p>}
      {index && open && (
        <PluginManual
          key={open.id}
          plugin={open}
          renderActions={renderActions}
          onBack={() => {
            setReturnTo(open.id);
            setOpenId(null);
          }}
        />
      )}
      {index && !open && (
        <PluginList
          index={index}
          filter={filter}
          onFilter={setFilter}
          onOpen={setOpenId}
          returnTo={returnTo}
          renderActions={renderActions}
        />
      )}
    </section>
  );
}

interface PluginListProps {
  index: PluginIndex;
  filter: PluginFilter;
  onFilter: (filter: PluginFilter) => void;
  onOpen: (id: string) => void;
  returnTo: string | null;
  renderActions: PluginsProps['renderActions'];
}

function PluginList({ index, filter, onFilter, onOpen, returnTo, renderActions }: PluginListProps) {
  const shown = filterPlugins(index, filter);
  const tags = useMemo(() => [...new Set(index.plugins.flatMap((p) => p.tags))].sort(), [index]);
  /** Sets one criterion; the empty string clears it. */
  function set<K extends keyof PluginFilter>(key: K, value: string): void {
    const next = { ...filter };
    if (value === '') delete next[key];
    else next[key] = value as PluginFilter[K];
    onFilter(next);
  }

  return (
    <>
      <div className="toolbar">
        <label>
          Search plugins{' '}
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
            {PLUGIN_KINDS.map((kind) => (
              <option key={kind} value={kind}>
                {kind}
              </option>
            ))}
          </select>
        </label>
        <label>
          Firmware{' '}
          <select value={filter.firmware ?? ''} onChange={(e) => set('firmware', e.target.value)}>
            <option value="">any</option>
            {PLUGIN_FIRMWARE.map((id) => (
              <option key={id} value={id}>
                {firmwareLabel(id)}
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
      <p className="muted" role="status">
        {shown.length === index.plugins.length
          ? plural(index.plugins.length, 'plugin', 'plugins')
          : `${shown.length} of ${plural(index.plugins.length, 'plugin', 'plugins')}`}
      </p>
      {shown.length === 0 && <p>No plugins match these filters.</p>}
      <div className="pack-grid">
        {shown.map((plugin) => (
          <PluginCard
            key={plugin.id}
            plugin={plugin}
            onOpen={onOpen}
            focus={plugin.id === returnTo}
            renderActions={renderActions}
          />
        ))}
      </div>
    </>
  );
}

/** The facts shown on a card and above a manual: who made it, which version, what it needs. */
function PluginFacts({ plugin }: { plugin: PluginIndexEntry }) {
  return (
    <>
      <p className="muted">
        by {plugin.author} · version {plugin.version}
      </p>
      <p className="badges">
        <span className="badge">{plugin.kind}</span>
        <span className="badge licence">{plugin.license}</span>
        <span className="badge firmware">{firmwareLabel(plugin.firmware)}</span>
      </p>
      {plugin.requires && <p>Needs {plugin.requires}.</p>}
      <p>{plugin.description}</p>
      {plugin.credits && <p className="muted">{plugin.credits}</p>}
      {plugin.source && (
        <p>
          <a href={plugin.source} target="_blank" rel="noopener noreferrer">
            Source code<span className="visually-hidden"> of {plugin.name}</span>
          </a>
        </p>
      )}
    </>
  );
}

interface PluginCardProps {
  plugin: PluginIndexEntry;
  onOpen: (id: string) => void;
  focus: boolean;
  renderActions: PluginsProps['renderActions'];
}

function PluginCard({ plugin, onOpen, focus, renderActions }: PluginCardProps) {
  const headingId = useId();
  const manualRef = useRef<HTMLButtonElement>(null);
  // Back from the manual returns focus to the button that opened it.
  useEffect(() => {
    if (focus) manualRef.current?.focus();
  }, [focus]);
  return (
    <article className="pack-card" aria-labelledby={headingId}>
      <img
        className="plugin-screenshot"
        src={pluginFileUrl(base(), plugin.screenshot)}
        alt={`The ${plugin.name} screen on the OP-1`}
        loading="lazy"
      />
      <h3 id={headingId}>{plugin.name}</h3>
      <PluginFacts plugin={plugin} />
      <p>
        <button ref={manualRef} type="button" aria-label={`Read the manual of ${plugin.name}`} onClick={() => onOpen(plugin.id)}>
          Read the manual
        </button>
      </p>
      <PluginDownload plugin={plugin} renderActions={renderActions} />
    </article>
  );
}

/**
 * Download (the file checked against the index first) and the actions later phases add, with the
 * safety note for hundekraft plugins beside them.
 */
function PluginDownload({ plugin, renderActions }: { plugin: PluginIndexEntry; renderActions: PluginsProps['renderActions'] }) {
  const [working, setWorking] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const noteId = useId();
  const hundekraft = plugin.firmware === 'op1/hundekraft';

  async function download(): Promise<void> {
    setWorking(true);
    setError(null);
    try {
      const bytes = await fetchPluginBytes(base(), plugin);
      saveBlob(new Blob([bytes as Uint8Array<ArrayBuffer>], { type: 'application/octet-stream' }), plugin.file);
    } catch (e) {
      setError(`Download failed: ${messageOf(e)}`);
    } finally {
      setWorking(false);
    }
  }

  return (
    <div className="plugin-actions">
      <p>
        <button
          type="button"
          className="primary"
          onClick={() => void download()}
          disabled={working}
          aria-label={`Download ${plugin.file}`}
          aria-describedby={hundekraft ? noteId : undefined}
        >
          Download {plugin.file} ({formatSize(plugin.sizeBytes)})
        </button>{' '}
        {renderActions?.(plugin)}
      </p>
      {hundekraft && (
        <p className="notice" id={noteId}>
          A plugin runs as firmware code on the OP-1. Install it with the{' '}
          <a href={WORKSHOP_URL} target="_blank" rel="noopener noreferrer">
            hundekraft workshop
          </a>
          , and remove an older copy of the plugin there before installing a new one.
        </p>
      )}
      {working && <p role="status">Downloading…</p>}
      {error && (
        <p role="alert" className="error">
          {error}
        </p>
      )}
    </div>
  );
}

interface PluginManualProps {
  plugin: PluginIndexEntry;
  renderActions: PluginsProps['renderActions'];
  onBack: () => void;
}

function PluginManual({ plugin, renderActions, onBack }: PluginManualProps) {
  const [manual, setManual] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [attempt, setAttempt] = useState(0);
  const headingRef = useRef<HTMLHeadingElement>(null);

  useEffect(() => headingRef.current?.focus(), []);
  useEffect(() => {
    let live = true;
    fetchManual(base(), plugin).then(
      (text) => {
        if (!live) return;
        setManual(text);
        setError(null);
      },
      (e: unknown) => {
        if (live) setError(messageOf(e));
      },
    );
    return () => {
      live = false;
    };
  }, [plugin, attempt]);

  return (
    <div className="pack-detail">
      <p>
        <button type="button" onClick={onBack}>
          Back to plugins
        </button>
      </p>
      <h3 ref={headingRef} tabIndex={-1}>
        {plugin.name}
      </h3>
      <PluginFacts plugin={plugin} />
      <PluginDownload plugin={plugin} renderActions={renderActions} />
      <section className="panel" aria-label={`Manual of ${plugin.name}`}>
        {error && (
          <div role="alert" className="error">
            <p>Could not load the manual: {error}</p>
            <button type="button" onClick={() => setAttempt(attempt + 1)}>
              Try again
            </button>
          </div>
        )}
        {manual === null && !error && <p role="status">Loading the manual…</p>}
        {manual !== null && <Markdown source={manual} folderUrl={pluginFolderUrl(base(), plugin.id)} headingOffset={3} />}
      </section>
    </div>
  );
}
