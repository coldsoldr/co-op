import { useRef, useState } from 'react';
import { checkCoop, idFromSavedName, type HostedPluginEntry } from '@co-op/shared/plugins';
import { fetchPluginBytes } from '../core/plugins/catalogue';
import {
  CoopCaps,
  coopDeletePlugin,
  coopHello,
  coopPutPlugin,
  coopSetPlugin,
  readCoopInventory,
  type CoopHello,
} from '../core/usbmidi/coop';
import type { Inventory, InventoryRecord } from '../core/usbmidi/inventory';
import { createSession, type Session } from '../core/usbmidi/session';
import { messageOf } from '../core/util';
import { formatSize } from './format';
import { openOp1Port } from './webmidi';

const base = (): string => import.meta.env.BASE_URL;

export const NOT_COOP =
  'This OP-1 does not answer as coop. Check it runs the coop firmware and is plugged in normally, not in disk mode.';
export const COOP_TOO_OLD =
  'This OP-1 runs coop, but a build without plugin management. Flash coop e3 or later to manage plugins from here.';
export const POWER_CYCLE = 'Power-cycle the OP-1 (switch it off and on) to apply the change, then read it again.';

/**
 * One conversation with a coop OP-1: open the MIDI port, check HELLO, run `work`, close the port again (Windows lets
 * one program hold a MIDI port at a time). Throws NOT_COOP or COOP_TOO_OLD as plain errors.
 */
export async function withCoop<T>(work: (session: Session, hello: CoopHello) => Promise<T>): Promise<T> {
  const port = await openOp1Port();
  try {
    const session = createSession(port);
    const hello = await coopHello(session).catch(() => null);
    if (!hello) throw new Error(NOT_COOP);
    if (!(hello.caps & CoopCaps.INVENTORY) || !(hello.caps & CoopCaps.FILE_WRITE)) throw new Error(COOP_TOO_OLD);
    return await work(session, hello);
  } finally {
    port.close();
  }
}

interface CoopInstallProps {
  plugin: HostedPluginEntry;
  /** The inventory read after the install, shared with the rest of the app. */
  onInventory: (inventory: Inventory) => void;
}

/** "Install on OP-1" for a coop plugin from the library: the file goes to /yaffs2/plugins; a power cycle loads it. */
export function CoopInstall({ plugin, onInventory }: CoopInstallProps) {
  const [state, setState] = useState<{ busy: boolean; message: string | null; error: boolean }>({ busy: false, message: null, error: false });

  async function install(): Promise<void> {
    setState({ busy: true, message: 'Downloading…', error: false });
    try {
      const bytes = await fetchPluginBytes(base(), plugin);
      const inventory = await withCoop(async (session) => {
        const before = await readCoopInventory(session);
        const there = before.records.some((r) => r.path === `plugins/${plugin.file}`);
        if (there && !window.confirm(`${plugin.name} is already on the OP-1. Replace it?`)) return null;
        await coopPutPlugin(session, plugin.file, bytes, {
          onProgress: (n, total) => setState({ busy: true, message: `Sending ${Math.round((100 * n) / total)} %`, error: false }),
        });
        return readCoopInventory(session);
      });
      if (!inventory) {
        setState({ busy: false, message: null, error: false });
        return;
      }
      onInventory(inventory);
      setState({ busy: false, message: `Installed. ${POWER_CYCLE}`, error: false });
    } catch (e) {
      setState({ busy: false, message: messageOf(e), error: true });
    }
  }

  return (
    <>
      <button type="button" onClick={() => void install()} disabled={state.busy}>
        Install on OP-1
      </button>
      {state.message && (
        <p role={state.error ? 'alert' : 'status'} className={state.error ? 'error' : 'muted'}>
          {state.message}
        </p>
      )}
    </>
  );
}

const STATE_TEXT: Record<string, string> = {
  loaded: 'loaded',
  failed: 'failed to load',
  off: 'switched off',
  pending: 'installed, loads after a power cycle',
  skipped: 'skipped',
};

interface CoopDeviceProps {
  inventory: Inventory | null;
  onInventory: (inventory: Inventory | null) => void;
}

/**
 * The plugins on a coop OP-1, read over USB: each file with what the last boot made of it, switched on or off for
 * the next boot, removable; and a .coop file from this computer installed beside them.
 */
