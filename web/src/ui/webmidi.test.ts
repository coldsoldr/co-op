import { afterEach, describe, expect, it, vi } from 'vitest';
import { MIDI_REFUSED, NO_OP1_PORT, NO_WEB_MIDI, PORT_BUSY, RESTART_TIMEOUT, openMidiLeases, openOp1Port, waitForOp1Restart } from './webmidi';

/** Just enough of a Web MIDI port for openOp1Port. */
class FakePort {
  state: MIDIPortDeviceState = 'connected';
  onmidimessage: ((event: { data: Uint8Array }) => void) | null = null;
  opened = false;
  closed = false;
  /** Open now: opened and not closed since. */
  isOpen = false;
  /** Makes open() reject, as when another program holds the port. */
  refuseOpen = false;
  sent: Uint8Array[] = [];
  /** Runs while open() is pending, e.g. to deliver a message before it settles. */
  duringOpen: (() => void) | null = null;
  constructor(public name: string) {}
  open(): Promise<this> {
    this.duringOpen?.();
    if (this.refuseOpen) return Promise.reject(new DOMException('in use', 'InvalidAccessError'));
    this.opened = true;
    this.isOpen = true;
    return Promise.resolve(this);
  }
  close(): Promise<this> {
    this.closed = true;
    this.isOpen = false;
    return Promise.resolve(this);
  }
  send(bytes: Uint8Array): void {
    this.sent.push(bytes);
  }
}

class FakeAccess extends EventTarget {
  /** statechange listeners added and not removed since. */
  listeners = 0;
  constructor(
    readonly inputs: Map<string, FakePort>,
    readonly outputs: Map<string, FakePort>,
  ) {
    super();
  }
  override addEventListener(...args: Parameters<EventTarget['addEventListener']>): void {
    this.listeners++;
    super.addEventListener(...args);
  }
  override removeEventListener(...args: Parameters<EventTarget['removeEventListener']>): void {
    this.listeners--;
    super.removeEventListener(...args);
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
  // Every operation, however it ended, gave its MIDI session back.
  expect(openMidiLeases()).toBe(0);
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
    stubMidi(new FakeAccess(portsOf(new FakePort('Keystep')), portsOf(new FakePort('OP-1'))));
    await expect(openOp1Port()).rejects.toThrow(NO_OP1_PORT);
    expect(NO_OP1_PORT).toMatch(/not in disk mode/);
  });

  it('says another program holds the port when the OP-1 is listed but could not be opened', async () => {
    // Chromium lists a device it failed to open as disconnected.
    const held = new FakePort('OP-1 Midi Device');
    held.state = 'disconnected';
    stubMidi(new FakeAccess(portsOf(new FakePort('Keystep'), held), portsOf(new FakePort('OP-1'))));
    await expect(openOp1Port()).rejects.toThrow(PORT_BUSY);
    expect(PORT_BUSY).toBe('Another program is using the OP-1’s MIDI port. Close it and try again.');
  });

  it('says another program holds the port when opening it fails, and closes the other one', async () => {
    const input = new FakePort('OP-1');
    const output = new FakePort('OP-1');
    output.refuseOpen = true;
    stubMidi(new FakeAccess(portsOf(input), portsOf(output)));
    await expect(openOp1Port()).rejects.toThrow(PORT_BUSY);
    expect(input.isOpen || output.isOpen).toBe(false);
    expect(input.onmidimessage).toBeNull();
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
    expect(openMidiLeases()).toBe(1);
    port.close();
    expect(input.onmidimessage).toBeNull();
    expect(input.closed && output.closed).toBe(true);
    expect(input.isOpen || output.isOpen).toBe(false);
    expect(openMidiLeases()).toBe(0);
    // A second close does nothing more.
    port.close();
    expect(openMidiLeases()).toBe(0);
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
    port.close();
  });

  it('drops its watchers on close, so the access keeps nothing of the operation', async () => {
    const access = new FakeAccess(portsOf(new FakePort('OP-1')), portsOf(new FakePort('OP-1')));
    stubMidi(access);
    const port = await openOp1Port();
    const seen: boolean[] = [];
    port.watch((connected) => seen.push(connected));
    expect(access.listeners).toBe(1);
    port.close();
    expect(access.listeners).toBe(0);
    access.dispatchEvent(new Event('statechange'));
    expect(seen).toEqual([]);
  });

  it('opens the ports again for the next operation', async () => {
    const input = new FakePort('OP-1');
    const output = new FakePort('OP-1');
    const request = stubMidi(new FakeAccess(portsOf(input), portsOf(output)));
    for (let i = 0; i < 3; i++) {
      const port = await openOp1Port();
      expect(input.isOpen && output.isOpen).toBe(true);
      port.close();
      expect(input.isOpen || output.isOpen).toBe(false);
    }
    expect(request).toHaveBeenCalledTimes(3);
  });

  it('stops when aborted', async () => {
    stubMidi(new FakeAccess(portsOf(new FakePort('OP-1')), portsOf(new FakePort('OP-1'))));
    const controller = new AbortController();
    controller.abort(new Error('closed'));
    await expect(openOp1Port(controller.signal)).rejects.toThrow('closed');
  });

  it('closes the ports when aborted while they open', async () => {
    const input = new FakePort('OP-1');
    const output = new FakePort('OP-1');
    const controller = new AbortController();
    input.duringOpen = () => controller.abort(new Error('closed'));
    stubMidi(new FakeAccess(portsOf(input), portsOf(output)));
    await expect(openOp1Port(controller.signal)).rejects.toThrow('closed');
    expect(input.isOpen || output.isOpen).toBe(false);
    expect(input.onmidimessage).toBeNull();
  });
});

