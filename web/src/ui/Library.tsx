import {
  memo,
  useCallback,
  useEffect,
  useRef,
  useState,
  type ChangeEvent,
  type DragEvent,
  type InputHTMLAttributes,
  type ReactNode,
} from 'react';
import { decodePcm, readPatch } from '../core/codec/patch';
import { exportChangeSet } from '../core/library/changeset';
import { filesFromDataTransfer } from '../core/store/drop-store';
import { profiles } from '../core/profiles/registry';
import { playPcm } from './audio';
import { BackupPanel } from './BackupPanel';
import { ConfirmDialog } from './Dialog';
import { saveBlob, zipBlob } from './download';
import { compatView, dateStamp, formatSize, groupByFolder, kindMismatchText, rootOf } from './format';
import { PatchActions } from './PatchActions';
import {
  displayName,
  filterRows,
  sortRows,
  type LibraryRow,
  type SortDirection,
  type SortKey,
} from './rows';
import type { Device, Operation } from './useDevice';

/** React does not type webkitdirectory; it passes the attribute through to the DOM. */
const folderInputProps = { webkitdirectory: '', directory: '' } as InputHTMLAttributes<HTMLInputElement>;

const messageOf = (error: unknown): string => (error instanceof Error ? error.message : String(error));

/**
 * `device` is the connection shared with the other tabs. `active` is false while the Library tab
 * is hidden; playback stops then.
 */
export function Library({ device, active = true }: { device: Device; active?: boolean }) {
  return (
    <section aria-labelledby="library-heading">
      <h2 id="library-heading">Device library</h2>
      {device.source ? (
        <Connected key={device.source.key} device={device} active={active} />
      ) : (
        <ConnectPanel device={device} />
      )}
      {device.error && (
        <p role="alert" className="error">
          {device.error}
        </p>
      )}
      {device.skipped.length > 0 && (
        <div className="warning" role="status">
          <p>These dropped items could not be read and were skipped:</p>
          <ul className="file-list">
            {device.skipped.map((path) => (
              <li key={path}>
                <code>{path}</code>
              </li>
            ))}
          </ul>
        </div>
      )}
    </section>
  );
}

function ConnectPanel({ device }: { device: Device }) {
  const [dragging, setDragging] = useState(false);
  const [readingDrop, setReadingDrop] = useState(false);
  const blocked = device.busy || readingDrop;

  function onDrop(event: DragEvent<HTMLDivElement>): void {
    event.preventDefault();
    setDragging(false);
    if (blocked) return;
    setReadingDrop(true);
    // filesFromDataTransfer takes the entries synchronously, before the DataTransfer is emptied.
    filesFromDataTransfer(event.dataTransfer)
      .then(({ files, skipped }) => device.openDrop(files, skipped))
      .catch((e: unknown) => device.fail(`could not read the dropped folder: ${messageOf(e)}`))
      .finally(() => setReadingDrop(false));
  }

  function onPick(event: ChangeEvent<HTMLInputElement>): void {
    const files = event.currentTarget.files;
    if (files) void device.openInput(files);
  }

  return (
    <div className="connect">
      <p>
        Put the OP-1 in disk mode (shift + COM, then DISK) and plug it in by USB; its drive then shows up like a USB
        stick.
      </p>
      {device.fsAccess ? (
        <p>
          <button type="button" className="primary" onClick={() => void device.connect()} disabled={blocked}>
            Connect OP-1
          </button>{' '}
          {device.remembered && (
            <button type="button" onClick={() => void device.reconnect()} disabled={blocked}>
              Reconnect {device.remembered.name}
            </button>
          )}
        </p>
      ) : (
        <p>
          This browser cannot write to folders, so the drive opens read-only and your changes are collected into a zip
          to copy onto the OP-1. Chrome or Edge can write to the drive directly.
        </p>
      )}
      <div
        className={`dropzone${dragging ? ' dragging' : ''}${blocked ? ' disabled' : ''}`}
        aria-disabled={blocked}
        onDragOver={(e) => {
          e.preventDefault();
          if (!blocked) setDragging(true);
        }}
        onDragLeave={() => setDragging(false)}
        onDrop={onDrop}
      >
        <p>Drop the OP-1 drive folder here to open a read-only copy, or</p>
        <label>
          Choose the OP-1 drive folder{' '}
          <input type="file" multiple onChange={onPick} disabled={blocked} {...folderInputProps} />
        </label>
      </div>
      {readingDrop && <p role="status">Reading dropped folder…</p>}
      {device.loading && <p role="status">Opening…</p>}
    </div>
  );
}

