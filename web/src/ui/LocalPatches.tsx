import { useState, type FormEvent } from 'react';
import { CatalogueError, InstallError, installPatches } from '../core/catalogue/catalogue';
import { OP1_FLASH_BYTES } from '../core/library/drive-space';
import { inFolder, type LocalPatch } from '../core/library/local-library';
import { findUnsupported } from '../core/profiles/guard';
import { joinPath } from '../core/store/store';
import { messageOf } from '../core/util';
import { useConfirmUnsupported } from './ConfirmUnsupported';
import { Dialog } from './Dialog';
import { formatSize } from './format';
import { PatchKeyboard } from './PatchKeyboard';
import { clickedControl, transferRow } from './AuditionBar';
import { BulkTools } from './BulkTools';
import { LocalActions } from './LocalActions';
import { localPlace } from './places';
import type { Audition } from './useAudition';
import type { LocalLibrary } from './useLocalLibrary';
import { usePatchPreview } from './usePatchPreview';
import type { Device } from './useDevice';

type SendKind = 'synth' | 'drum';

interface LocalPatchesProps {
  device: Device;
  local: LocalLibrary;
  /** The folder shown, relative to the local folder. */
  folder: string;
  audition: Audition;
  active: boolean;
  confirmPlugins?: (files: readonly Uint8Array[], profile: Device['profile']) => Promise<boolean>;
}

const count = (n: number): string => `${n} ${n === 1 ? 'patch' : 'patches'}`;
const sendable = (patch: LocalPatch): boolean => patch.error === null && (patch.kind === 'synth' || patch.kind === 'drum');

