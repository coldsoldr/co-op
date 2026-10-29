import { useEffect, useRef, useState } from 'react';
import type { PluginIndexEntry } from '@co-op/shared/plugins';
import { fetchPluginBytes } from '../core/plugins/catalogue';
import { checkDevice, type CheckResult } from '../core/usbmidi/check';
import { CancelledOnOp1Error, WORKSHOP_URL, installPlugins, type PluginFile } from '../core/usbmidi/install';
import type { Inventory, InventoryRecord } from '../core/usbmidi/inventory';
import { messageOf, sleep } from '../core/util';
import { Dialog } from './Dialog';
import { formatSize } from './format';
import { openOp1Port, waitForOp1Restart } from './webmidi';

export const DISCONNECTED = 'The OP-1 was disconnected during the install. Nothing was installed. Plug it in again (not in disk mode) and try again.';
const CLOSED = 'The install was closed.';
/** The ports went away after the prompt: most likely the restart after T1, whose ok was lost. */
const RESTARTED = 'The OP-1 restarted.';
export const NOT_BACK =
  'The OP-1 restarted but did not answer the check afterwards. Wait for the tape screen, then check it over USB from the Library to see whether the plugin loaded.';

/** How long the check after the restart keeps trying while the OP-1 boots. */
const RECHECK_MS = 60_000;
const RECHECK_EVERY_MS = 2_000;

type Step =
  | { step: 'confirm' }
  | { step: 'working'; status: string }
  | { step: 'sending'; sent: number; total: number }
  | { step: 'prompt' }
  | { step: 'restarting' }
  | { step: 'done'; record: InventoryRecord | null }
  | { step: 'cancelled' }
  | { step: 'failed'; message: string };

const base = (): string => import.meta.env.BASE_URL;

/**
 * Checks the OP-1 again once it is back from its restart. Right after the ports return it may not
 * answer yet (it is still booting), so silence and errors are retried for RECHECK_MS.
 */
async function recheck(signal: AbortSignal): Promise<Extract<CheckResult, { kind: 'kennel' }>> {
  const deadline = Date.now() + RECHECK_MS;
  for (;;) {
    try {
      const port = await openOp1Port(signal);
      try {
        const result = await checkDevice(port, { signal });
        if (result.kind === 'kennel') return result;
      } finally {
        port.close();
      }
    } catch (e) {
      if (signal.aborted) throw e;
    }
    if (Date.now() >= deadline) throw new Error(NOT_BACK);
    await sleep(RECHECK_EVERY_MS, signal);
  }
}

/** The record of the installed file, found by its path or by the plugin id from a file. */
const recordOf = (inventory: Inventory | null, path: string, id: string): InventoryRecord | null =>
  inventory?.records.find((r) => r.path === path || (r.name === id && (r.source === 'knl' || r.source === 'op1p'))) ?? null;

/** "Install over USB" for a hundekraft plugin, and the dialog that does it. */
export function InstallOverUsb({ plugin, onInstalled }: { plugin: PluginIndexEntry; onInstalled: (inventory: Inventory | null) => void }) {
  const [open, setOpen] = useState(false);
  return (
    <>
      <button type="button" onClick={() => setOpen(true)} aria-label={`Install ${plugin.file} over USB`}>
        Install over USB
      </button>
      {open && <PluginInstall plugin={plugin} onInstalled={onInstalled} onClose={() => setOpen(false)} />}
    </>
  );
}

interface PluginInstallProps {
  plugin: PluginIndexEntry;
  /** Called with the inventory the OP-1 reports after its restart. */
  onInstalled: (inventory: Inventory | null) => void;
  onClose: () => void;
}

/**
 * Installs one hundekraft plugin over USB MIDI: explains and confirms, downloads and checks the
 * file, sends it, waits for T1 or T2 on the OP-1, waits for its restart and checks it again. The
 * port is closed on every way out, and closing the dialog stops whatever is running. While the
 * OP-1 shows its prompt the dialog cannot be closed: the answer is T1 or T2 on the OP-1.
 */
