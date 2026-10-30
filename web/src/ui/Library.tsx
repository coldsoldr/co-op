import {
  memo,
  useCallback,
  useEffect,
  useId,
  useMemo,
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
import { installedNames } from '../core/plugins/requirements';
import { playPcm } from './audio';
import { BackupPanel, type BackupControls } from './BackupPanel';
import { ConfirmDialog } from './Dialog';
import { saveBlob, zipBlob } from './download';
import { compatView, dateStamp, folderOf, formatSize, kindMismatchText, rootOf, soundLine } from './format';
import { Folders } from './Folders';
import { OpusPlugins } from './OpusPlugins';
import { usePluginPrompt } from './PluginPrompt';
import { TraitsPopover, type PluginRequirements } from './TraitsPopover';
import { UsbCheck } from './UsbCheck';
import { PatchActions } from './PatchActions';
import { UploadDialog, uploadUnavailable } from './Op1FunUpload';
import type { Op1Fun } from './useOp1Fun';
import {
  displayName,
  filterRows,
  sortRows,
  type LibraryRow,
  type SortDirection,
  type SortKey,
} from './rows';
import type { Device, Operation } from './useDevice';
import { joinPath } from '../core/store/store';
import { messageOf } from '../core/util';

/** React does not type webkitdirectory; it passes the attribute through to the DOM. */
const folderInputProps = { webkitdirectory: '', directory: '' } as InputHTMLAttributes<HTMLInputElement>;

/**
 * `device` is the connection shared with the other tabs. `active` is false while the Library tab
 * is hidden; playback stops then. `op1fun` is the op1.fun account, for the Upload action.
 */
export function Library({ device, active = true, op1fun }: { device: Device; active?: boolean; op1fun: Op1Fun }) {
  const [usbCheck, setUsbCheck] = useState(false);
  const openUsbCheck = useCallback(() => setUsbCheck(true), []);
  return (
    <section aria-labelledby="library-heading">
      {device.source ? (
        <Connected
          key={device.source.key}
          device={device}
          active={active}
          op1fun={op1fun}
          onCheckUsb={openUsbCheck}
        />
      ) : (
        <ConnectPanel device={device} onCheckUsb={openUsbCheck} />
      )}
      {usbCheck && <UsbCheck device={device} onClose={() => setUsbCheck(false)} />}
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

function ConnectPanel({ device, onCheckUsb }: { device: Device; onCheckUsb: () => void }) {
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
      <div className="hero">
        <div>
          <div className="eyebrow">01 / NOT CONNECTED</div>
          <h2 id="library-heading">Connect your OP–1</h2>
        </div>
        <div className="keys">
          <Key label="Check over USB" colour="blue" onClick={onCheckUsb} />
        </div>
      </div>
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
      <p className="muted">
        To see which firmware and plugins the OP-1 runs, plug it in normally (not in disk mode) and use Check over USB.
      </p>
      {readingDrop && <p role="status">Reading dropped folder…</p>}
      {device.loading && <p role="status">Opening…</p>}
    </div>
  );
}

interface Sort {
  key: SortKey;
  direction: SortDirection;
}

const COLUMNS: ReadonlyArray<{ key: SortKey; label: string }> = [
  { key: 'name', label: 'Name' },
  { key: 'kind', label: 'Kind' },
  { key: 'size', label: 'Size' },
  { key: 'compat', label: 'Compatibility' },
];

/** A round key like the OP-1's, with its coloured dot. */
function Key({
  label,
  colour,
  onClick,
  disabled,
}: {
  label: string;
  colour: 'blue' | 'green' | 'grey' | 'orange';
  onClick: () => void;
  disabled?: boolean;
}) {
  return (
    <button type="button" className="key" onClick={onClick} disabled={disabled}>
      <span className="key-cap">
        <span className={`key-dot ${colour}`} />
      </span>
      {label}
    </button>
  );
}

function Connected({
  device,
  active,
  op1fun,
  onCheckUsb,
}: {
  device: Device;
  active: boolean;
  op1fun: Op1Fun;
  onCheckUsb: () => void;
}) {
  const source = device.source;
  const [playing, setPlaying] = useState<string | null>(null);
  const stopRef = useRef<(() => void) | null>(null);
  const playingRef = useRef<string | null>(null);
  /** Bumped by every play click and by hiding the tab, so a read that finishes late never starts. */
  const playToken = useRef(0);
  const activeRef = useRef(active);
  /** The folder chosen in the left column; null shows every patch. */
  const [place, setPlace] = useState<string | null>(null);
  const backup = useRef<BackupControls>(null);
  const [query, setQuery] = useState('');
  const [sort, setSort] = useState<Sort>({ key: 'name', direction: 'ascending' });
  const [selected, setSelected] = useState<ReadonlySet<string>>(() => new Set());
  const [confirmDisconnect, setConfirmDisconnect] = useState(false);
  const [uploading, setUploading] = useState<LibraryRow | null>(null);
  const uploadNoteId = useId();
  const uploadWhy = uploadUnavailable(op1fun);
  const { confirmPlugins, dialog: pluginDialog } = usePluginPrompt(device);
  /** What the rows' details need to name the plugins a patch needs. */
  const plugins = useMemo(
    () => ({ index: device.pluginIndex, firmwareId: device.profile.id, loaded: installedNames(device.installedPlugins) }),
    [device.pluginIndex, device.profile.id, device.installedPlugins],
  );
  const store = source?.store ?? null;

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

  // A folder gone from the drive (removed, or after a refresh) falls back to all patches.
  const folder =
    place !== null && Object.entries(device.folders).some(([root, names]) => names.some((n) => joinPath(root, n) === place))
      ? place
      : null;
  const inView = folder === null ? device.rows : device.rows.filter((r) => folderOf(r) === folder);
  const where = folder === null ? `on ${source.store.label || 'the drive'}` : `in ${folder}`;
  const shown = filterRows(inView, query);
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
      onUpload={setUploading}
      uploadUnavailable={uploadWhy}
      uploadNoteId={uploadNoteId}
      selected={selected.has(row.path)}
      onSelect={toggleSelected}
      plugins={plugins}
    />
  );

  return (
    <>
      <div className="hero">
        <div>
          <div className="eyebrow">01 / CONNECTED</div>
          <h2 id="library-heading">OP–1 drive</h2>
          <div className="pills">
            <label className="pill firmware-pill">
              <span className="dot ok" />
              <span className="visually-hidden">Profile</span>
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
            </label>
            <span className="pill quiet">{device.detection}</span>
            <span className="pill quiet">
              {source.store.label || 'dropped folder'}
              {staged ? ' (read-only copy)' : ''}
            </span>
            <button type="button" onClick={() => void device.refresh()} disabled={device.busy}>
              Refresh
            </button>
          </div>
        </div>
        <div className="keys">
          <Key label="Check over USB" colour="blue" onClick={onCheckUsb} />
          <Key label="Back up" colour="green" onClick={() => backup.current?.backUp()} disabled={device.busy} />
          <Key label="Restore" colour="grey" onClick={() => backup.current?.restore()} disabled={device.busy} />
          <Key label="Disconnect" colour="orange" onClick={requestDisconnect} disabled={device.busy} />
        </div>
      </div>

      {device.suggestion && (
        <div className="notice suggestion" role="status">
          <p>
            {device.suggestion.result.reason}. Switch profile?
          </p>
          <button
            type="button"
            className="primary"
            onClick={() => void device.setProfile(device.suggestion?.profile.id ?? device.profile.id)}
            disabled={device.busy}
          >
            Switch to {device.suggestion.profile.label}
          </button>{' '}
          <button type="button" onClick={device.dismissSuggestion}>
            Keep {device.profile.label}
          </button>
          <p className="muted">
            Patch contents show where sounds were made, not which firmware the OP-1 runs now.
          </p>
        </div>
      )}

      {staged && (
        <div className="notice readonly-notice" role="note">
          <p>
            Read-only source: a dropped folder cannot be written to, so changes are collected here. Download them as a
            zip and follow CHANGES.txt inside to apply them to the OP-1.
          </p>
          <button type="button" className="primary" onClick={downloadChanges} disabled={device.changeCount === 0}>
            Download changes ({device.changeCount})
          </button>
          {device.changeCount > 0 && !device.unsavedChanges && <p className="muted">All changes are downloaded.</p>}
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

      {uploadWhy && device.rows.length > 0 && (
        <p id={uploadNoteId} className="muted">
          Upload: {uploadWhy}
        </p>
      )}

      <div className="library-layout">
        <Folders device={device} selected={folder} onSelect={setPlace} />
        <div className="library-main">
          <div className="library-bar">
            <div className="eyebrow">03 / {(folder ?? 'all patches').replace('/', ' · ').toUpperCase()}</div>
            <label className="search">
              <span className="visually-hidden">Search</span>
              <svg aria-hidden="true" viewBox="0 0 24 24">
                <circle cx="11" cy="11" r="7" />
                <path d="M21 21l-4.3-4.3" />
              </svg>
              <input
                type="search"
                value={query}
                onChange={(e) => setQuery(e.target.value)}
                placeholder="Search patches"
              />
            </label>
          </div>

          {device.progress && (
            <p role="status">
              Reading patches: {device.progress.done} of {device.progress.total}
            </p>
          )}
          {!device.loading && inView.length === 0 && (
            <p className="empty">{folder ? 'No patches in this folder yet.' : 'No patches found in the synth and drum folders.'}</p>
          )}

          {inView.length > 0 && (
            <>
              <div className="table-scroll">
                <table className="library patches">
                  <caption>
                    {shown.length === inView.length
                      ? `${inView.length} patch file(s) ${where}`
                      : `${shown.length} of ${inView.length} patch file(s) ${where}`}
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
                      <th scope="col" className="play">
                        <span className="visually-hidden">Play</span>
                      </th>
                      {COLUMNS.map((column) => (
                        <th key={column.key} scope="col" aria-sort={sort.key === column.key ? sort.direction : 'none'}>
                          <button type="button" className="sort" onClick={() => sortBy(column.key)}>
                            {column.label}
                            <span aria-hidden="true">
                              {sort.key === column.key ? (sort.direction === 'ascending' ? ' ▲' : ' ▼') : ''}
                            </span>
                          </button>
                        </th>
                      ))}
                      <th scope="col">
                        <span className="visually-hidden">Actions</span>
                      </th>
                    </tr>
                  </thead>
                  <tbody>{sortRows(shown, sort.key, sort.direction).map(rowFor)}</tbody>
                </table>
              </div>
              {shown.length === 0 && <p>No patches match “{query}”.</p>}
            </>
          )}
        </div>
      </div>

      {device.profile.firmware === 'opus' && (
        <OpusPlugins store={source.store} settledCount={device.settledCount} />
      )}

      <BackupPanel
        controls={backup}
        store={source.store}
        profile={device.profile}
        staged={staged !== null}
        selectedPaths={selectedPaths}
        busy={device.busy}
        exclusive={device.exclusive}
        run={device.run}
        confirmPlugins={confirmPlugins}
      />
      {pluginDialog}

      {uploading && (
        <UploadDialog
          entry={uploading}
          op1fun={op1fun}
          read={(path) => device.exclusive(() => source.store.read(path))}
          onClose={() => setUploading(null)}
        />
      )}

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
  onUpload: (entry: LibraryRow) => void;
  uploadUnavailable: string | null;
  uploadNoteId: string;
  selected: boolean;
  onSelect: (path: string, on: boolean) => void;
  plugins: PluginRequirements;
}

const Row = memo(function Row({
  row,
  folders,
  playing,
  onTogglePlay,
  run,
  staged,
  busy,
  onUpload,
  uploadUnavailable,
  uploadNoteId,
  selected,
  onSelect,
  plugins,
}: RowProps) {
  const mismatch = kindMismatchText(row);
  const nonStandard = row.info !== null && !row.info.canonical;
  const [error, setError] = useState<string | null>(null);
  const sound = soundLine(row);

  function play(): void {
    setError(null);
    onTogglePlay(row.path).catch((e: unknown) => setError(`cannot play: ${messageOf(e)}`));
  }

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
      <td className="play">
        <button
          type="button"
          className={`play-key ${row.kind}`}
          onClick={play}
          disabled={!row.info}
          aria-label={`${playing ? 'Stop' : 'Play'} ${row.fileName}`}
          aria-pressed={playing}
        >
          <svg aria-hidden="true" viewBox="0 0 24 24">
            <path d={playing ? 'M6 5h12v14H6z' : 'M7 4l13 8-13 8z'} />
          </svg>
        </button>
      </td>
      <th scope="row" className="name">
        {displayName(row)}
        <code className="file">{row.fileName}</code>
        {sound && <span className="sound">{sound}</span>}
        {error && (
          <span role="alert" className="row-error">
            {error}
          </span>
        )}
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
      <td className="kind">{row.kind}</td>
      <td className="size">{formatSize(row.sizeBytes)}</td>
      <td className="compat">
        <CompatBadge row={row} />
        {row.info?.traits && <TraitsPopover row={row} traits={row.info.traits} plugins={plugins} />}
      </td>
      <td className="more-cell">
        <PatchActions
          entry={row}
          folders={folders ?? NO_FOLDERS}
          onError={setError}
          run={run}
          staged={staged}
          busy={busy}
          onUpload={onUpload}
          uploadUnavailable={uploadUnavailable}
          uploadNoteId={uploadNoteId}
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