/** One folder of the local library in the Library tab: tick patches and send them to the OP-1. */
export function LocalPatches({ device, local, folder, audition, active, confirmPlugins }: LocalPatchesProps) {
  const [selected, setSelected] = useState<ReadonlySet<string>>(() => new Set());
  const [choosing, setChoosing] = useState(false);
  const [targets, setTargets] = useState<Partial<Record<SendKind, string>>>({});
  const [working, setWorking] = useState(false);
  const [outcome, setOutcome] = useState<{ ok: boolean; message: string } | null>(null);
  const { confirmUnsupported, dialog } = useConfirmUnsupported();
  const preview = usePatchPreview(active);

  const patches = local.patches.filter((p) => inFolder(p, folder));
  const chosen = patches.filter((p) => selected.has(p.path) && sendable(p));
  const kinds = (['synth', 'drum'] as const).filter((kind) => chosen.some((p) => p.kind === kind));
  const rootOf = (kind: SendKind): string | undefined => device.profile.layout.patchRoots.find((r) => r.kind === kind)?.path;
  const foldersOf = (kind: SendKind): string[] => device.folders[rootOf(kind) ?? ''] ?? [];
  /** The folder a kind goes to: the one picked, else the first that is not the preset folder. */
  const targetOf = (kind: SendKind): string | undefined => {
    const options = foldersOf(kind);
    const picked = targets[kind];
    return picked !== undefined && options.includes(picked) ? picked : (options.find((n) => n.toLowerCase() !== 'user') ?? options[0]);
  };
  const allOn = patches.length > 0 && patches.every((p) => selected.has(p.path));
  const ticked = patches.filter((p) => selected.has(p.path)).map((p) => p.path);

  function toggle(path: string, on: boolean): void {
    setSelected((prev) => {
      const next = new Set(prev);
      if (on) next.add(path);
      else next.delete(path);
      return next;
    });
  }

  function toggleAll(on: boolean): void {
    setSelected(on ? new Set(patches.map((p) => p.path)) : new Set());
  }

  async function send(event: FormEvent): Promise<void> {
    event.preventDefault();
    const store = local.folder;
    const key = device.source?.key;
    const profile = device.profile;
    if (!store) return;
    const folderFor = Object.fromEntries(
      kinds.map((kind) => [kind, joinPath(rootOf(kind) ?? '', targetOf(kind) ?? '')]),
    ) as Record<SendKind, string>;
    setChoosing(false);
    setWorking(true);
    setOutcome(null);
    let installed: string[] = [];
    try {
      const size = chosen.reduce((total, p) => total + p.sizeBytes, 0);
      if (device.usedBytes !== null && device.usedBytes + size > OP1_FLASH_BYTES) {
        throw new CatalogueError(`${formatSize(size)} of patches will not fit on the OP-1`);
      }
      const read = await Promise.all(chosen.map(async (p) => ({ patch: p, bytes: await store.read(p.path) })));
      const risky = findUnsupported(
        read.map(({ patch, bytes }) => ({ path: patch.path, bytes })),
        profile,
      );
      if (!(await confirmUnsupported(risky, profile))) return;
      if (confirmPlugins && !(await confirmPlugins(read.map((r) => r.bytes), profile))) return;
      const files = read.map(({ patch, bytes }) => ({ file: `${patch.kind}/${patch.fileName}`, bytes }));
      await device.run(async (drive, source) => {
        if (source.key !== key) throw new CatalogueError('the drive changed; send again');
        if (device.currentProfile().id !== profile.id) throw new CatalogueError('the profile changed; send again');
        installed = (await installPatches(drive, profile.id, 'local', files, (kind) => folderFor[kind])).installed;
      });
      const where = device.source?.staged ? ' Download them from the changes above to copy them onto the OP-1.' : '';
      setOutcome({ ok: true, message: `Sent ${count(installed.length)} to the OP-1.${where}` });
      setSelected(new Set());
    } catch (e) {
      const sent = e instanceof InstallError ? e.installed.length : installed.length;
      setOutcome({ ok: false, message: `Send failed after ${count(sent)}: ${messageOf(e)}` });
    } finally {
      setWorking(false);
    }
  }

  return (
    <>
      <BulkTools
        place={localPlace(local)}
        selected={ticked}
        onClear={() => setSelected(new Set())}
        onDone={() => {
          setSelected(new Set());
          preview.close();
        }}
      >
        {device.source && (
          <button
            type="button"
            className="primary"
            onClick={() => setChoosing(true)}
            disabled={chosen.length === 0 || working || device.busy}
          >
            Send to OP-1 ({chosen.length})
          </button>
        )}
      </BulkTools>
      {preview.error && (
        <p role="alert" className="error">
          {preview.error}
        </p>
      )}
      {preview.open && active && (
        <PatchKeyboard name={preview.open.name} player={preview.open.player} kit={preview.open.kit} onClose={preview.close} />
      )}
      {outcome && (
        <p role={outcome.ok ? 'status' : 'alert'} className={outcome.ok ? undefined : 'error'}>
          {outcome.message}
        </p>
      )}
      {patches.length === 0 && <p className="empty">No patches in this folder.</p>}
      {patches.length > 0 && (
        <div className="table-scroll">
          <table className="library patches">
            <caption>
              {patches.length} patch file(s) in {folder || local.folder?.label}
            </caption>
            <thead>
              <tr>
                <th scope="col" className="select">
                  <span className="visually-hidden">Select</span>
                  <input
                    type="checkbox"
                    aria-label="Select all local patches here"
                    checked={allOn}
                    onChange={(e) => toggleAll(e.target.checked)}
                  />
                </th>
                <th scope="col">Name</th>
                <th scope="col">Kind</th>
                <th scope="col">Size</th>
                <th scope="col" className="play">
                  <span className="visually-hidden">Preview</span>
                </th>
                <th scope="col">
                  <span className="visually-hidden">Actions</span>
                </th>
              </tr>
            </thead>
            <tbody>
              {patches.map((patch) => {
                const onOp1 = audition.ready && (patch.kind === 'synth' || patch.engine === 'drum' || patch.engine === 'dbox') && patch.error === null;
                const id = `local:${patch.path}`;
                const load = (): Promise<Uint8Array> => {
                  if (!local.folder) throw new Error('the folder is no longer open');
                  return local.folder.read(patch.path);
                };
                const start = (): void => void audition.play(id, patch.name, { load });
                const playing = preview.playing === id;
                const noPreview = patch.preview === false;
                return (
                <tr
                  key={patch.path}
                  data-local-path={patch.path}
                  {...(onOp1 ? transferRow(audition, id) : {})}
                  onClick={(e) => {
                    if (onOp1 && !clickedControl(e.target)) start();
                  }}
                >
                  <td className="select">
                    <input
                      type="checkbox"
                      aria-label={`Select ${patch.path}`}
                      checked={selected.has(patch.path)}
                      onChange={(e) => toggle(patch.path, e.target.checked)}
                    />
                  </td>
                  <th scope="row" className="name">
                    {onOp1 ? (
                      <button type="button" className="row-pick" onClick={start} aria-label={`Audition ${patch.name}`}>
                        {patch.name}
                      </button>
                    ) : (
                      patch.name
                    )}
                    <code className="file">{patch.path}</code>
                    {(patch.engine || patch.fx) && <span className="sound">{[patch.engine, patch.fx].filter(Boolean).join(' · ')}</span>}
                    {patch.error && <span className="row-error">{patch.error}</span>}
                  </th>
                  <td className="kind">{patch.kind}</td>
                  <td className="size">{patch.error ? '' : formatSize(patch.sizeBytes)}</td>
                  <td className="play">
                    <button
                      type="button"
                      className={`play-key ${patch.kind}`}
                      disabled={noPreview || patch.error !== null}
                      title={noPreview ? 'No preview: the OP-1 only stored its voice clip' : 'Preview in the browser'}
                      aria-label={`${playing ? 'Stop' : 'Preview'} ${patch.name}`}
                      aria-pressed={playing}
                      onClick={() => void preview.toggle(id, patch.name, async () => load())}
                    >
                      <svg aria-hidden="true" viewBox="0 0 24 24">
                        <path d={playing ? 'M6 5h12v14H6z' : 'M7 4l13 8-13 8z'} />
                      </svg>
                    </button>
                  </td>
                  <td>
                    <LocalActions patch={patch} local={local} />
                  </td>
                </tr>
                );
              })}
            </tbody>
          </table>
        </div>
      )}

      {choosing && (
        <Dialog title={`Send ${count(chosen.length)} to the OP-1`} onClose={() => setChoosing(false)}>
          <form onSubmit={(e) => void send(e)}>
            {kinds.map((kind) => (
              <label key={kind}>
                {kind} patches go to
                <select
                  value={targetOf(kind) ?? ''}
                  onChange={(e) => setTargets((prev) => ({ ...prev, [kind]: e.target.value }))}
                  required
                >
                  {foldersOf(kind).map((name) => (
                    <option key={name} value={name}>
                      {name}
                    </option>
                  ))}
                </select>
              </label>
            ))}
            <p className="muted">Files are copied unchanged. A name already in the folder gets a number added. New folders are made in the left column.</p>
            <div className="dialog-buttons">
              <button type="button" onClick={() => setChoosing(false)}>
                Cancel
              </button>
              <button type="submit" className="primary">
                Send
              </button>
            </div>
          </form>
        </Dialog>
      )}
      {dialog}
    </>
  );
}
