import { useRef, useState, type ChangeEvent } from 'react';
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
import { findUnsupported } from '../core/profiles/guard';
import { useConfirmUnsupported } from './ConfirmUnsupported';
import { saveBlob } from './download';
import { dateStamp } from './format';
import type { Operation, Progress } from './useDevice';
import { messageOf } from '../core/util';

interface BackupPanelProps {
  store: DeviceStore;
  profile: DeviceProfile;
  /** True when restored files are staged for download rather than written to the drive. */
  staged: boolean;
  /** Paths chosen in the library for "Back up selected". */
  selectedPaths: readonly string[];
  /** True while another action is queued or running. */
  busy: boolean;
  /** Queues a read-only task so it never overlaps an operation. */
  exclusive: <T>(task: () => Promise<T>) => Promise<T>;
  /** Queues a write, then reloads the library. */
  run: (op: Operation, touched?: readonly string[]) => Promise<void>;
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

/** Raised inside a restore when it would write a file the unsupported-patch guard did not see. */
class NeedsGuard extends Error {
  constructor(readonly diff: BackupDiff) {
    super('the drive changed since the patches were checked; please confirm again');
  }
}

/** Raised inside a restore when the drive changed since the overwrite was confirmed. */
class NeedsConfirmation extends Error {
  constructor(readonly diff: BackupDiff) {
    super('the drive changed since you confirmed; please confirm again');
  }
}

/** Paths among `selected` that still differ from the backup, and those of them that would overwrite. */
function plan(diff: BackupDiff, selected: ReadonlySet<string>): { paths: string[]; overwrites: string[] } {
  const chosen = diff.entries.filter((e) => selected.has(e.path) && e.status !== 'same');
  return { paths: chosen.map((e) => e.path), overwrites: chosen.filter((e) => e.status === 'changed').map((e) => e.path) };
}

/** Download a backup zip of the patch folders (all or selected), and restore selected files from one. */
export function BackupPanel({ store, profile, staged, selectedPaths, busy, exclusive, run }: BackupPanelProps) {
  const [backupProgress, setBackupProgress] = useState<Progress | null>(null);
  const [backupError, setBackupError] = useState<string | null>(null);
  const [pending, setPending] = useState<Pending | null>(null);
  const [restoreError, setRestoreError] = useState<string | null>(null);
  const [restoreProgress, setRestoreProgress] = useState<Progress | null>(null);
  const [confirming, setConfirming] = useState<string[] | null>(null);
  const [outcome, setOutcome] = useState<Outcome | null>(null);
  const { confirmUnsupported, dialog } = useConfirmUnsupported();
  /** The paths the unsupported-patch guard last judged; a restore writing any other path asks again. */
  const guarded = useRef<ReadonlySet<string>>(new Set());

  async function download(paths?: readonly string[]): Promise<void> {
    setBackupError(null);
    setBackupProgress({ done: 0, total: 0 });
    try {
      const blob = await exclusive(() =>
        createBackup(store, profile, {
          onProgress: (done, total) => setBackupProgress({ done, total }),
          ...(paths ? { paths } : {}),
        }),
      );
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
      const next = await exclusive(async () => {
        const backup = await readBackup(file);
        const diff = await diffBackup(store, backup);
        return { backup, diff };
      });
      const selected = new Set(next.diff.entries.filter((e) => e.status !== 'same').map((e) => e.path));
      setPending({ ...next, selected });
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

  /** Compares the selection with the drive as it is now, then confirms overwrites or applies. */
  async function requestApply(current: Pending | null = pending): Promise<void> {
    if (!current) return;
    setRestoreError(null);
    try {
      const diff = await exclusive(() => diffBackup(store, current.backup));
      setPending({ ...current, diff });
      const { paths, overwrites } = plan(diff, current.selected);
      const risky = findUnsupported(
        paths.flatMap((path) => {
          const bytes = current.backup.files.get(path);
          return bytes ? [{ path, bytes }] : [];
        }),
        profile,
      );
      if (!(await confirmUnsupported(risky, profile))) return;
      guarded.current = new Set(paths);
      if (overwrites.length > 0) setConfirming(overwrites);
      else await apply([], current);
    } catch (e) {
      setRestoreError(messageOf(e));
    }
  }

  /** Applies the restore, re-checking the drive first; `confirmed` are the overwrites the user accepted. */
  async function apply(confirmed: readonly string[], current: Pending | null = pending): Promise<void> {
    setConfirming(null);
    if (!current) return;
    const { backup, selected } = current;
    let restoring: string[] = [];
    setRestoreError(null);
    try {
      await run(
        async (target) => {
          const diff = await diffBackup(target, backup);
          const { paths, overwrites } = plan(diff, selected);
          if (paths.some((path) => !guarded.current.has(path))) throw new NeedsGuard(diff);
          if (overwrites.some((path) => !confirmed.includes(path))) throw new NeedsConfirmation(diff);
          restoring = paths;
          let done = 0;
          setRestoreProgress({ done, total: paths.length });
          await applyRestore(target, backup, paths, () => setRestoreProgress({ done: ++done, total: paths.length }));
        },
        [...selected],
      );
      setOutcome({
        message: staged
          ? `Restored ${restoring.length} file(s) into the staged copy; download the changes to apply them.`
          : `Restored ${restoring.length} file(s).`,
        restored: restoring,
        failed: false,
      });
      setPending(null);
    } catch (e) {
      if (e instanceof NeedsGuard) {
        void requestApply({ backup, selected, diff: e.diff });
      } else if (e instanceof NeedsConfirmation) {
        setPending({ backup, selected, diff: e.diff });
        setConfirming(plan(e.diff, selected).overwrites);
      } else {
        const restored = e instanceof RestoreError ? e.restored : [];
        setOutcome({ message: messageOf(e), restored, failed: true });
      }
    } finally {
      setRestoreProgress(null);
    }
  }

  const changedOrNew = pending ? pending.diff.entries.filter((e) => e.status !== 'same') : [];
  const chosenCount = pending ? plan(pending.diff, pending.selected).paths.length : 0;
  const sameCount = pending ? pending.diff.entries.length - changedOrNew.length : 0;
  const profileMismatch = pending && pending.backup.manifest.profile !== profile.id;
  const working = busy || backupProgress !== null || restoreProgress !== null;

  return (
    <section className="panel" aria-labelledby="backup-heading">
      <h3 id="backup-heading">Backup and restore</h3>
      <p>
        <button type="button" onClick={() => void download()} disabled={working}>
          Download backup
        </button>{' '}
        <button
          type="button"
          onClick={() => void download(selectedPaths)}
          disabled={working || selectedPaths.length === 0}
        >
          Back up selected ({selectedPaths.length})
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
          <input type="file" accept=".zip,application/zip" onChange={(e) => void pick(e)} disabled={working} />
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
            <button type="button" onClick={() => void requestApply()} disabled={chosenCount === 0 || working}>
              Restore {chosenCount} file(s)
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
          files={confirming}
          confirmLabel="Restore and overwrite"
          onCancel={() => setConfirming(null)}
          onConfirm={() => void apply(confirming)}
        />
      )}
      {dialog}
    </section>
  );
}
