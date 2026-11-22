import { describe, expect, it } from 'vitest';
import { FakeCoop } from '../../../test/fake-coop';
import {
  COOP_CHUNK,
  COOP_WINDOW,
  CoopRefusedError,
  coopDeletePlugin,
  coopHello,
  coopPutPlugin,
  coopSetPlugin,
  parseCoopHello,
  parseCoopInventory,
  readCoopInventory,
  readCoopMemoryReport,
} from './coop';
import { readU32le } from './framing';
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
    // Before e12 the inventory has no memory figures: none are made up.
    expect(inv.memory).toBeUndefined();
    expect(inv.records[0]).not.toHaveProperty('need');
    expect(inv.records[0]).not.toHaveProperty('scratch');
  });

  it('reads the budget, the bytes in use and each file’s need and buffer from an e12 inventory', () => {
    // firmware/docs/sysex.md "INVENTORY on e12": the header's five fields, and columns 10 and 11 on every row.
    const inv = parseCoopInventory(
      'coop-inventory 1\tbuild e12\tprotocol 1\tbudget 5898240\tused 980992\tfxbuf 134784\tfxbufs 4\tclip 4194304\n' +
        'rings.coop\tsynth\trings\tloaded\ton\t0.5\tcoldsoldr\t113048\t\t667712\t0\n' +
        'tape_echo.coop\tfx\ttape echo\tloaded\ton\t0.3\tcoldsoldr\t23892\tno memory for this position\t14592\t134784\n' +
        'zzover.coop\t?\t\tnew\ton\t\t\t428\t\t0\t0\n',
    );
    expect(inv.build).toBe('e12');
    expect(inv.memory).toEqual({ budget: 5898240, used: 980992, fxbuf: 134784, fxbufs: 4, clip: 4194304 });
    expect(inv.records.map((r) => [r.name, r.state, r.size, r.need, r.scratch, r.reason])).toEqual([
      ['rings', 'loaded', 113048, 667712, 0, ''],
      ['tape_echo', 'loaded', 23892, 14592, 134784, 'no memory for this position'],
      ['zzover', 'pending', 428, 0, 0, ''],
    ]);
  });

  it('takes header fields by name, in any order, and ignores ones it does not know', () => {
    const inv = parseCoopInventory('coop-inventory 1\tbuild e13\tused 64\tlater 9\tprotocol 1\tbudget 6000000\n');
    expect(inv.build).toBe('e13');
    expect(inv.memory).toEqual({ budget: 6000000, used: 64, fxbuf: 0, fxbufs: 0, clip: null });
    // A budget without what is in use is no account of the memory.
    expect(parseCoopInventory('coop-inventory 1\tbuild e12\tprotocol 1\tbudget 5898240\n').memory).toBeUndefined();
  });

  it('serves the e12 fields page by page from the fake device', async () => {
    const device = new FakeCoop({
      build: 'e12',
      files: { 'saw.coop': bytes(2996), 'late.coop': bytes(428) },
      boot: { 'saw.coop': { kind: 'synth', name: 'saw', state: 'loaded', need: 3968 } },
      memory: { budget: 5898240, used: 3968 },
    });
    const inv = await readCoopInventory(createSession(device));
    expect(inv.memory).toEqual({ budget: 5898240, used: 3968, fxbuf: 0, fxbufs: 0, clip: 4194304 });
    expect(inv.records.map((r) => [r.name, r.size, r.need, r.scratch])).toEqual([
      ['late', 428, 0, 0],
      ['saw', 2996, 3968, 0],
    ]);
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

describe('coop file writes with chunks in flight', () => {
  const DATA = 0x23;
  const data = bytes(64 * 20 + 17);
  const chunks = Math.ceil(data.length / COOP_CHUNK);
  /** The PUT_DATA messages in the order sent: the offset, and the queue's depth as each arrived. */
  const sent = (device: FakeCoop): { offset: number; queued: number | undefined }[] =>
    device.requests.filter((r) => r.cmd === DATA).map((r) => ({ offset: readU32le(r.payload), queued: r.queued }));
  const deepest = (device: FakeCoop): number => Math.max(...sent(device).map((s) => s.queued ?? 0));
  /** Where the first chunk sent a second time sits in `sent`. */
  const firstResend = (device: FakeCoop): number => {
    const offsets = sent(device).map((s) => s.offset);
    return offsets.findIndex((o, i) => offsets.indexOf(o) < i);
  };
  /** Collects the progress and checks that it only ever reports what the OP-1 holds, never what is on its way. */
  const watch = (device: FakeCoop, progress: number[]) => ({
    onProgress: (n: number, total: number): void => {
      expect(n).toBeLessThanOrEqual(device.written);
      expect(n).toBeGreaterThan(progress.at(-1) ?? 0);
      expect(total).toBe(data.length);
      progress.push(n);
    },
  });

  it('keeps three chunks in flight, and the queue of four never drops one', async () => {
    const device = new FakeCoop({ queue: 4 });
    const progress: number[] = [];
    await coopPutPlugin(createSession(device), 'saw.coop', data, watch(device, progress));
    expect(device.files.get('saw.coop')).toEqual(data);
    expect(COOP_WINDOW).toBe(3);
    expect(deepest(device)).toBe(3);
    expect(device.drops).toBe(0);
    expect(sent(device).map((s) => s.offset)).toEqual(Array.from({ length: chunks }, (_, i) => i * COOP_CHUNK));
    expect(progress).toEqual(Array.from({ length: chunks }, (_, i) => Math.min((i + 1) * COOP_CHUNK, data.length)));
  });

  it('never keeps more in flight than the queue holds less one', async () => {
    const device = new FakeCoop({ queue: 4 });
    await coopPutPlugin(createSession(device), 'saw.coop', data, { window: 9 });
    expect(device.files.get('saw.coop')).toEqual(data);
    expect(deepest(device)).toBe(3);
    expect(device.drops).toBe(0);
  });

  it('sends one at a time when told to', async () => {
    const device = new FakeCoop({ queue: 4 });
    await coopPutPlugin(createSession(device), 'saw.coop', data, { window: 1 });
    expect(device.files.get('saw.coop')).toEqual(data);
    expect(deepest(device)).toBe(1);
    expect(sent(device)).toHaveLength(chunks);
  });

  it('takes a later ok for a chunk whose own reply was lost', async () => {
    const device = new FakeCoop({ queue: 4 });
    device.loseReplyOf = { cmd: DATA, nth: 5 };
    const progress: number[] = [];
    await coopPutPlugin(createSession(device), 'saw.coop', data, watch(device, progress));
    expect(device.files.get('saw.coop')).toEqual(data);
    expect(sent(device)).toHaveLength(chunks);
    expect(progress).not.toContain(5 * COOP_CHUNK);
    expect(progress.at(-1)).toBe(data.length);
  });

  it('falls back to one in flight when no reply comes in time', async () => {
    const device = new FakeCoop({ queue: 4 });
    // Chunks 5, 6 and 7 are written and their replies lost: nothing in flight confirms chunk 5.
    device.loseReplyOf = { cmd: DATA, nth: 5, count: 3 };
    const progress: number[] = [];
    await coopPutPlugin(createSession(device, { timeoutMs: 200 }), 'saw.coop', data, watch(device, progress));
    expect(device.files.get('saw.coop')).toEqual(data);
    // Chunk 5 goes again, alone; the OP-1 names the offset it had reached, and the rest follows one at a time.
    const again = firstResend(device);
    expect(again).toBe(7);
    expect(sent(device)[again]?.offset).toBe(4 * COOP_CHUNK);
    expect(sent(device)[again + 1]?.offset).toBe(7 * COOP_CHUNK);
    expect(sent(device).slice(again).every((s) => s.queued === 1)).toBe(true);
    expect(sent(device)).toHaveLength(chunks + 1);
    expect(progress.at(-1)).toBe(data.length);
  });

  it('falls back at once when the OP-1 answers a chunk with an error', async () => {
    const device = new FakeCoop({ queue: 4 });
    // Chunk 6 never arrives, so chunk 7 is out of order: status 5, long before the three seconds of a timeout.
    device.loseRequestOf = { cmd: DATA, nth: 6 };
    const progress: number[] = [];
    const started = Date.now();
    await coopPutPlugin(createSession(device), 'saw.coop', data, watch(device, progress));
    expect(Date.now() - started).toBeLessThan(2000);
    expect(device.files.get('saw.coop')).toEqual(data);
    const again = firstResend(device);
    expect(sent(device)[again]?.offset).toBe(5 * COOP_CHUNK);
    expect(sent(device).slice(again + 1).every((s) => s.queued === 1)).toBe(true);
    expect(progress.at(-1)).toBe(data.length);
  });

  it('writes the same file to a device that takes one message at a time', async () => {
    const device = new FakeCoop({ queue: 1 });
    const progress: number[] = [];
    await coopPutPlugin(createSession(device), 'saw.coop', data, watch(device, progress));
    expect(device.files.get('saw.coop')).toEqual(data);
    expect(device.drops).toBe(2);
    expect(progress.at(-1)).toBe(data.length);
    // The next file starts with the window again, and arrives whole too.
    await coopPutPlugin(createSession(device), 'drive.coop', data);
    expect(device.files.get('drive.coop')).toEqual(data);
    expect(device.drops).toBe(4);
  });

  it('writes the same file to a device that stays silent about what it could not take', async () => {
    // One at a time, and the reply to the chunk it did take is lost as well: only the timeout tells.
    const device = new FakeCoop({ queue: 1 });
    device.loseReplyOf = { cmd: DATA, nth: 1 };
    await coopPutPlugin(createSession(device, { timeoutMs: 100 }), 'saw.coop', data);
    expect(device.files.get('saw.coop')).toEqual(data);
  });

  it('reports a write error on the OP-1 as one', async () => {
    const device = new FakeCoop({ queue: 4 });
    device.failWriteOf = 4;
    const error = await coopPutPlugin(createSession(device), 'saw.coop', data).catch((e: unknown) => e);
    expect(error).toBeInstanceOf(CoopRefusedError);
    expect((error as CoopRefusedError).status).toBe(7);
    expect((error as Error).message).toMatch(/a write error on the OP-1/);
    expect(device.files.has('saw.coop')).toBe(false);
  });

  it('still fails a file the OP-1 finds damaged at the end', async () => {
    const device = new FakeCoop({ queue: 4 });
    const send = device.send.bind(device);
    let damaged = false;
    device.send = (message) => {
      // One byte of the first chunk flips on the way.
      if (message[5] !== DATA || damaged) return send(message);
      damaged = true;
      const bad = Uint8Array.from(message);
      bad[20] = (bad[20] as number) ^ 1;
      send(bad);
    };
    await expect(coopPutPlugin(createSession(device), 'saw.coop', data)).rejects.toThrow(/finishing saw\.coop: a checksum mismatch/);
    expect(device.files.has('saw.coop')).toBe(false);
  });
});

describe('the memory figure of the OP-1 itself', () => {
  const counters = (n: number): string => `coop-stats 3 build e10\n${'cb_max 12345 cb_sum 678901 blocks 4321\n'.repeat(n)}plugins seen 3 loaded 2 failed 1\n`;
  const plugmem =
    'plugmem budget 1572864 used 683152 over_budget 1 heap_short 0 skipped 0 badname 0 tape_node 524288 heap_free_boot 15058684 scratch_files 1 fxbuf 134784 fxbuf_mem 673960 fxbuf_short 0\n' +
    'plugfile cs_hall.coop failed needs 514880: over the plugin memory budget\n';
  const windows = 'top seq 4 blocks 100\nwin seq 5 blocks 90\n';
  const expected = { budget: 1572864, used: 683152, exact: true, needs: { 'cs_hall.coop': 514880 } };

  it('reads the plugmem line of the statistics file, twice', async () => {
    const device = new FakeCoop({ stats: counters(20) + plugmem + windows });
    expect(await readCoopMemoryReport(createSession(device))).toEqual(expected);
    const first = device.requests.length;
    // The next look starts where the line was found.
    expect(await readCoopMemoryReport(createSession(device))).toEqual(expected);
    expect(device.requests.length - first).toBeLessThan(first);
  });

  it('finds the line again after the file has grown or shrunk', async () => {
    let n = 30;
    const device = new FakeCoop({ stats: () => counters(n) + plugmem + windows });
    expect(await readCoopMemoryReport(createSession(device))).toEqual(expected);
    n = 2;
    expect(await readCoopMemoryReport(createSession(device))).toEqual(expected);
  });

  it('reads a block that ends the file', async () => {
    expect(await readCoopMemoryReport(createSession(new FakeCoop({ stats: counters(1) + plugmem })))).toEqual(expected);
  });

  it('does not trust a figure the second pass does not repeat', async () => {
    // The figure differs on every pass, as if the passes kept mixing two versions of the file.
    const device = new FakeCoop({ stats: (i) => counters(1) + plugmem.replace('used 683152', `used ${683152 + (i >> 2)}`) + windows });
    expect(await readCoopMemoryReport(createSession(device))).toBeNull();
  });

  it('is null on a core without the file or without the line', async () => {
    expect(await readCoopMemoryReport(createSession(new FakeCoop()))).toBeNull();
    expect(await readCoopMemoryReport(createSession(new FakeCoop({ stats: counters(3) + windows })))).toBeNull();
  });
});
