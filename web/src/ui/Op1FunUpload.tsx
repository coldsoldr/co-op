import { useState, type FormEvent } from 'react';
import { MAX_DESCRIPTION } from '../core/op1fun/client';
import { OP1FUN_LICENSES, type Op1FunPatch } from '../core/op1fun/types';
import { Dialog } from './Dialog';
import { op1FunMessage, type Op1Fun } from './useOp1Fun';
import type { LibraryRow } from './rows';

/** Why uploading is not possible right now, or null when it is. */
export function uploadUnavailable(op1fun: Op1Fun): string | null {
  if (!op1fun.workerUrl) return 'Uploading to op1.fun needs the co-op Worker, which this build of co-op does not have.';
  if (!op1fun.client) return 'Save your op1.fun email and token on the op1.fun tab to upload patches.';
  return null;
}

interface UploadDialogProps {
  entry: LibraryRow;
  op1fun: Op1Fun;
  /** Reads the file's current bytes (staged changes included). */
  read: (path: string) => Promise<Uint8Array>;
  onClose: () => void;
}

const defaultName = (entry: LibraryRow): string => entry.info?.name || entry.fileName.replace(/\.aiff?$/i, '');

/** Uploads one library patch, as its raw file bytes, to the saved op1.fun account. */
export function UploadDialog({ entry, op1fun, read, onClose }: UploadDialogProps) {
  const [name, setName] = useState(() => defaultName(entry));
  const [description, setDescription] = useState('');
  const [license, setLicense] = useState('');
  const [isPublic, setPublic] = useState(true);
  const [sending, setSending] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [uploaded, setUploaded] = useState<Op1FunPatch | null>(null);
  // While uploading, Escape and Cancel do nothing, so the result is never lost.
  const close = sending ? () => undefined : onClose;

  async function submit(event: FormEvent): Promise<void> {
    event.preventDefault();
    const client = op1fun.client;
    if (!client || name.trim() === '') return;
    setSending(true);
    setError(null);
    try {
      const bytes = await read(entry.path);
      setUploaded(await client.uploadPatch({ name: name.trim(), description: description.trim(), license, public: isPublic, bytes }));
    } catch (e) {
      setError(`Upload failed: ${op1FunMessage(e)}`);
    } finally {
      setSending(false);
    }
  }

  return (
    <Dialog title={`Upload ${entry.fileName} to op1.fun`} onClose={close}>
      {uploaded ? (
        <>
          <p role="status">
            Uploaded.{' '}
            {uploaded.selfUrl ? (
              <a href={uploaded.selfUrl} target="_blank" rel="noreferrer">
                Open {uploaded.name || name} on op1.fun
              </a>
            ) : (
              `op1.fun saved it as patch ${uploaded.id}.`
            )}
          </p>
          <div className="dialog-buttons">
            <button type="button" onClick={onClose} autoFocus>
              Close
            </button>
          </div>
        </>
      ) : (
        <form onSubmit={(e) => void submit(e)}>
          <label>
            Name
            <input type="text" value={name} onChange={(e) => setName(e.target.value)} required autoFocus />
          </label>
          <label>
            Description
            <textarea value={description} onChange={(e) => setDescription(e.target.value)} rows={3} maxLength={MAX_DESCRIPTION} />
          </label>
          <label>
            Licence
            <select value={license} onChange={(e) => setLicense(e.target.value)}>
              {OP1FUN_LICENSES.map((value) => (
                <option key={value} value={value}>
                  {value || 'No licence'}
                </option>
              ))}
            </select>
          </label>
          <label>
            <input type="checkbox" checked={isPublic} onChange={(e) => setPublic(e.target.checked)} /> Public
          </label>
          <p className="muted">
            The file is sent unchanged through the co-op Worker, which passes it and your op1.fun email and token to
            op1.fun and keeps nothing.
          </p>
          {sending && <p role="status">Uploading…</p>}
          {error && (
            <p role="alert" className="error">
              {error}
            </p>
          )}
          <div className="dialog-buttons">
            <button type="button" onClick={close} disabled={sending}>
              Cancel
            </button>
            <button type="submit" className="primary" disabled={sending || name.trim() === ''}>
              Upload
            </button>
          </div>
        </form>
      )}
    </Dialog>
  );
}