describe('the MIDI session in a frame', () => {
  interface Frame {
    removed: boolean;
    attributes: Record<string, string>;
  }
  /** A document whose frames have their own navigator, with requestMIDIAccess on the prototype as in a browser. */
  const setup = (frameMidi: 'works' | 'refuses' | 'none') => {
    const input = new FakePort('OP-1');
    const output = new FakePort('OP-1');
    const access = new FakeAccess(portsOf(input), portsOf(output));
    const framed = vi.fn((options: unknown) => {
      void options;
      return frameMidi === 'refuses' ? Promise.reject(new Error('no MIDI in frames')) : Promise.resolve(access);
    });
    const page = vi.fn(() => Promise.resolve(access));
    const frames: Frame[] = [];
    const document = {
      body: { append: vi.fn() },
      createElement: vi.fn(() => {
        const frame = {
          hidden: false,
          tabIndex: 0,
          title: '',
          attributes: {} as Record<string, string>,
          setAttribute(name: string, value: string) {
            this.attributes[name] = value;
          },
          removed: false,
          remove() {
            this.removed = true;
          },
          contentWindow: { navigator: frameMidi === 'none' ? {} : { requestMIDIAccess: framed } },
        };
        frames.push(frame);
        return frame;
      }),
    };
    class PageNavigator {}
    Object.defineProperty(PageNavigator.prototype, 'requestMIDIAccess', { value: page });
    vi.stubGlobal('navigator', new PageNavigator());
    vi.stubGlobal('document', document);
    return { input, output, framed, page, frames, document };
  };

  it('asks a hidden frame for MIDI and removes the frame when the port closes', async () => {
    const { input, output, framed, page, frames, document } = setup('works');
    const port = await openOp1Port();
    expect(framed).toHaveBeenCalledWith({ sysex: true });
    expect(page).not.toHaveBeenCalled();
    expect(document.body.append).toHaveBeenCalledTimes(1);
    expect(frames).toHaveLength(1);
    expect(frames[0]?.attributes).toMatchObject({ allow: 'midi', 'aria-hidden': 'true' });
    expect(frames[0]?.removed).toBe(false);
    port.close();
    expect(frames[0]?.removed).toBe(true);
    expect(input.isOpen || output.isOpen).toBe(false);
  });

  it('removes the frame when there is no OP-1, and after a restart wait', async () => {
    const { input, frames } = setup('works');
    input.name = 'Keystep';
    await expect(openOp1Port()).rejects.toThrow(NO_OP1_PORT);
    input.name = 'OP-1';
    await waitForOp1Restart(undefined, { goneMs: 1, timeoutMs: 1000, pollMs: 1000 });
    expect(frames.map((f) => f.removed)).toEqual([true, true]);
  });

  it("falls back to the page's navigator when the frame gets no MIDI", async () => {
    for (const how of ['refuses', 'none'] as const) {
      const { page, frames } = setup(how);
      const port = await openOp1Port();
      expect(page).toHaveBeenCalledTimes(1);
      expect(frames[0]?.removed).toBe(true);
      port.close();
    }
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
    return { set, input, output, access };
  };
  /** Long enough that only the events and the poll can end the wait. */
  const slow = { goneMs: 10_000, timeoutMs: 10_000, pollMs: 10_000 };

  it('resolves once the ports went away and came back, opening none and leaving no listener', async () => {
    const { set, input, output, access } = setup();
    let done = false;
    const wait = waitForOp1Restart(undefined, slow).then(() => (done = true));
    await tick(5);
    expect(done).toBe(false);
    expect(openMidiLeases()).toBe(1);
    set('disconnected');
    await tick(5);
    expect(done).toBe(false);
    set('connected');
    await wait;
    expect(input.opened || output.opened).toBe(false);
    expect(access.listeners).toBe(0);
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
