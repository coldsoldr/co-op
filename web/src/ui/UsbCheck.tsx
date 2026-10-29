import { useEffect, useRef, useState } from 'react';
import { checkDevice, type CheckResult } from '../core/usbmidi/check';
import type { InventoryRecord } from '../core/usbmidi/inventory';
import { getProfile } from '../core/profiles/registry';
import { messageOf } from '../core/util';
import { Dialog } from './Dialog';
import { rememberProfile } from './remembered-profile';
import type { Device } from './useDevice';
import { openOp1Port } from './webmidi';

const HUNDEKRAFT = 'op1/hundekraft';
const hundekraftLabel = getProfile(HUNDEKRAFT)?.label ?? 'OP-1 (hundekraft)';

export const DISCONNECTED = 'The OP-1 was disconnected during the check. Plug it in again (not in disk mode) and check again.';
const CLOSED = 'The check was closed.';

type State =
  | { step: 'idle' }
  | { step: 'checking' }
  | { step: 'done'; result: CheckResult }
  | { step: 'failed'; message: string };

const SOURCE_LABELS: Record<string, string> = { builtin: 'built-in', knl: '.knl', op1p: '.op1p' };

const stateText = (record: InventoryRecord): string =>
  `${record.state ?? 'unknown'}${record.reason ? ` (${record.reason})` : ''}`;

/**
 * The read-only USB MIDI check: opens the OP-1's MIDI port, asks hundekraft 3.x what it is and what
 * plugins its last boot loaded, then closes the port. `timeoutMs` shortens the wait for each reply.
 */
export function UsbCheck({ device, onClose, timeoutMs }: { device: Device; onClose: () => void; timeoutMs?: number }) {
  const [state, setState] = useState<State>({ step: 'idle' });
  const [chosen, setChosen] = useState<string | null>(null);
  const controller = useRef<AbortController | null>(null);
  const { setInstalledPlugins } = device;

  // Closing the panel stops a check in flight, which closes the port.
  useEffect(() => () => controller.current?.abort(new Error(CLOSED)), []);

  async function check(): Promise<void> {
    controller.current?.abort(new Error(CLOSED));
    const current = new AbortController();
    controller.current = current;
    setState({ step: 'checking' });
    setChosen(null);
    try {
      const port = await openOp1Port(current.signal);
      const unwatch = port.watch((connected) => {
        if (!connected) current.abort(new Error(DISCONNECTED));
      });
      try {
        const result = await checkDevice(port, { signal: current.signal, ...(timeoutMs ? { timeoutMs } : {}) });
        if (current.signal.aborted) return;
        setInstalledPlugins(result.kind === 'kennel' ? result.inventory : null);
        setState({ step: 'done', result });
      } finally {
        unwatch();
        port.close();
      }
    } catch (e) {
      // A newer check, or closing the panel, took over.
      if (controller.current !== current || messageOf(current.signal.reason) === CLOSED) return;
      setState({ step: 'failed', message: current.signal.aborted ? messageOf(current.signal.reason) : messageOf(e) });
    }
  }

  function chooseHundekraft(): void {
    if (device.source) {
      void device.setProfile(HUNDEKRAFT);
      setChosen(`The library now uses ${hundekraftLabel}.`);
    } else {
      rememberProfile(HUNDEKRAFT);
      setChosen(`The next drive you open starts on ${hundekraftLabel}.`);
    }
  }

  const checking = state.step === 'checking';
  return (
    <Dialog title="Check the OP-1 over USB" onClose={onClose}>
      <div className="usb-check">
        <p>
          This asks a hundekraft 3.x OP-1 which firmware it runs and which plugins it loaded, over USB MIDI. It only
          reads; nothing on the OP-1 changes.
        </p>
        <p className="notice" role="note">
          The OP-1 has no MIDI while it is in disk mode. Leave disk mode first (eject the drive on the computer, then
          leave disk mode on the OP-1) and keep it plugged in by USB. You can open the drive again afterwards.
        </p>

        {state.step === 'checking' && <p role="status">Checking the OP-1…</p>}
        {state.step === 'failed' && (
          <p role="alert" className="error">
            {state.message}
          </p>
        )}
        {state.step === 'done' && state.result.kind === 'silent' && (
          <div role="status">
            <p>
              <strong>Firmware: no answer.</strong> The OP-1’s MIDI port is there, but it did not answer the hundekraft
              check. It runs TE’s own firmware, op1r, or hundekraft 2.7 or older (none of these answer), or it was busy
              restarting: wait for the tape screen and check again.
            </p>
          </div>
        )}
        {state.step === 'done' && state.result.kind === 'kennel' && (
          <KennelResult result={state.result} chosen={chosen} busy={device.busy} onUse={chooseHundekraft} />
        )}

        <div className="dialog-buttons">
          <button type="button" onClick={onClose}>
            Close
          </button>
          <button type="button" className="primary" onClick={() => void check()} disabled={checking}>
            {state.step === 'idle' ? 'Check' : 'Check again'}
          </button>
        </div>
      </div>
    </Dialog>
  );
}

function KennelResult({
  result,
  chosen,
  busy,
  onUse,
}: {
  result: Extract<CheckResult, { kind: 'kennel' }>;
  chosen: string | null;
  busy: boolean;
  onUse: () => void;
}) {
  const { hello, inventory, off } = result;
  const bootOnly = off ? off.boot.filter((id) => !off.stored.includes(id)) : [];
  const nextOnly = off ? off.stored.filter((id) => !off.boot.includes(id)) : [];
  return (
    <div role="status">
      <p>
        <strong>Firmware: hundekraft 3.x (kennel protocol {hello.version})</strong>
      </p>
      <p>
        <button type="button" className="primary" onClick={onUse} disabled={busy || chosen !== null}>
          Use {hundekraftLabel}
        </button>{' '}
        {chosen && <span className="muted">{chosen}</span>}
      </p>
      {!inventory && (
        <p>This build does not report its plugins. hundekraft 3.0.3 and later do.</p>
      )}
      {inventory?.presets && (
        <p>
          Synth presets: {inventory.presets.count} of {inventory.presets.cap} (reported by the OP-1)
        </p>
      )}
      {inventory && inventory.records.length === 0 && <p>The OP-1 reports no plugins.</p>}
      {inventory && inventory.records.length > 0 && (
        <table className="plugins">
          <caption>Plugins and built-ins the OP-1 found at its last boot</caption>
          <thead>
            <tr>
              <th scope="col">Name</th>
              <th scope="col">Kind</th>
              <th scope="col">Source</th>
              <th scope="col">Version</th>
              <th scope="col">Author</th>
              <th scope="col">State</th>
            </tr>
          </thead>
          <tbody>
            {inventory.records.map((record, i) => (
              <tr key={`${record.path || record.name}-${i}`} data-plugin={record.name}>
                <td>{record.name}</td>
                <td>{record.kind ?? 'unreadable'}</td>
                <td>{record.source ? SOURCE_LABELS[record.source] : 'unknown'}</td>
                <td>{record.version}</td>
                <td>{record.author}</td>
                <td>{stateText(record)}</td>
              </tr>
            ))}
          </tbody>
        </table>
      )}
      {off && (
        <p>
          Switched off: {off.stored.length > 0 ? off.stored.join(', ') : 'nothing'}
          {nextOnly.length > 0 && ` (${nextOnly.join(', ')} from the next boot)`}
          {bootOnly.length > 0 && ` (${bootOnly.join(', ')} stays off until the next boot)`}
        </p>
      )}
    </div>
  );
}
