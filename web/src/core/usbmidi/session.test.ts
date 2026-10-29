import { describe, it, expect } from 'vitest';
import { Command, HEADER, pack7, u32le } from './framing';
import { NoAnswerError, RefusedError, createSession } from './session';
import { FakeOp1 } from '../../../test/fake-op1';

/** Short waits keep the timeout and retry tests fast. */
const fast = { timeoutMs: 20 };

const tick = (ms = 0): Promise<void> => new Promise((resolve) => setTimeout(resolve, ms));

/** Inventory text of a given length, so paging boundaries are easy to see. */
const textOf = (length: number): string => Array.from({ length }, (_, i) => String.fromCharCode(97 + (i % 26))).join('');

describe('hello', () => {
  it('reads the version and the capabilities byte', async () => {
    const session = createSession(new FakeOp1({ version: 2, caps: 3 }), fast);
    expect(await session.hello()).toEqual({ version: 2, inventory: true, plugins: true });
  });

  it('reads both capabilities as absent when the build sends only the version', async () => {
    const session = createSession(new FakeOp1({ version: 1, caps: null }), fast);
    expect(await session.hello()).toEqual({ version: 1, inventory: false, plugins: false });
  });

  it('reads single capability bits', async () => {
    expect(await createSession(new FakeOp1({ caps: 1 }), fast).hello()).toMatchObject({ inventory: true, plugins: false });
    expect(await createSession(new FakeOp1({ caps: 2 }), fast).hello()).toMatchObject({ inventory: false, plugins: true });
  });

  it('asks only once per session', async () => {
    const device = new FakeOp1();
    const session = createSession(device, fast);
    await Promise.all([session.hello(), session.hello()]);
    await session.hello();
    expect(device.requests).toHaveLength(1);
  });
});

describe('ask', () => {
  it('gives up after every try on a silent port, each try with a new seq', async () => {
    const device = new FakeOp1({ silent: true });
    const session = createSession(device, fast);
    const error = await session.hello().catch((e: unknown) => e);
    expect(error).toBeInstanceOf(NoAnswerError);
    expect((error as Error).message).toBe('the OP-1 did not answer HELLO (5 tries)');
    expect(device.requests.map((r) => r.seq)).toEqual([1, 2, 3, 4, 5]);
  });

  it('honours the number of tries', async () => {
    const device = new FakeOp1({ silent: true });
    await expect(createSession(device, { timeoutMs: 5, tries: 2 }).ask(Command.HELLO)).rejects.toThrow(/2 tries/);
    expect(device.requests).toHaveLength(2);
  });

  it('resends after lost replies', async () => {
    const device = new FakeOp1({ loseReplies: 2, version: 4 });
    expect((await createSession(device, fast).hello()).version).toBe(4);
    expect(device.requests.map((r) => r.seq)).toEqual([1, 2, 3]);
  });

  it('ignores stale replies and replies to another command', async () => {
    const device = new FakeOp1({ silent: true });
    const session = createSession(device, { timeoutMs: 1000 });
    const reply = session.ask(Command.HELLO);
    await tick();
    const [request] = device.requests;
    if (!request) throw new Error('nothing was sent');
    const send = (cmd: number, seq: number, body: number[]): void =>
      device.inject([...HEADER, cmd | 0x40, seq & 0x7f, seq >> 7, ...pack7(Uint8Array.from(body)), 0xf7]);
    send(Command.HELLO, request.seq + 1, [0, 9, 0, 0, 0]);
    send(Command.LIST, request.seq, [0, 8, 0, 0, 0]);
    send(Command.HELLO, request.seq, [0, 7, 0, 0, 0]);
    expect((await reply).payload[0]).toBe(7);
  });

  it('takes a late reply to an earlier try as stale', async () => {
    // Every reply arrives after the timeout, so each try's reply lands during the next try.
    const device = new FakeOp1({ replyDelayMs: 50 });
    const error = await createSession(device, { timeoutMs: 20, tries: 3 })
      .hello()
      .catch((e: unknown) => e);
    expect(error).toBeInstanceOf(NoAnswerError);
  });

  it('keeps one message in flight', async () => {
    const device = new FakeOp1({ replyDelayMs: 5 });
    const session = createSession(device, fast);
    const both = Promise.all([session.ask(Command.HELLO), session.ask(Command.LIST, Uint8Array.from(u32le(0)))]);
    await tick(1);
    expect(device.requests).toHaveLength(1);
    const [hello, list] = await both;
    expect(hello.cmd).toBe(Command.HELLO);
    expect(list.cmd).toBe(Command.LIST);
    expect(device.requests).toHaveLength(2);
  });

  it('returns refusals with their status', async () => {
    const device = new FakeOp1({ inventory: 'kennel-inventory 1' });
    const reply = await createSession(device, fast).ask(Command.LIST, Uint8Array.from(u32le(500)));
    expect(reply.status).toBe(5);
  });

  it('reassembles replies split into runs with realtime bytes inside', async () => {
    const device = new FakeOp1({ realtime: true, splitInto: 3, version: 3 });
    expect((await createSession(device, fast).hello()).version).toBe(3);
  });

  it('stops at once when the signal aborts', async () => {
    const device = new FakeOp1({ silent: true });
    const controller = new AbortController();
    const session = createSession(device, { timeoutMs: 1000, signal: controller.signal });
    const asked = session.ask(Command.HELLO);
    await tick();
    controller.abort(new Error('closed'));
    await expect(asked).rejects.toThrow('closed');
    await expect(session.ask(Command.HELLO)).rejects.toThrow('closed');
    expect(device.requests).toHaveLength(1);
  });

  it('rejects when sending fails', async () => {
    const port = {
      send() {
        throw new Error('the port is disconnected');
      },
      onSysex() {},
      close() {},
    };
    await expect(createSession(port, fast).ask(Command.HELLO)).rejects.toThrow('the port is disconnected');
  });
});

