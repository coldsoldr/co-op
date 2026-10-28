import { useEffect, useRef, useState, type ChangeEvent, type DragEvent, type InputHTMLAttributes } from 'react';
import { decodePcm } from '../core/codec/patch';
import type { LibraryEntry } from '../core/library/library';
import { exportChangeSet } from '../core/library/changeset';
import { filesFromDataTransfer } from '../core/store/drop-store';
import { profiles } from '../core/profiles/registry';
import { playPcm } from './audio';
import { BackupPanel } from './BackupPanel';
import { saveBlob, zipBlob } from './download';
import {
  compatView,
  dateStamp,
  displayName,
  formatSize,
  groupByFolder,
  kindMismatchText,
  rootOf,
} from './format';
import { PatchActions } from './PatchActions';
import { useDevice, type Device } from './useDevice';

/** React does not type webkitdirectory; it passes the attribute through to the DOM. */
const folderInputProps = { webkitdirectory: '', directory: '' } as InputHTMLAttributes<HTMLInputElement>;


export function Library() {
  const device = useDevice();
  return (
    <section aria-labelledby="library-heading">
      <h2 id="library-heading">Device library</h2>
      {device.source ? <Connected device={device} /> : <ConnectPanel device={device} />}
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

  function onDrop(event: DragEvent<HTMLDivElement>): void {
    event.preventDefault();
    setDragging(false);
    // filesFromDataTransfer takes the entries synchronously, before the DataTransfer is emptied.
    void filesFromDataTransfer(event.dataTransfer).then(({ files, skipped }) => device.openDrop(files, skipped));
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
          <button type="button" className="primary" onClick={() => void device.connect()} disabled={device.loading}>
            Connect OP-1
          </button>{' '}
          {device.remembered && (
            <button type="button" onClick={() => void device.reconnect()} disabled={device.loading}>
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
        className={dragging ? 'dropzone dragging' : 'dropzone'}
        onDragOver={(e) => {
          e.preventDefault();
          setDragging(true);
        }}
        onDragLeave={() => setDragging(false)}
        onDrop={onDrop}
      >
        <p>Drop the OP-1 drive folder here to open a read-only copy, or</p>
        <label>
          Choose the OP-1 drive folder{' '}
          <input type="file" multiple onChange={onPick} {...folderInputProps} data-testid="folder-input" />
        </label>
      </div>
      {device.loading && <p role="status">Opening…</p>}
    </div>
  );
}

function Connected({ device }: { device: Device }) {
  const source = device.source;
  const [playing, setPlaying] = useState<string | null>(null);
  const stopRef = useRef<(() => void) | null>(null);

  useEffect(() => () => stopRef.current?.(), []);

  if (!source) return null;
  const staged = source.staged;

  function togglePlay(entry: LibraryEntry): void {
    stopRef.current?.();
    stopRef.current = null;
    if (playing === entry.path) {
      setPlaying(null);
      return;
    }
    if (!entry.patch) return;
    const stop = playPcm(decodePcm(entry.patch), entry.patch.comm.sampleRate, () => {
      if (stopRef.current === stop) {
        stopRef.current = null;
        setPlaying(null);
      }
    });
    stopRef.current = stop;
    setPlaying(entry.path);
  }

  function downloadChanges(): void {
    if (!staged) return;
    saveBlob(zipBlob(exportChangeSet(staged.changes())), `op1-changes-${dateStamp(new Date())}.zip`);
  }

  const groups = groupByFolder(device.entries);

  return (
    <>
      <div className="source-bar">
        <p>
          Source: <strong>{source.store.label || 'dropped folder'}</strong>
          {staged ? ' (read-only copy)' : ''}{' '}
          <button type="button" onClick={device.disconnect}>
            Disconnect
          </button>
        </p>
        <p>
          <label>
            Profile{' '}
            <select value={device.profile.id} onChange={(e) => void device.setProfile(e.target.value)}>
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

      {device.recovered.length > 0 && (
        <div className="notice" role="status">
          <p>An earlier write was interrupted; these files were put back:</p>
          <ul className="file-list">
            {device.recovered.map((path) => (
              <li key={path}>
                <code>{path}</code>
              </li>
            ))}
          </ul>
        </div>
      )}

      {device.progress && (
        <p role="status">
          Reading patches: {device.progress.done} of {device.progress.total}
        </p>
      )}
      {!device.loading && device.entries.length === 0 && <p>No patches found in the synth and drum folders.</p>}

      {device.entries.length > 0 && (
        <table className="library">
          <caption>
            {device.entries.length} patch file(s) on {source.store.label || 'the drive'}
          </caption>
          <thead>
            <tr>
              <th scope="col">Name</th>
              <th scope="col">File</th>
              <th scope="col">Kind</th>
              <th scope="col">Size</th>
              <th scope="col">Compatibility</th>
              <th scope="col">Actions</th>
            </tr>
          </thead>
          {groups.map((group) => (
            <tbody key={group.folder}>
              <tr className="group">
                <th scope="rowgroup" colSpan={6}>
                  {group.folder}
                </th>
              </tr>
              {group.entries.map((entry) => (
                <Row
                  key={entry.path}
                  entry={entry}
                  device={device}
                  staged={staged !== null}
                  playing={playing === entry.path}
                  onTogglePlay={() => togglePlay(entry)}
                />
              ))}
            </tbody>
          ))}
        </table>
      )}

      <BackupPanel store={source.store} profile={device.profile} staged={staged !== null} onRestored={device.refresh} />
    </>
  );
}

interface RowProps {
  entry: LibraryEntry;
  device: Device;
  staged: boolean;
  playing: boolean;
  onTogglePlay: () => void;
}

function Row({ entry, device, staged, playing, onTogglePlay }: RowProps) {
  const mismatch = kindMismatchText(entry);
  const nonStandard = entry.patch !== null && !entry.patch.op1Canonical;
  return (
    <tr data-path={entry.path}>
      <th scope="row" className="name">
        {displayName(entry)}
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
      <td>
        <code>{entry.fileName}</code>
      </td>
      <td>{entry.kind}</td>
      <td>{formatSize(entry.sizeBytes)}</td>
      <td>
        <CompatBadge entry={entry} />
      </td>
      <td>
        <PatchActions
          entry={entry}
          folders={device.folders[rootOf(entry)] ?? []}
          playing={playing}
          onTogglePlay={onTogglePlay}
          run={device.run}
          staged={staged}
        />
      </td>
    </tr>
  );
}

function CompatBadge({ entry }: { entry: LibraryEntry }) {
  const view = compatView(entry);
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
