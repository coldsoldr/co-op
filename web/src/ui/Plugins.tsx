import { useCallback, useEffect, useId, useMemo, useRef, useState, type ReactNode } from 'react';
import {
  PLUGIN_FIRMWARE,
  PLUGIN_KINDS,
  isHosted,
  pluginBuilds,
  pluginFor,
  type HostedPluginEntry,
  type PluginIndex,
  type PluginIndexEntry,
} from '@co-op/shared/plugins';
import {
  fetchManual,
  fetchPluginBytes,
  fetchPluginIndex,
  filterPlugins,
  groupByAuthor,
  pluginFileUrl,
  pluginFolderUrl,
  type PluginFilter,
} from '../core/plugins/catalogue';
import { getProfile } from '../core/profiles/registry';
import { WORKSHOP_URL } from '../core/usbmidi/install';
import { messageOf } from '../core/util';
import { saveBlob } from './download';
import { formatSize } from './format';
import { Markdown } from './Markdown';
import { PluginSubmit } from './PluginSubmit';

const firmwareLabel = (id: string): string => getProfile(id)?.label ?? id;
const plural = (n: number, one: string, many: string): string => `${n} ${n === 1 ? one : many}`;
const base = (): string => import.meta.env.BASE_URL;

interface PluginsProps {
  /** False while the tab is hidden: the index is not fetched until the tab is first shown. */
  active: boolean;
  /**
   * More actions for a plugin, shown beside Download on its card and manual: "Install over USB"
   * for hundekraft and "Copy to mods/" for opus.
   */
  /** Install actions, for a build with a file co-op hosts; a listed build links out instead. */
  renderActions?: (plugin: HostedPluginEntry) => ReactNode;
  /** The firmware chosen in Settings (a profile id): the list starts filtered to its plugins. */
  firmwareId?: string;
  /** Shown above the list: the plugins on the connected OP-1, for a firmware co-op manages them on (coop). */
  deviceSection?: ReactNode;
  /** Opens the Settings tab, from the note that says which firmware the list follows. */
  onOpenSettings?: () => void;
}

const isPluginFirmware = (id: string | undefined): id is (typeof PLUGIN_FIRMWARE)[number] =>
  id !== undefined && (PLUGIN_FIRMWARE as readonly string[]).includes(id);

