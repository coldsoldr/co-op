import { afterEach, describe, expect, it, vi } from 'vitest';
import { MIDI_REFUSED, NO_OP1_PORT, NO_WEB_MIDI, RESTART_TIMEOUT, openOp1Port, waitForOp1Restart } from './webmidi';

/** Just enough of a Web MIDI port for openOp1Port. */
class FakePort {
  state: MIDIPortDeviceState = 'connected';
  onmidimessage: ((event: { data: Uint8Array }) => void) | null = null;
  opened = false;
  closed = false;
  sent: Uint8Array[] = [];
  /** Runs while open() is pending, e.g. to deliver a message before it settles. */
  duringOpen: (() => void) | null = null;
  constructor(readonly name: string) {}
  open(): Promise<this> {
    this.duringOpen?.();
    this.opened = true;
    return Promise.resolve(this);
  }
  close(): Promise<this> {
    this.closed = true;
    return Promise.resolve(this);
  }
  send(bytes: Uint8Array): void {
    this.sent.push(bytes);
  }
}

class FakeAccess extends EventTarget {
  constructor(
    readonly inputs: Map<string, FakePort>,
    readonly outputs: Map<string, FakePort>,
  ) {
    super();
  }
}

const portsOf = (...ports: FakePort[]): Map<string, FakePort> => new Map(ports.map((p, i) => [String(i), p]));

function stubMidi(access: FakeAccess | Error): ReturnType<typeof vi.fn> {
  const request = vi.fn((options: unknown) => {
    void options;
    return access instanceof Error ? Promise.reject(access) : Promise.resolve(access);
  });
  vi.stubGlobal('navigator', { requestMIDIAccess: request });
  return request;
}

afterEach(() => {
  vi.unstubAllGlobals();
});

describe('openOp1Port', () => {
  it('names the browsers when there is no Web MIDI', async () => {
    vi.stubGlobal('navigator', {});
    await expect(openOp1Port()).rejects.toThrow(NO_WEB_MIDI);
    expect(NO_WEB_MIDI).toMatch(/Chrome, Edge or Firefox/);
  });

  it('explains a refused permission', async () => {
    stubMidi(new DOMException('denied', 'NotAllowedError'));
    await expect(openOp1Port()).rejects.toThrow(MIDI_REFUSED);
  });

  it('says to leave disk mode when there is no OP-1 port', async () => {
    const gone = new FakePort('OP-1 Midi Device');
    gone.state = 'disconnected';
    stubMidi(new FakeAccess(portsOf(new FakePort('Keystep'), gone), portsOf(new FakePort('OP-1'))));
    await expect(openOp1Port()).rejects.toThrow(NO_OP1_PORT);
    expect(NO_OP1_PORT).toMatch(/not in disk mode/);
  });

  it('lists the ports it does see, and says when there are none', async () => {
    stubMidi(new FakeAccess(portsOf(new FakePort('Keystep')), portsOf(new FakePort('Keystep'))));
    await expect(openOp1Port()).rejects.toThrow('MIDI ports this browser sees: Keystep.');
    stubMidi(new FakeAccess(portsOf(), portsOf()));
    await expect(openOp1Port()).rejects.toThrow('sees no MIDI ports at all');
  });

  it('never takes an OP-1 Field for an OP-1', async () => {
    stubMidi(new FakeAccess(portsOf(new FakePort('OP-1 Field')), portsOf(new FakePort('OP-1 Field'))));
    await expect(openOp1Port()).rejects.toThrow('MIDI ports this browser sees: OP-1 Field.');
  });

  it('asks for SysEx, opens the connected OP-1 ports and queues early messages', async () => {
    const input = new FakePort('OP-1 Midi Device');
    const output = new FakePort('op1 out');
    input.duringOpen = () => input.onmidimessage?.({ data: Uint8Array.from([0xf0, 1, 0xf7]) });
    const request = stubMidi(new FakeAccess(portsOf(new FakePort('Keystep'), input), portsOf(new FakePort('Keystep'), output)));
    const port = await openOp1Port();
    expect(request).toHaveBeenCalledWith({ sysex: true });
    expect(input.opened && output.opened).toBe(true);
    expect(port.name).toBe('op1 out');

    const received: number[][] = [];
    input.onmidimessage?.({ data: Uint8Array.from([0xf8]) });
    port.onSysex((bytes) => received.push([...bytes]));
    input.onmidimessage?.({ data: Uint8Array.from([0xf0, 2, 0xf7]) });
    expect(received).toEqual([[0xf0, 1, 0xf7], [0xf8], [0xf0, 2, 0xf7]]);

    port.send(Uint8Array.from([1, 2]));
    expect(output.sent).toEqual([Uint8Array.from([1, 2])]);
    port.close();
    expect(input.onmidimessage).toBeNull();
    expect(input.closed && output.closed).toBe(true);
  });

  it('reports port changes until unwatched', async () => {
    const input = new FakePort('OP-1');
    const output = new FakePort('OP-1');
    const access = new FakeAccess(portsOf(input), portsOf(output));
    stubMidi(access);
    const port = await openOp1Port();
    const seen: boolean[] = [];
    const unwatch = port.watch((connected) => seen.push(connected));
    access.dispatchEvent(new Event('statechange'));
    input.state = 'disconnected';
    access.dispatchEvent(new Event('statechange'));
    unwatch();
    access.dispatchEvent(new Event('statechange'));
    expect(seen).toEqual([true, false]);
  });

  it('stops when aborted', async () => {
    stubMidi(new FakeAccess(portsOf(new FakePort('OP-1')), portsOf(new FakePort('OP-1'))));
    const controller = new AbortController();
    controller.abort(new Error('closed'));
    await expect(openOp1Port(controller.signal)).rejects.toThrow('closed');
  });
});

