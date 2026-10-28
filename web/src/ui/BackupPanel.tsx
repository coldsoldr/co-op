import { useState, type ChangeEvent } from 'react';
import type { DeviceProfile } from '../core/profiles/types';
import {
  RestoreError,
  applyRestore,
  createBackup,
  diffBackup,
  readBackup,
  type Backup,
  type BackupDiff,
} from '../core/library/backup';
import type { DeviceStore } from '../core/store/store';
import { ConfirmDialog } from './Dialog';
import { saveBlob } from './download';
import { dateStamp } from './format';
import type { Progress } from './useDevice';

interface BackupPanelProps {
  store: DeviceStore;
  profile: DeviceProfile;
  /** True when restored files are staged for download rather than written to the drive. */
  staged: boolean;
  /** Called after a restore wrote anything, so the library reloads. */
  onRestored: () => Promise<void>;
}

interface Pending {
  backup: Backup;
  diff: BackupDiff;
  selected: Set<string>;
}

interface Outcome {
  message: string;
  restored: string[];
  failed: boolean;
}

const messageOf = (error: unknown): string => (error instanceof Error ? error.message : String(error));

/** Download a backup zip of the patch folders, and restore selected files from one. */
export function BackupPanel({ store, profile, staged, onRestored }: BackupPanelProps) {
  const [backupProgress, setBackupProgress] = useState<Progress | null>(null);
  const [backupError, setBackupError] = useState<string | null>(null);
  const [pending, setPending] = useState<Pending | null>(null);
  const [restoreError, setRestoreError] = useState<string | null>(null);
  const [restoreProgress, setRestoreProgress] = useState<Progress | null>(null);
  const [confirming, setConfirming] = useState(false);
  const [outcome, setOutcome] = useState<Outcome | null>(null);

  async function download(): Promise<void> {
    setBackupError(null);
    setBackupProgress({ done: 0, total: 0 });
    try {
      const blob = await createBackup(store, profile, {
        onProgress: (done, total) => setBackupProgress({ done, total }),
      });
      saveBlob(blob, `op1-backup-${dateStamp(new Date())}.zip`);
    } catch (e) {
      setBackupError(messageOf(e));
    } finally {
      setBackupProgress(null);
    }
  }

  async function pick(event: ChangeEvent<HTMLInputElement>): Promise<void> {
    const input = event.currentTarget;
    const file = input.files?.[0];
    setPending(null);
    setOutcome(null);
    setRestoreError(null);
    if (!file) return;
    try {
      const backup = await readBackup(file);
      const diff = await diffBackup(store, backup);
      const selected = new Set(diff.entries.filter((e) => e.status !== 'same').map((e) => e.path));
      setPending({ backup, diff, selected });
    } catch (e) {
      setRestoreError(messageOf(e));
    } finally {
      input.value = '';
    }
  }

  function toggle(path: string, on: boolean): void {
    if (!pending) return;
    const selected = new Set(pending.selected);
    if (on) selected.add(path);
    else selected.delete(path);
    setPending({ ...pending, selected });
  }

  const chosen = pending ? pending.diff.entries.filter((e) => pending.selected.has(e.path)) : [];
  const overwrites = chosen.filter((e) => e.status === 'changed').map((e) => e.path);

  function requestApply(): void {
    if (overwrites.length > 0) setConfirming(true);
    else void apply();
  }

  async function apply(): Promise<void> {
    setConfirming(false);
    if (!pending) return;
    const paths = chosen.map((e) => e.path);
    let done = 0;
    setRestoreProgress({ done, total: paths.length });
    setRestoreError(null);
    try {
      await applyRestore(store, pending.backup, paths, () => setRestoreProgress({ done: ++done, total: paths.length }));
      setOutcome({
        message: staged
          ? `Restored ${paths.length} file(s) into the staged copy; download the changes to apply them.`
          : `Restored ${paths.length} file(s).`,
        restored: paths,
        failed: false,
      });
      setPending(null);
    } catch (e) {
      const restored = e instanceof RestoreError ? e.restored : [];
      setOutcome({ message: messageOf(e), restored, failed: true });
    } finally {
      setRestoreProgress(null);
      await onRestored();
    }
  }

  const changedOrNew = pending ? pending.diff.entries.filter((e) => e.status !== 'same') : [];
  const sameCount = pending ? pending.diff.entries.length - changedOrNew.length : 0;
  const profileMismatch = pending && pending.backup.manifest.profile !== profile.id;

  return (
    <section className="panel" aria-labelledby="backup-heading">
      <h3 id="backup-heading">Backup and restore</h3>
      <p>
        <button type="button" onClick={() => void download()} disabled={backupProgress !== null}>
          Download backup
        </button>{' '}
        {backupProgress && (
          <span role="status">
            Backing up {backupProgress.done} of {backupProgress.total || '…'} files
          </span>
        )}
      </p>
      {backupError && (
        <p role="alert" className="error">
          Backup failed: {backupError}
        </p>
      )}

      <p>
        <label>
          Restore from backup{' '}
          <input type="file" accept=".zip,application/zip" onChange={(e) => void pick(e)} />
        </label>
      </p>
      {restoreError && (
        <p role="alert" className="error">
          {restoreError}
        </p>
      )}

      {pending && (
        <div className="restore">
          <p>
            Backup of <strong>{pending.backup.manifest.label || 'a drive'}</strong> made{' '}
            {new Date(pending.backup.manifest.createdAt).toLocaleString()}.
          </p>
          {profileMismatch && (
            <p className="warning">
              This backup was made with profile {pending.backup.manifest.profile}, not {profile.id}.
            </p>
          )}
          {changedOrNew.length === 0 ? (
            <p>Everything in this backup is already on the drive; there is nothing to restore.</p>
          ) : (
            <fieldset>
              <legend>Files to restore</legend>
              <ul className="checklist">
                {changedOrNew.map((e) => (
                  <li key={e.path}>
                    <label>
                      <input
                        type="checkbox"
                        checked={pending.selected.has(e.path)}
                        onChange={(ev) => toggle(e.path, ev.target.checked)}
                      />{' '}
                      <code>{e.path}</code> <span className={`tag ${e.status}`}>{e.status}</span>
                    </label>
                  </li>
                ))}
              </ul>
            </fieldset>
          )}
          {sameCount > 0 && <p>{sameCount} file(s) are identical on the drive and are left alone.</p>}
          {pending.diff.onDeviceOnly.length > 0 && (
            <details>
              <summary>{pending.diff.onDeviceOnly.length} file(s) on the drive are not in the backup (kept)</summary>
              <ul className="file-list">
                {pending.diff.onDeviceOnly.map((path) => (
                  <li key={path}>
                    <code>{path}</code>
                  </li>
                ))}
              </ul>
            </details>
          )}
          <p>
            <button type="button" onClick={requestApply} disabled={chosen.length === 0 || restoreProgress !== null}>
              Restore {chosen.length} file(s)
            </button>{' '}
            <button type="button" onClick={() => setPending(null)} disabled={restoreProgress !== null}>
              Cancel
            </button>{' '}
            {restoreProgress && (
              <span role="status">
                Restoring {restoreProgress.done} of {restoreProgress.total}
              </span>
            )}
          </p>
        </div>
      )}

      {outcome && (
        <div role={outcome.failed ? 'alert' : 'status'} className={outcome.failed ? 'error' : 'notice'}>
          <p>{outcome.failed ? `Restore stopped: ${outcome.message}` : outcome.message}</p>
          {outcome.failed && (
            <>
              <p>
                {outcome.restored.length > 0
                  ? 'These files were restored before the failure:'
                  : 'No files were restored before the failure.'}
              </p>
              {outcome.restored.length > 0 && (
                <ul className="file-list">
                  {outcome.restored.map((path) => (
                    <li key={path}>
                      <code>{path}</code>
                    </li>
                  ))}
                </ul>
              )}
            </>
          )}
        </div>
      )}

      {confirming && (
        <ConfirmDialog
          title="Overwrite changed files?"
          message="These files on the drive differ from the backup and will be replaced by the backed-up versions:"
          files={overwrites}
          confirmLabel="Restore and overwrite"
          onCancel={() => setConfirming(false)}
          onConfirm={() => void apply()}
        />
      )}
    </section>
  );
}