describe('readInventory', () => {
  it('pages through LIST in 64-byte steps', async () => {
    const text = textOf(150);
    const device = new FakeOp1({ inventory: text });
    expect(await createSession(device, fast).readInventory()).toBe(text);
    expect(device.requests.map((r) => [...r.payload])).toEqual([
      [0, 0, 0, 0],
      [64, 0, 0, 0],
      [128, 0, 0, 0],
    ]);
  });

  it('reads an exact multiple of a page without asking past the end', async () => {
    const device = new FakeOp1({ inventory: textOf(128) });
    expect(await createSession(device, fast).readInventory()).toHaveLength(128);
    expect(device.requests).toHaveLength(2);
  });

  it('reads an empty text in one ask', async () => {
    const device = new FakeOp1({ inventory: '' });
    expect(await createSession(device, fast).readInventory()).toBe('');
    expect(device.requests).toHaveLength(1);
  });

  it('decodes UTF-8 split across pages', async () => {
    const text = `${textOf(63)}é${textOf(10)}`;
    expect(await createSession(new FakeOp1({ inventory: text }), fast).readInventory()).toBe(text);
  });

  it('survives lost replies part way through', async () => {
    const text = textOf(100);
    const device = new FakeOp1({ inventory: text, loseReplies: 1, splitInto: 7, realtime: true });
    expect(await createSession(device, fast).readInventory()).toBe(text);
  });

  it('throws on a refusal', async () => {
    const device = new FakeOp1({ caps: null });
    await expect(createSession(device, fast).readInventory()).rejects.toThrow(
      new RefusedError(Command.LIST, 1, 'the plugin inventory'),
    );
  });

  it('throws when a reply carries no text before the total', async () => {
    const device = new FakeOp1({ silent: true });
    const session = createSession(device, { timeoutMs: 1000 });
    const read = session.readInventory();
    await tick();
    device.reply(Command.LIST, device.requests[0]?.seq ?? 0, [0, ...u32le(10)]);
    await expect(read).rejects.toThrow('the OP-1 sent an empty piece of the plugin inventory');
  });
});

describe('readOff', () => {
  it('reads the stored and the boot lists as ids', async () => {
    const device = new FakeOp1({ off: 'drive,tape_echo', offAtBoot: 'drive' });
    const session = createSession(device, fast);
    expect(await session.readOff('stored')).toEqual(['drive', 'tape_echo']);
    expect(await session.readOff('boot')).toEqual(['drive']);
    expect(device.requests.map((r) => [...r.payload])).toEqual([
      [0, 0, 0],
      [1, 0, 0],
    ]);
  });

  it('pages a long list and reads an empty one', async () => {
    const ids = Array.from({ length: 20 }, (_, i) => `plugin_${i}`);
    const device = new FakeOp1({ off: ids.join(','), offAtBoot: '' });
    const session = createSession(device, fast);
    expect(await session.readOff('stored')).toEqual(ids);
    expect(device.requests.map((r) => r.payload[1])).toEqual([0, 64, 128]);
    expect(await session.readOff('boot')).toEqual([]);
  });

  it('drops spaces and a trailing line end around ids', async () => {
    const session = createSession(new FakeOp1({ off: 'drive, tape_echo,\n' }), fast);
    expect(await session.readOff('stored')).toEqual(['drive', 'tape_echo']);
  });
});
