/**
 * Opens the OP-1's USB MIDI port through Web MIDI with SysEx, as a MidiPort for the usbmidi core.
 * Only works while the OP-1 is plugged in normally: in disk mode it shows no MIDI port at all.
 */
import type { MidiPort } from '../core/usbmidi/session';

export interface Op1Port extends MidiPort {
  /** The output port's name, as the system reports it. */
  name: string;
  /** Calls `cb` on every port change with whether both OP-1 ports are still there; returns an unsubscribe. */
  watch(cb: (connected: boolean) => void): () => void;
}

const OP1_NAME = /op-?1/i;
/** An OP-1 Field matches OP1_NAME too, but runs neither hundekraft nor its protocol. */
const FIELD_NAME = /field/i;

export const NO_WEB_MIDI = 'This browser cannot talk to MIDI devices. Use Chrome, Edge or Firefox on a computer to check the OP-1 over USB.';
export const MIDI_REFUSED =
  'MIDI access was refused. Allow MIDI devices (with SysEx) for this site in the browser’s site settings, then check again.';
export const NO_OP1_PORT =
  'No OP-1 MIDI port found. Plug the OP-1 in by USB normally, not in disk mode: in disk mode it has no MIDI. If the OP-1 is plugged in, unplug the cable and plug it in again.';

const abortReason = (signal: AbortSignal): unknown => signal.reason ?? new DOMException('aborted', 'AbortError');

/** Resolves with `promise`, or rejects as soon as the signal aborts. */
function untilAborted<T>(promise: Promise<T>, signal: AbortSignal | undefined): Promise<T> {
  if (!signal) return promise;
  if (signal.aborted) return Promise.reject(abortReason(signal));
  return new Promise((resolve, reject) => {
    const onAbort = (): void => reject(abortReason(signal));
    signal.addEventListener('abort', onAbort, { once: true });
    promise.then(resolve, reject).finally(() => signal.removeEventListener('abort', onAbort));
  });
}

function findOp1<T extends MIDIPort>(ports: Iterable<T>): T | undefined {
  for (const port of ports) {
    const name = port.name ?? '';
    if (port.state !== 'disconnected' && OP1_NAME.test(name) && !FIELD_NAME.test(name)) return port;
  }
  return undefined;
}

/**
 * NO_OP1_PORT plus the ports the browser does show, so a port held by another program (Windows
 * lets one program at a time use a MIDI port) or an unexpected name is visible in the message.
 */
function noOp1Port(access: MIDIAccess): string {
  const names = new Set<string>();
  for (const ports of [access.inputs.values(), access.outputs.values()]) {
    for (const port of ports) if (port.state !== 'disconnected') names.add(port.name || '(unnamed)');
  }
  const seen = names.size > 0 ? `MIDI ports this browser sees: ${[...names].join(', ')}.` : 'This browser sees no MIDI ports at all.';
  return `${NO_OP1_PORT} ${seen} On Windows only one program can use a MIDI port at a time: close other browsers and music software that may hold it, then quit and reopen this browser.`;
}

export async function openOp1Port(signal?: AbortSignal): Promise<Op1Port> {
  if (typeof navigator === 'undefined' || typeof navigator.requestMIDIAccess !== 'function') throw new Error(NO_WEB_MIDI);
  let access: MIDIAccess;
  try {
    access = await untilAborted(navigator.requestMIDIAccess({ sysex: true }), signal);
  } catch (e) {
    if (signal?.aborted) throw e;
    throw new Error(MIDI_REFUSED, { cause: e });
  }
  const input = findOp1(access.inputs.values());
  const output = findOp1(access.outputs.values());
  if (!input || !output) throw new Error(noOp1Port(access));

  // Listen before opening: the input opens asynchronously, and a reply can arrive before open()
  // settles. Messages that come before onSysex has a listener wait in `early`.
  let listener: ((bytes: Uint8Array) => void) | null = null;
  const early: Uint8Array[] = [];
  const receive = (event: MIDIMessageEvent): void => {
    if (!event.data) return;
    if (listener) listener(event.data);
    else early.push(event.data);
  };
  input.onmidimessage = receive;
  const close = (): void => {
    if (input.onmidimessage === receive) input.onmidimessage = null;
    void input.close().catch(() => undefined);
    void output.close().catch(() => undefined);
  };
  try {
    await untilAborted(Promise.all([input.open(), output.open()]), signal);
  } catch (e) {
    close();
    throw e;
  }

  return {
    name: output.name ?? 'OP-1',
    send: (bytes) => output.send(bytes),
    onSysex(cb) {
      listener = cb;
      for (const bytes of early.splice(0)) cb(bytes);
    },
    close,
    watch(cb) {
      const changed = (): void => cb(input.state !== 'disconnected' && output.state !== 'disconnected');
      access.addEventListener('statechange', changed);
      return () => access.removeEventListener('statechange', changed);
    },
  };
}
