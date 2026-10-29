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

async function midiAccess(signal: AbortSignal | undefined): Promise<MIDIAccess> {
  if (typeof navigator === 'undefined' || typeof navigator.requestMIDIAccess !== 'function') throw new Error(NO_WEB_MIDI);
  try {
    return await untilAborted(navigator.requestMIDIAccess({ sysex: true }), signal);
  } catch (e) {
    if (signal?.aborted) throw e;
    throw new Error(MIDI_REFUSED, { cause: e });
  }
}

export const RESTART_TIMEOUT =
  'The OP-1 did not come back on USB. If it shows the tape screen, unplug the cable and plug it in again, then check it over USB. If it does not start, switch it off and on: it then starts without plugin files.';

export interface RestartWait {
  /** How long the whole restart may take. */
  timeoutMs?: number;
  /**
   * How long to wait for the ports to go away. Some systems may not report it; after this the OP-1
   * counts as back while its ports are there.
   */
  goneMs?: number;
  /** How often to look at the ports besides on statechange, which a system may not send. */
  pollMs?: number;
}

/**
 * Waits for the OP-1 to restart after an install: its MIDI ports go away, then come back. Resolves
 * once both are back, rejects with RESTART_TIMEOUT after `timeoutMs`.
 */
export async function waitForOp1Restart(signal?: AbortSignal, wait: RestartWait = {}): Promise<void> {
  const { timeoutMs = 90_000, goneMs = 15_000, pollMs = 1_000 } = wait;
  const access = await midiAccess(signal);
  const present = (): boolean => findOp1(access.inputs.values()) !== undefined && findOp1(access.outputs.values()) !== undefined;
  signal?.throwIfAborted();
  return new Promise((resolve, reject) => {
    let gone = !present();
    const check = (): void => {
      if (!present()) gone = true;
      else if (gone) finish();
    };
    const finish = (error?: unknown): void => {
      clearTimeout(goneTimer);
      clearTimeout(timer);
      clearInterval(poll);
      access.removeEventListener('statechange', check);
      signal?.removeEventListener('abort', onAbort);
      if (error === undefined) resolve();
      else reject(error);
    };
    const onAbort = (): void => finish(abortReason(signal as AbortSignal));
    const goneTimer = setTimeout(() => {
      if (!gone && present()) finish();
    }, goneMs);
    const timer = setTimeout(() => finish(new Error(RESTART_TIMEOUT)), timeoutMs);
    const poll = setInterval(check, pollMs);
    access.addEventListener('statechange', check);
    signal?.addEventListener('abort', onAbort, { once: true });
  });
}

export async function openOp1Port(signal?: AbortSignal): Promise<Op1Port> {
  const access = await midiAccess(signal);
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
