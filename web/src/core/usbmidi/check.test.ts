import { describe, it, expect } from 'vitest';
import { checkDevice } from './check';
import { NoAnswerError } from './session';
import { FakeOp1 } from '../../../test/fake-op1';

const fast = { timeoutMs: 20 };

const INVENTORY = [
  ['kennel-inventory 1', 'core 01e00000 0003a000 1a2b3c4d', 'presets 00042 cap 00100'].join('\t'),
  ['synth', 'rings', 'knl', 'plugins/rings.knl', '1.0', 'coldsoldr', '111768', '0badf00d', 'loaded', '', ''].join('\t'),
  ['fx', 'tape_echo', 'knl', 'plugins/tape_echo.knl', '1.0', 'coldsoldr', '53844', '12345678', 'off', '', 'switched off'].join(
    '\t',
  ),
].join('\n');

describe('checkDevice', () => {
  it('reads hello, the inventory and both switched-off lists from a kennel OP-1', async () => {
    const device = new FakeOp1({ inventory: INVENTORY, off: 'tape_echo', offAtBoot: 'tape_echo', realtime: true, splitInto: 5 });
    const result = await checkDevice(device, fast);
    if (result.kind !== 'kennel') throw new Error(`expected kennel, got ${result.kind}`);
    expect(result.hello).toEqual({ version: 1, inventory: true, plugins: true });
    expect(result.inventory?.presets).toEqual({ count: 42, cap: 100 });
    expect(result.inventory?.records.map((r) => [r.name, r.state])).toEqual([
      ['rings', 'loaded'],
      ['tape_echo', 'off'],
    ]);
    expect(result.off).toEqual({ stored: ['tape_echo'], boot: ['tape_echo'] });
    expect(device.closed).toBe(false);
  });

  it('reports a kennel build without the inventory', async () => {
    const device = new FakeOp1({ caps: null, version: 1 });
    expect(await checkDevice(device, fast)).toEqual({
      kind: 'kennel',
      hello: { version: 1, inventory: false, plugins: false },
      inventory: null,
      off: null,
    });
    expect(device.requests).toHaveLength(1);
  });

  it('reports a port that never answers as silent', async () => {
    expect(await checkDevice(new FakeOp1({ silent: true }), fast)).toEqual({ kind: 'silent' });
  });

  it('survives lost replies', async () => {
    const result = await checkDevice(new FakeOp1({ inventory: INVENTORY, loseReplies: 3 }), fast);
    expect(result.kind).toBe('kennel');
  });

  it('fails when the OP-1 stops answering after HELLO', async () => {
    const device = new FakeOp1({ inventory: INVENTORY });
    const send = device.send.bind(device);
    device.send = (bytes) => {
      if (device.requests.length === 0) send(bytes);
      else device.requests.push({ cmd: bytes[5] ?? 0, seq: 0, payload: new Uint8Array(0) });
    };
    await expect(checkDevice(device, fast)).rejects.toBeInstanceOf(NoAnswerError);
  });

  it('fails on an inventory it cannot read', async () => {
    await expect(checkDevice(new FakeOp1({ inventory: 'something else' }), fast)).rejects.toThrow(/does not know/);
  });

  it('stops when aborted', async () => {
    const controller = new AbortController();
    controller.abort(new Error('the OP-1 was disconnected'));
    await expect(checkDevice(new FakeOp1(), { ...fast, signal: controller.signal })).rejects.toThrow('disconnected');
  });
});