describe('waitForOp1Restart', () => {
  const tick = (ms: number): Promise<void> => new Promise((resolve) => setTimeout(resolve, ms));
  const setup = () => {
    const input = new FakePort('OP-1');
    const output = new FakePort('OP-1');
    const access = new FakeAccess(portsOf(input), portsOf(output));
    stubMidi(access);
    const set = (state: MIDIPortDeviceState): void => {
      input.state = state;
      output.state = state;
      access.dispatchEvent(new Event('statechange'));
    };
    return { set };
  };
  /** Long enough that only the events and the poll can end the wait. */
  const slow = { goneMs: 10_000, timeoutMs: 10_000, pollMs: 10_000 };

  it('resolves once the ports went away and came back', async () => {
    const { set } = setup();
    let done = false;
    const wait = waitForOp1Restart(undefined, slow).then(() => (done = true));
    await tick(5);
    expect(done).toBe(false);
    set('disconnected');
    await tick(5);
    expect(done).toBe(false);
    set('connected');
    await wait;
  });

  it('resolves when the ports were already gone and come back', async () => {
    const { set } = setup();
    set('disconnected');
    const wait = waitForOp1Restart(undefined, slow);
    await tick(5);
    set('connected');
    await wait;
  });

  it('notices the change by polling when no statechange comes', async () => {
    const input = new FakePort('OP-1');
    const output = new FakePort('OP-1');
    stubMidi(new FakeAccess(portsOf(input), portsOf(output)));
    const wait = waitForOp1Restart(undefined, { ...slow, pollMs: 5 });
    input.state = 'disconnected';
    await tick(20);
    input.state = 'connected';
    await wait;
  });

  it('counts the OP-1 as back when its ports never seemed to go away', async () => {
    setup();
    await waitForOp1Restart(undefined, { ...slow, goneMs: 10 });
  });

  it('gives up after the timeout', async () => {
    const { set } = setup();
    const wait = waitForOp1Restart(undefined, { ...slow, timeoutMs: 20 });
    set('disconnected');
    await expect(wait).rejects.toThrow(RESTART_TIMEOUT);
    expect(RESTART_TIMEOUT).toMatch(/switch it off and on: it then starts without plugin files/);
  });

  it('stops when aborted', async () => {
    setup();
    const controller = new AbortController();
    const wait = waitForOp1Restart(controller.signal, slow);
    await tick(1);
    controller.abort(new Error('closed'));
    await expect(wait).rejects.toThrow('closed');
  });
});