/** The plugin library published with the app: browse plugins, read their manuals and download them. */
export function Plugins({ active, renderActions, firmwareId, deviceSection, onOpenSettings }: PluginsProps) {
  const [index, setIndex] = useState<PluginIndex | null>(null);
  const [loadError, setLoadError] = useState<string | null>(null);
  const requested = useRef(false);
  const [filter, setFilter] = useState<PluginFilter>(() => (isPluginFirmware(firmwareId) ? { firmware: firmwareId } : {}));
  // A new choice in Settings filters the list to that firmware again; "any" stays available in the menu. Adjusted
  // while rendering, when the prop changes (react.dev "Adjusting some state when a prop changes").
  const [seenFirmware, setSeenFirmware] = useState(firmwareId);
  if (seenFirmware !== firmwareId) {
    setSeenFirmware(firmwareId);
    const next = { ...filter };
    if (isPluginFirmware(firmwareId)) next.firmware = firmwareId;
    else delete next.firmware;
    setFilter(next);
  }
  const [openId, setOpenId] = useState<string | null>(null);
  /** The plugin whose manual button takes focus when returning to the list. */
  const [returnTo, setReturnTo] = useState<string | null>(null);
  /** Author groups the user has opened or closed; kept here so a manual and back leaves them as they were. */
  const [toggled, setToggled] = useState<ReadonlySet<string>>(() => new Set());
  const [submitting, setSubmitting] = useState(false);
  const submitButton = useRef<HTMLButtonElement>(null);

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

  // A plugin with a file per firmware is shown as the build for the firmware in the Firmware menu,
  // or else the one chosen in Settings: its file, version and needs, and its install action.
  const prefer = filter.firmware ?? (isPluginFirmware(firmwareId) ? firmwareId : undefined);
  const view = useCallback(
    (plugin: PluginIndexEntry): PluginIndexEntry => (prefer === undefined ? null : pluginFor(plugin, prefer)) ?? plugin,
    [prefer],
  );
  // Kept the same object between renders: the manual loads again when its plugin changes.
  const open = useMemo(() => {
    const found = index?.plugins.find((p) => p.id === openId);
    return found ? view(found) : null;
  }, [index, openId, view]);

  return (
    <section aria-labelledby="plugins-heading">
      <div className="packs-header">
        <h2 id="plugins-heading">Plugins</h2>
        {!submitting && (
          <button ref={submitButton} type="button" onClick={() => setSubmitting(true)}>
            Submit a plugin
          </button>
        )}
      </div>
      {submitting && (
        <PluginSubmit
          onBack={() => {
            setSubmitting(false);
            requestAnimationFrame(() => submitButton.current?.focus());
          }}
        />
      )}
      {!submitting && loadError && (
        <div role="alert" className="error">
          <p>Could not load the plugin library: {loadError}</p>
          <button type="button" onClick={load}>
            Try again
          </button>
        </div>
      )}
      {!submitting && !index && !loadError && <p role="status">Loading plugins…</p>}
      {!submitting && index && open && (
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
      {!submitting && !open && firmwareId !== undefined && (
        <p className="muted">
          {isPluginFirmware(firmwareId)
            ? `Showing plugins for ${firmwareLabel(firmwareId)}, the firmware chosen in Settings.`
            : `${firmwareLabel(firmwareId)} runs no plugins. Choose your firmware in Settings, or pick one in the Firmware menu to browse.`}{' '}
          {onOpenSettings && (
            <button type="button" className="link" onClick={onOpenSettings}>
              Settings
            </button>
          )}
        </p>
      )}
      {!submitting && !open && deviceSection}
      {!submitting && index && !open && (
        <PluginList
          index={index}
          filter={filter}
          onFilter={setFilter}
          toggled={toggled}
          onToggle={(author) =>
            setToggled((prev) => {
              const next = new Set(prev);
              if (!next.delete(author)) next.add(author);
              return next;
            })
          }
          onOpen={setOpenId}
          returnTo={returnTo}
          renderActions={renderActions}
          view={view}
        />
      )}
    </section>
  );
}

interface PluginListProps {
  index: PluginIndex;
  filter: PluginFilter;
  onFilter: (filter: PluginFilter) => void;
  toggled: ReadonlySet<string>;
  onToggle: (author: string) => void;
  onOpen: (id: string) => void;
  returnTo: string | null;
  renderActions: PluginsProps['renderActions'];
  /** A plugin as it is shown: the build for the firmware the list follows. */
  view: (plugin: PluginIndexEntry) => PluginIndexEntry;
}

function PluginList({ index, filter, onFilter, toggled, onToggle, onOpen, returnTo, renderActions, view }: PluginListProps) {
  const shown = filterPlugins(index, filter).map(view);
  const groups = groupByAuthor(shown);
  // A search or filter shows what it found. Otherwise several authors start closed, and a lone
  // author in the list as shown starts open, as a closed group would be all there is to see. The
  // firmware is not a search: it is set from Settings for most visitors, and counting it kept every
  // group open with its toggle disabled.
  const filtering = filter.text !== undefined || filter.kind !== undefined || filter.tag !== undefined;
  const startsOpen = groups.length === 1;
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
      {groups.map(({ author, plugins }) => {
        const expanded = filtering || toggled.has(author) !== startsOpen;
        return (
          <section key={author} className="author-group" aria-label={`Plugins by ${author}`}>
            <h3>
              <button type="button" aria-expanded={expanded} disabled={filtering} onClick={() => onToggle(author)}>
                <span>{author}</span>
                <span className="mono">{plural(plugins.length, 'plugin', 'plugins')}</span>
                <svg aria-hidden="true" viewBox="0 0 24 24">
                  <path d="M6 9l6 6 6-6" />
                </svg>
              </button>
            </h3>
            {expanded && (
              <div className="pack-grid">
                {plugins.map((plugin) => (
                  <PluginCard
                    key={plugin.id}
                    plugin={plugin}
                    onOpen={onOpen}
                    focus={plugin.id === returnTo}
                    renderActions={renderActions}
                  />
                ))}
              </div>
            )}
          </section>
        );
      })}
    </>
  );
}

/** The facts shown on a card and above a manual: who made it, which version, what it needs. */
function PluginFacts({ plugin }: { plugin: PluginIndexEntry }) {
  return (
    <>
      <p className="muted">
        by {plugin.author}
        {plugin.version !== undefined && ` · version ${plugin.version}`}
      </p>
      <p className="badges">
        <span className="badge">{plugin.kind}</span>
        {plugin.license !== undefined && <span className="badge licence">{plugin.license}</span>}
        {/* The build shown first, then any other firmware the plugin has a file for. */}
        {[plugin.firmware, ...pluginBuilds(plugin).map((b) => b.firmware).filter((f) => f !== plugin.firmware)].map((id) => (
          <span key={id} className="badge firmware">
            {firmwareLabel(id)}
          </span>
        ))}
      </p>
      {plugin.requires && <p>Needs {plugin.requires}.</p>}
      <p>{plugin.description}</p>
      {plugin.video && (
        <video className="plugin-screenshot" controls preload="none" src={plugin.video} aria-label={`Demo of ${plugin.name}`} />
      )}
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
      <h4 id={headingId}>{plugin.name}</h4>
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
 * Where to get a plugin: a link out for a listed build, else Download and the install actions.
 */
function PluginDownload({ plugin, renderActions }: { plugin: PluginIndexEntry; renderActions: PluginsProps['renderActions'] }) {
  return isHosted(plugin) ? <HostedDownload plugin={plugin} renderActions={renderActions} /> : <ListedLink plugin={plugin} />;
}

/** The host name of a link, for its label; the link itself if it does not parse. */
function hostOf(link: string): string {
  try {
    return new URL(link).hostname;
  } catch {
    return link;
  }
}

/**
 * A listed build: co-op hosts no file for it, so the card links to where its author offers it.
 */
function ListedLink({ plugin }: { plugin: PluginIndexEntry }) {
  if (plugin.link === undefined) return null;
  return (
    <div className="plugin-actions">
      <div className="plugin-buttons">
        <a className="button primary" href={plugin.link} target="_blank" rel="noopener noreferrer">
          Get it from {hostOf(plugin.link)}
          <span className="visually-hidden"> ({plugin.name}, opens a new tab)</span>
        </a>
      </div>
      <p className="muted">Listed here for reference: co-op links to its author's page and hosts no copy.</p>
    </div>
  );
}

/**
 * Download (the file checked against the index first) and the actions later phases add, with the
 * safety note for hundekraft plugins beside them.
 */
function HostedDownload({ plugin, renderActions }: { plugin: HostedPluginEntry; renderActions: PluginsProps['renderActions'] }) {
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
      <div className="plugin-buttons">
        <button
          type="button"
          className="primary"
          onClick={() => void download()}
          disabled={working}
          aria-label={`Download ${plugin.file}`}
          aria-describedby={hundekraft ? noteId : undefined}
        >
          Download {plugin.file} ({formatSize(plugin.sizeBytes)})
        </button>
        {renderActions?.(plugin)}
      </div>
      {hundekraft && (
        <p className="notice" id={noteId}>
          A plugin runs as firmware code on the OP-1. Install it over USB here or with the{' '}
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
