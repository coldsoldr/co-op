import { useId, useState } from 'react';
import type { PluginIndexEntry } from '@co-op/shared/plugins';
import { copyOpusPlugin, findOpusPluginFile } from '../core/library/mods';
import { fetchPluginBytes } from '../core/plugins/catalogue';
import { op1Opus } from '../core/profiles/op1-opus';
import { messageOf } from '../core/util';
import { ConfirmDialog } from './Dialog';
import type { Device } from './useDevice';

const base = (): string => import.meta.env.BASE_URL;

/** What the last copy did. */
type Outcome = { ok: boolean; message: string };

/**
 * "Copy to mods/" for an opus plugin: downloads and checks the file against the index, asks before
 * replacing a file of the same name, then writes `mods/<file>` as one queued operation. The reload
 * that follows refreshes the Installed opus plugins list. Without an opus drive open the button is
 * disabled and says what is needed.
 */
export function CopyToMods({ plugin, device }: { plugin: PluginIndexEntry; device: Device }) {
  const [working, setWorking] = useState(false);
  const [replacing, setReplacing] = useState<{ path: string; bytes: Uint8Array; key: number } | null>(null);
  const [outcome, setOutcome] = useState<Outcome | null>(null);
  const noteId = useId();
  const source = device.source;
  const opusOpen = source !== null && device.profile.id === op1Opus.id;
  const staged = source?.staged != null;

  async function write(bytes: Uint8Array, key: number, overwrite: boolean): Promise<void> {
    setReplacing(null);
    let path = '';
    try {
      await device.run(async (store, current) => {
        if (current.key !== key) throw new Error('the drive changed; copy again');
        if (device.currentProfile().id !== op1Opus.id) throw new Error('the profile changed; copy again');
        path = await copyOpusPlugin(store, plugin.file, bytes, { overwrite });
      });
      const where = staged ? ' Download the changes from the Library tab to put it on the OP-1.' : '';
      setOutcome({ ok: true, message: `Copied to ${path}.${where}` });
    } catch (e) {
      setOutcome({ ok: false, message: `Copy failed: ${messageOf(e)}` });
    } finally {
      setWorking(false);
    }
  }

  async function copy(): Promise<void> {
    if (!source) return;
    const key = source.key;
    setWorking(true);
    setOutcome(null);
    try {
      const bytes = await fetchPluginBytes(base(), plugin);
      const existing = await device.exclusive(() => findOpusPluginFile(source.store, plugin.file));
      if (existing) {
        setReplacing({ path: existing, bytes, key });
        return;
      }
      await write(bytes, key, false);
    } catch (e) {
      setOutcome({ ok: false, message: `Copy failed: ${messageOf(e)}` });
      setWorking(false);
    }
  }

  return (
    <>
      <button
        type="button"
        onClick={() => void copy()}
        disabled={!opusOpen || working || device.busy}
        aria-label={`Copy ${plugin.file} to mods/`}
        aria-describedby={noteId}
      >
        Copy to mods/
      </button>{' '}
      <span className="muted" id={noteId}>
        {opusOpen
          ? 'opus loads the plugins in mods/ in file-name order.'
          : 'To copy it to mods/, open an opus drive on the Library tab (profile OP-1 (opus)).'}
      </span>
      {working && !replacing && (
        <span role="status" className="muted">
          {' '}
          Copying {plugin.file}…
        </span>
      )}
      {outcome && (
        <span role={outcome.ok ? 'status' : 'alert'} className={outcome.ok ? undefined : 'error'}>
          {' '}
          {outcome.message}
        </span>
      )}
      {replacing && (
        <ConfirmDialog
          title={`Replace ${replacing.path}?`}
          message={`mods/ already has a file named ${plugin.file}. Replacing it overwrites that plugin with ${plugin.name} ${plugin.version}.`}
          files={[replacing.path]}
          confirmLabel="Replace"
          onConfirm={() => void write(replacing.bytes, replacing.key, true)}
          onCancel={() => {
            setReplacing(null);
            setWorking(false);
          }}
        />
      )}
    </>
  );
}