type View = 'folders' | 'all';

interface Sort {
  key: SortKey;
  direction: SortDirection;
}

const COLUMNS: ReadonlyArray<{ key: SortKey; label: string }> = [
  { key: 'name', label: 'Name' },
  { key: 'file', label: 'File' },
  { key: 'kind', label: 'Kind' },
  { key: 'size', label: 'Size' },
  { key: 'compat', label: 'Compatibility' },
];

function Connected({ device, active }: { device: Device; active: boolean }) {
  const source = device.source;
  const [playing, setPlaying] = useState<string | null>(null);
  const stopRef = useRef<(() => void) | null>(null);
  const playingRef = useRef<string | null>(null);
  /** Bumped by every play click and by hiding the tab, so a read that finishes late never starts. */
  const playToken = useRef(0);
  const activeRef = useRef(active);
  const [view, setView] = useState<View>('folders');
  const [query, setQuery] = useState('');
  const [sort, setSort] = useState<Sort>({ key: 'name', direction: 'ascending' });
  const [selected, setSelected] = useState<ReadonlySet<string>>(() => new Set());
  const [confirmDisconnect, setConfirmDisconnect] = useState(false);
  const store = source?.store ?? null;

  useEffect(() => {
    activeRef.current = active;
    if (!active) {
      playToken.current++;
      stopRef.current?.();
    }
  }, [active]);
  useEffect(() => () => stopRef.current?.(), []);

  const togglePlay = useCallback(
    async (path: string): Promise<void> => {
      const token = ++playToken.current;
      const wasPlaying = stopRef.current !== null && playingRef.current === path;
      stopRef.current?.();
      if (wasPlaying || !store) return;
      // Patches are not kept in memory; read the file again to play it.
      const patch = readPatch(await store.read(path));
      // A later click (or hiding the tab) while the file was being read wins.
      if (token !== playToken.current || !activeRef.current) return;
      const stop = playPcm(decodePcm(patch), patch.comm.sampleRate, () => {
        if (stopRef.current === stop) {
          stopRef.current = null;
          playingRef.current = null;
          setPlaying(null);
        }
      });
      stopRef.current = stop;
      playingRef.current = path;
      setPlaying(path);
    },
    [store],
  );

  const toggleSelected = useCallback((path: string, on: boolean) => {
    setSelected((prev) => {
      const next = new Set(prev);
      if (on) next.add(path);
      else next.delete(path);
      return next;
    });
  }, []);

  if (!source) return null;
  const staged = source.staged;

  function downloadChanges(): void {
    if (!staged) return;
    saveBlob(zipBlob(exportChangeSet(staged.changes())), `op1-changes-${dateStamp(new Date())}.zip`);
    device.markDownloaded();
  }

  function requestDisconnect(): void {
    if (device.unsavedChanges) setConfirmDisconnect(true);
    else device.disconnect();
  }

  function sortBy(key: SortKey): void {
    setSort((prev) =>
      prev.key === key
        ? { key, direction: prev.direction === 'ascending' ? 'descending' : 'ascending' }
        : { key, direction: 'ascending' },
    );
  }

  const shown = filterRows(device.rows, query);
  const selectedPaths = device.rows.filter((r) => selected.has(r.path)).map((r) => r.path);
  const allShownSelected = shown.length > 0 && shown.every((r) => selected.has(r.path));
  const pendingPaths = staged ? [...new Set(staged.changes().map((c) => c.path))] : [];

  function selectShown(on: boolean): void {
    setSelected((prev) => {
      const next = new Set(prev);
      for (const row of shown) {
        if (on) next.add(row.path);
        else next.delete(row.path);
      }
      return next;
    });
  }

  const rowFor = (row: LibraryRow): ReactNode => (
    <Row
      key={row.path}
      row={row}
      folders={device.folders[rootOf(row)]}
      playing={playing === row.path}
      onTogglePlay={togglePlay}
      run={device.run}
      staged={staged !== null}
      busy={device.busy}
      selected={selected.has(row.path)}
      onSelect={toggleSelected}
    />
  );

  return (
    <>
      <div className="source-bar">
        <p>
          Source: <strong>{source.store.label || 'dropped folder'}</strong>
          {staged ? ' (read-only copy)' : ''}{' '}
          <button type="button" onClick={() => void device.refresh()} disabled={device.busy}>
            Refresh
          </button>{' '}
          <button type="button" onClick={requestDisconnect} disabled={device.busy}>
            Disconnect
          </button>
        </p>
        <p>
          <label>
            Profile{' '}
            <select
              value={device.profile.id}
              onChange={(e) => void device.setProfile(e.target.value)}
              disabled={device.busy}
            >
              {profiles.map((p) => (
                <option key={p.id} value={p.id}>
                  {p.label}
                </option>
              ))}
            </select>
          </label>{' '}
          <span className="muted">{device.detection}</span>
        </p>
      </div>

      {staged && (
        <div className="notice readonly-notice" role="note">
          <p>
            Read-only source: a dropped folder cannot be written to, so changes are collected here. Download them as a
            zip and follow CHANGES.txt inside to apply them to the OP-1.
          </p>
          <button type="button" className="primary" onClick={downloadChanges} disabled={device.changeCount === 0}>
            Download changes ({device.changeCount})
          </button>
        </div>
      )}

      {(device.recovered.length > 0 || device.recoveryError) && (
        <div className="notice" role="status">
          {device.recovered.length > 0 && (
            <>
              <p>An earlier write was interrupted; these files were put back:</p>
              <ul className="file-list">
                {device.recovered.map((path) => (
                  <li key={path}>
                    <code>{path}</code>
                  </li>
                ))}
              </ul>
            </>
          )}
          {device.recoveryError && <p className="error">{device.recoveryError}</p>}
        </div>
      )}

      {device.progress && (
        <p role="status">
          Reading patches: {device.progress.done} of {device.progress.total}
        </p>
      )}
      {!device.loading && device.rows.length === 0 && <p>No patches found in the synth and drum folders.</p>}

      {device.rows.length > 0 && (
        <>
          <div className="toolbar">
            <fieldset className="view-toggle">
              <legend>View</legend>
              <label>
                <input type="radio" name="view" checked={view === 'folders'} onChange={() => setView('folders')} /> By
                folder
              </label>
              <label>
                <input type="radio" name="view" checked={view === 'all'} onChange={() => setView('all')} /> All
                patches
              </label>
            </fieldset>
            <label>
              Search{' '}
              <input
                type="search"
                value={query}
                onChange={(e) => setQuery(e.target.value)}
                placeholder="patch or file name"
              />
            </label>
          </div>
          <table className="library">
            <caption>
              {shown.length === device.rows.length
                ? `${device.rows.length} patch file(s) on ${source.store.label || 'the drive'}`
                : `${shown.length} of ${device.rows.length} patch file(s) on ${source.store.label || 'the drive'}`}
            </caption>
            <thead>
              <tr>
                <th scope="col" className="select">
                  <span className="visually-hidden">Select</span>
                  <input
                    type="checkbox"
                    aria-label="Select all shown patches"
                    checked={allShownSelected}
                    onChange={(e) => selectShown(e.target.checked)}
                  />
                </th>
                {COLUMNS.map((column) =>
                  view === 'all' ? (
                    <th
                      key={column.key}
                      scope="col"
                      aria-sort={sort.key === column.key ? sort.direction : 'none'}
                    >
                      <button type="button" className="sort" onClick={() => sortBy(column.key)}>
                        {column.label}
                        <span aria-hidden="true">
                          {sort.key === column.key ? (sort.direction === 'ascending' ? ' ▲' : ' ▼') : ''}
                        </span>
                      </button>
                    </th>
                  ) : (
                    <th key={column.key} scope="col">
                      {column.label}
                    </th>
                  ),
                )}
                <th scope="col">Actions</th>
              </tr>
            </thead>
            {view === 'all' ? (
              <tbody>{sortRows(shown, sort.key, sort.direction).map(rowFor)}</tbody>
            ) : (
              groupByFolder(shown).map((group) => (
                <tbody key={group.folder}>
                  <tr className="group">
                    <th scope="rowgroup" colSpan={7}>
                      {group.folder}
                    </th>
                  </tr>
                  {group.rows.map(rowFor)}
                </tbody>
              ))
            )}
          </table>
          {shown.length === 0 && <p>No patches match “{query}”.</p>}
        </>
      )}

      <BackupPanel
        store={source.store}
        profile={device.profile}
        staged={staged !== null}
        selectedPaths={selectedPaths}
        busy={device.busy}
        exclusive={device.exclusive}
        run={device.run}
      />

      {confirmDisconnect && (
        <ConfirmDialog
          title="Discard staged changes?"
          message="These changes have not been downloaded and will be lost if you disconnect:"
          files={pendingPaths}
          confirmLabel="Disconnect and discard"
          onCancel={() => setConfirmDisconnect(false)}
          onConfirm={() => {
            setConfirmDisconnect(false);
            device.disconnect();
          }}
        />
      )}
    </>
  );
}

