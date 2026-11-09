import { describe, expect, it } from 'vitest';
import { FakeCoop } from '../../../test/fake-coop';
import {
  CoopRefusedError,
  coopDeletePlugin,
  coopHello,
  coopPutPlugin,
  coopSetPlugin,
  parseCoopHello,
  parseCoopInventory,
  readCoopInventory,
} from './coop';
import { NoAnswerError, createSession } from './session';

const bytes = (n: number, seed = 7): Uint8Array => Uint8Array.from({ length: n }, (_, i) => (i * seed + 3) & 0xff);

const twoPlugins = (): FakeCoop =>
  new FakeCoop({
    files: { 'saw.coop': bytes(2996), 'stereo_delay.coop': bytes(984) },
    boot: {
      'saw.coop': { kind: 'synth', name: 'saw', state: 'loaded', version: '0.1', author: 'coldsoldr' },
      'stereo_delay.coop': { kind: 'fx', name: 'stereo delay', state: 'failed', reason: 'plugin memory full' },
    },
  });

describe('coop HELLO', () => {
  it('reads the protocol, the capabilities and the build tag', async () => {
    const hello = await coopHello(createSession(new FakeCoop({ build: 'e3' })));
    expect(hello).toEqual({ protocol: 1, caps: 0x3f, build: 'e3' });
  });

  it('is null for a payload that is not coop', () => {
    expect(parseCoopHello(Uint8Array.from([0x6b, 0x6e, 0x6c, 0x21, 1, 0, 0]))).toBeNull();
  });

  it('gives up on a device that does not answer', async () => {
    const session = createSession(new FakeCoop({ silent: true }), { timeoutMs: 5, tries: 2 });
    await expect(coopHello(session)).rejects.toBeInstanceOf(NoAnswerError);
  });
});

describe('coop inventory', () => {
  it('parses every column into an inventory record', () => {
    const inv = parseCoopInventory(
      'coop-inventory 1\tbuild e3\tprotocol 1\n' +
        'saw.coop\tsynth\tsaw\tloaded\toff\t0.1\tcoldsoldr\t2996\t\n' +
        'new_one.coop\t?\t\tnew\ton\t\t\t0\t\n',
    );
    expect(inv.build).toBe('e3');
    expect(inv.records[0]).toMatchObject({
      kind: 'synth',
      name: 'saw',
      source: 'coop',
      path: 'plugins/saw.coop',
      state: 'loaded',
      enabled: false,
      size: 2996,
      version: '0.1',
      author: 'coldsoldr',
    });
    expect(inv.records[1]).toMatchObject({ kind: null, name: 'new_one', state: 'pending', enabled: true });
  });

  it('refuses text that is not a coop inventory', () => {
    expect(() => parseCoopInventory('kennel-inventory 1\n')).toThrow(/not a coop inventory/);
  });

  it('reads an inventory longer than one page', async () => {
    const inv = await readCoopInventory(createSession(twoPlugins()));
    expect(inv.records.map((r) => [r.name, r.state, r.enabled])).toEqual([
      ['saw', 'loaded', true],
      ['stereo_delay', 'failed', true],
    ]);
    expect(inv.records[1]?.reason).toBe('plugin memory full');
  });
});

describe('coop plugin files', () => {
  it('writes a file in chunks and the OP-1 checks size and CRC', async () => {
    const device = new FakeCoop();
    const data = bytes(1000);
    const progress: number[] = [];
    await coopPutPlugin(createSession(device), 'saw.coop', data, { onProgress: (n) => progress.push(n) });
    expect(device.files.get('saw.coop')).toEqual(data);
    expect(progress.at(-1)).toBe(1000);
    expect(device.requests.filter((r) => r.cmd === 0x23)).toHaveLength(Math.ceil(1000 / 64));
  });

  it('counts a resent chunk the OP-1 already wrote as written', async () => {
    const device = new FakeCoop();
    device.loseReplyOf = { cmd: 0x23, nth: 3 };
    const data = bytes(400);
    await coopPutPlugin(createSession(device, { timeoutMs: 5 }), 'saw.coop', data);
    expect(device.files.get('saw.coop')).toEqual(data);
  });

  it('refuses a name that is not <id>.coop before sending anything', async () => {
    const device = new FakeCoop();
    await expect(coopPutPlugin(createSession(device), '../x.coop', bytes(4))).rejects.toThrow(/not a coop plugin file name/);
    expect(device.requests).toHaveLength(0);
  });

  it('deletes a file, and says so when it was not there', async () => {
    const device = twoPlugins();
    const session = createSession(device);
    await coopDeletePlugin(session, 'saw.coop');
    expect(device.files.has('saw.coop')).toBe(false);
    await expect(coopDeletePlugin(session, 'saw.coop')).rejects.toBeInstanceOf(CoopRefusedError);
  });

  it('switches a plugin off and on for the next boot', async () => {
    const device = twoPlugins();
    const session = createSession(device);
    await coopSetPlugin(session, 'saw.coop', false);
    expect((await readCoopInventory(session)).records[0]?.enabled).toBe(false);
    await coopSetPlugin(session, 'saw.coop', true);
    expect(device.disabled.has('saw.coop')).toBe(false);
  });

  it('explains a refusal while the settings menu is open', async () => {
    const device = twoPlugins();
    device.menuOpen = true;
    await expect(coopSetPlugin(createSession(device), 'saw.coop', false)).rejects.toThrow(/settings menu is open/);
  });
});