export function PluginInstall({ plugin, onInstalled, onClose }: PluginInstallProps) {
  const [state, setState] = useState<Step>({ step: 'confirm' });
  const controller = useRef<AbortController | null>(null);
  const path = `plugins/${plugin.file}`;

  const cancelRef = useRef<HTMLButtonElement>(null);
  const promptRef = useRef<HTMLDivElement>(null);

  useEffect(() => () => controller.current?.abort(new Error(CLOSED)), []);
  // Cancel is the default. This runs after the dialog's showModal, which focuses the first link.
  useEffect(() => cancelRef.current?.focus(), []);
  // The prompt has no buttons here: focus goes to its instruction.
  useEffect(() => {
    if (state.step === 'prompt') promptRef.current?.focus();
  }, [state.step]);

  async function install(): Promise<void> {
    controller.current?.abort(new Error(CLOSED));
    const current = new AbortController();
    controller.current = current;
    const { signal } = current;
    try {
      setState({ step: 'working', status: `Downloading and checking ${plugin.file}…` });
      const file: PluginFile = { path, bytes: await fetchPluginBytes(base(), plugin) };
      signal.throwIfAborted();
      setState({ step: 'working', status: 'Connecting to the OP-1…' });
      const port = await openOp1Port(signal);
      // Its own signal, so a disconnect after the prompt (the restart) can end the install
      // without stopping the wait for the OP-1 to come back.
      const transfer = new AbortController();
      let prompted = false;
      const unwatch = port.watch((connected) => {
        if (!connected) transfer.abort(new Error(prompted ? RESTARTED : DISCONNECTED));
      });
      try {
        await installPlugins(port, [file], {
          signal: AbortSignal.any([signal, transfer.signal]),
          onProgress: (sent, total) => setState({ step: 'sending', sent, total }),
          onPrompt: () => {
            prompted = true;
            setState({ step: 'prompt' });
          },
        });
      } catch (e) {
        if (signal.aborted || messageOf(transfer.signal.reason) !== RESTARTED) {
          throw transfer.signal.aborted ? transfer.signal.reason : e;
        }
      } finally {
        unwatch();
        port.close();
      }
      setState({ step: 'restarting' });
      await waitForOp1Restart(signal);
      const result = await recheck(signal);
      signal.throwIfAborted();
      onInstalled(result.inventory);
      setState({ step: 'done', record: recordOf(result.inventory, path, plugin.id) });
    } catch (e) {
      // Closing the dialog, or a newer try, took over.
      if (controller.current !== current || messageOf(signal.reason) === CLOSED) return;
      if (e instanceof CancelledOnOp1Error) setState({ step: 'cancelled' });
      else setState({ step: 'failed', message: signal.aborted ? messageOf(signal.reason) : messageOf(e) });
    }
  }

  // While the OP-1 shows its prompt only T1 or T2 on it ends the install: a prompt left behind
  // stays up after T1 and applies the next COMMIT from any host without asking again.
  const prompting = state.step === 'prompt';
  const running = state.step === 'working' || state.step === 'sending' || state.step === 'restarting';
  return (
    <Dialog
      title={`Install ${plugin.name} over USB`}
      onClose={() => {
        if (!prompting) onClose();
      }}
    >
      <div className="usb-check">
        <p className="notice" role="note">
          The OP-1 must be out of disk mode and plugged in by USB normally: in disk mode it has no MIDI. Eject the drive
          on the computer and leave disk mode on the OP-1 first.
        </p>
        {state.step === 'confirm' && (
          <>
            <p>This sends one file to the OP-1:</p>
            <ul className="file-list">
              <li>
                <code>{path}</code> ({formatSize(plugin.sizeBytes)})
              </li>
            </ul>
            <p>
              A plugin runs as firmware code on the OP-1. Install only plugins you trust. The OP-1 asks you to press T1 to
              install it, then restarts.
            </p>
            <p>
              If the OP-1 does not start after the restart, switch it off and on: it then starts without plugin files.
              Removing the plugin is done in the{' '}
              <a href={WORKSHOP_URL} target="_blank" rel="noopener noreferrer">
                hundekraft workshop
              </a>
              .
            </p>
          </>
        )}
        {state.step === 'working' && <p role="status">{state.status}</p>}
        {state.step === 'sending' && (
          <>
            <p role="status">
              Sending {path} to the OP-1: {formatSize(state.sent)} of {formatSize(state.total)}
            </p>
            <progress max={state.total} value={state.sent} aria-label={`Sending ${path}`} />
          </>
        )}
        {prompting && (
          <div role="status" ref={promptRef} tabIndex={-1}>
            <p>
              <strong>Press T1 on the OP-1 to install and restart, or T2 to cancel.</strong>
            </p>
            <p className="muted">This window waits for your answer on the OP-1; to cancel, press T2 there.</p>
          </div>
        )}
        {state.step === 'restarting' && (
          <p role="status">The OP-1 is installing and restarting. This can take a minute; keep it plugged in.</p>
        )}
        {state.step === 'done' && <Installed name={plugin.name} record={state.record} />}
        {state.step === 'cancelled' && <p role="status">Cancelled on the OP-1 (T2). Nothing was installed.</p>}
        {state.step === 'failed' && (
          <p role="alert" className="error">
            {state.message}
          </p>
        )}

        <div className="dialog-buttons">
          {!prompting && (
            <button ref={cancelRef} type="button" onClick={onClose}>
              {state.step === 'confirm' || running ? 'Cancel' : 'Close'}
            </button>
          )}
          {state.step === 'confirm' && (
            <button type="button" className="danger" onClick={() => void install()}>
              Install {plugin.file}
            </button>
          )}
          {(state.step === 'failed' || state.step === 'cancelled') && (
            <button type="button" className="primary" onClick={() => void install()}>
              Try again
            </button>
          )}
        </div>
      </div>
    </Dialog>
  );
}

/** What the OP-1 reports for the plugin after its restart. */
function Installed({ name, record }: { name: string; record: InventoryRecord | null }) {
  const reason = record?.reason ? ` (${record.reason})` : '';
  const text =
    !record ? `The OP-1 restarted but does not list ${name}. Nothing seems to have been installed; try again.`
    : record.state === 'loaded' ? `${name} is installed and loaded.`
    : record.state === 'failed' ? `${name} is on the OP-1 but did not load${reason}. Remove it in the hundekraft workshop.`
    : record.state === 'off' ? `${name} is installed but switched off${reason}.`
    : record.state === 'skipped' ? `The OP-1 started without plugin files: its last boot did not reach the tape screen, which a plugin can cause. If it happens again, remove ${name} in the hundekraft workshop.`
    : `${name} is on the OP-1; its state is unknown.`;
  return (
    <p role="status" data-state={record?.state ?? 'missing'}>
      {text}
    </p>
  );
}