const NO_FOLDERS: readonly string[] = [];

interface RowProps {
  row: LibraryRow;
  folders: readonly string[] | undefined;
  playing: boolean;
  onTogglePlay: (path: string) => Promise<void>;
  run: (op: Operation, touched?: readonly string[]) => Promise<void>;
  staged: boolean;
  busy: boolean;
  selected: boolean;
  onSelect: (path: string, on: boolean) => void;
}

const Row = memo(function Row({ row, folders, playing, onTogglePlay, run, staged, busy, selected, onSelect }: RowProps) {
  const mismatch = kindMismatchText(row);
  const nonStandard = row.info !== null && !row.info.canonical;
  return (
    <tr data-path={row.path}>
      <td className="select">
        <input
          type="checkbox"
          aria-label={`Select ${row.path}`}
          checked={selected}
          onChange={(e) => onSelect(row.path, e.target.checked)}
        />
      </td>
      <th scope="row" className="name">
        {displayName(row)}
        {(mismatch || nonStandard) && (
          <ul className="details">
            {mismatch && <li className="warning">{mismatch}</li>}
            {nonStandard && (
              <li className="muted" title="Renaming the patch rewrites its metadata in the standard layout">
                non-standard metadata
              </li>
            )}
          </ul>
        )}
      </th>
      <td className="file">
        <code>{row.fileName}</code>
      </td>
      <td className="kind">{row.kind}</td>
      <td className="size">{formatSize(row.sizeBytes)}</td>
      <td className="compat">
        <CompatBadge row={row} />
      </td>
      <td>
        <PatchActions
          entry={row}
          folders={folders ?? NO_FOLDERS}
          playing={playing}
          onTogglePlay={onTogglePlay}
          run={run}
          staged={staged}
          busy={busy}
        />
      </td>
    </tr>
  );
});

function CompatBadge({ row }: { row: LibraryRow }) {
  const view = compatView(row);
  if (view.level === 'warn') {
    return (
      <span className="badge warn" tabIndex={0} title={view.detail ?? undefined}>
        warn
        {view.detail && <span className="badge-reason">: {view.detail}</span>}
      </span>
    );
  }
  return (
    <span className={`badge ${view.level}`} title={view.detail ?? undefined}>
      {view.label}
      {view.detail && ` (${view.detail})`}
    </span>
  );
}