export function CoopDevice({ inventory, onInventory }: CoopDeviceProps) {
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState<{ text: string; error: boolean } | null>(null);
  const [build, setBuild] = useState<string | null>(null);
  const [changed, setChanged] = useState(false);
  const fileInput = useRef<HTMLInputElement>(null);
  const records = (inventory?.records ?? []).filter((r) => r.source === 'coop');

  async function run(work: (session: Session) => Promise<string | null>, change: boolean): Promise<void> {
    setBusy(true);
    setMessage(null);
    try {
      const note = await withCoop(async (session, hello) => {
        setBuild(hello.build);
        const text = await work(session);
        onInventory(await readCoopInventory(session));
        return text;
      });
      if (change) setChanged(true);
      setMessage(note ? { text: note, error: false } : null);
    } catch (e) {
      setMessage({ text: messageOf(e), error: true });
    } finally {
      setBusy(false);
    }
  }

  const fileOf = (r: InventoryRecord): string => r.path.replace(/^plugins\//, '');

  async function installFile(file: File): Promise<void> {
    const bytes = new Uint8Array(await file.arrayBuffer());
    const check = checkCoop(bytes);
    if (!check.info) {
      setMessage({ text: `${file.name} is not a coop plugin: ${check.problems.join('; ')}`, error: true });
      return;
    }
    const target = `${idFromSavedName(check.info.savedName ?? '')}.coop`;
    if (records.some((r) => fileOf(r) === target) && !window.confirm(`${target} is already on the OP-1. Replace it?`)) return;
    await run(async (session) => {
      await coopPutPlugin(session, target, bytes);
      return `Installed ${target} (${formatSize(bytes.length)}).`;
    }, true);
  }

  return (
    <div className="panel coop-device" aria-labelledby="coop-device-heading">
      <h3 id="coop-device-heading">On your OP-1</h3>
      <p className="muted">
        Plug the OP-1 in by USB normally (not in disk mode). co-op reads its plugins over MIDI; every change applies at
        the next power cycle.
      </p>
      <div className="toolbar">
        <button type="button" onClick={() => void run(async () => null, false)} disabled={busy}>
          {inventory ? 'Read again' : 'Read plugins from the OP-1'}
        </button>
        <button type="button" onClick={() => fileInput.current?.click()} disabled={busy}>
          Install a .coop file…
        </button>
        <input
          ref={fileInput}
          type="file"
          accept=".coop"
          hidden
          onChange={(e) => {
            const file = e.target.files?.[0];
            e.target.value = '';
            if (file) void installFile(file);
          }}
        />
        {build && <span className="pill quiet">coop {build}</span>}
      </div>
      {busy && <p role="status">Talking to the OP-1…</p>}
      {message && (
        <p role={message.error ? 'alert' : 'status'} className={message.error ? 'error' : 'muted'}>
          {message.text}
        </p>
      )}
      {changed && <p className="notice">{POWER_CYCLE}</p>}
      {inventory && records.length === 0 && <p>No plugin files on the OP-1.</p>}
      {records.length > 0 && (
        <div className="table-scroll">
          <table className="library coop-plugins">
            <caption className="visually-hidden">Plugin files on the OP-1</caption>
            <thead>
              <tr>
                <th scope="col">Plugin</th>
                <th scope="col">Kind</th>
                <th scope="col">Version</th>
                <th scope="col">Now</th>
                <th scope="col">Next boot</th>
                <th scope="col">
                  <span className="visually-hidden">Actions</span>
                </th>
              </tr>
            </thead>
            <tbody>
              {records.map((r) => {
                const file = fileOf(r);
                return (
                  <tr key={file}>
                    <th scope="row">
                      {r.name.replaceAll('_', ' ')}
                      <div className="mono muted">{file}</div>
                    </th>
                    <td>{r.kind ?? '?'}</td>
                    <td>{r.version || '–'}</td>
                    <td className={r.state === 'failed' ? 'error' : undefined}>
                      {STATE_TEXT[r.state ?? ''] ?? 'unknown'}
                      {r.reason && <div className="muted">{r.reason}</div>}
                    </td>
                    <td>{r.enabled === false ? 'off' : 'on'}</td>
                    <td>
                      <button
                        type="button"
                        disabled={busy}
                        onClick={() =>
                          void run(async (session) => {
                            await coopSetPlugin(session, file, r.enabled === false);
                            return null;
                          }, true)
                        }
                      >
                        {r.enabled === false ? 'Switch on' : 'Switch off'}
                      </button>{' '}
                      <button
                        type="button"
                        disabled={busy}
                        onClick={() => {
                          if (!window.confirm(`Remove ${file} from the OP-1? Sounds that use it play the stock engine or filter until it is back.`)) return;
                          void run(async (session) => {
                            await coopDeletePlugin(session, file);
                            return `Removed ${file}.`;
                          }, true);
                        }}
                      >
                        Remove
                      </button>
                    </td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </div>
      )}
    </div>
  );
}
