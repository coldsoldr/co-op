import { existsSync, readdirSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';
import {
  blk,
  budgetOfCore,
  coopMemoryOf,
  CoopFileError,
  estimateFromSize,
  formatKb,
  largestRows,
  levelOf,
  ownBytes,
  planMemory,
  positionBuffers,
  reportFromInventory,
  reportFromStats,
  setBytes,
  type CoopMemory,
  type DeviceCoopFile,
  type KnownCoopFile,
  type MemoryPlan,
} from './memory';

const PLUGINS = fileURLToPath(new URL('../../../../plugins', import.meta.url));
const coopFile = (id: string): string => `${PLUGINS}/${id}/${id}.coop`;
const read = (id: string): Uint8Array => new Uint8Array(readFileSync(coopFile(id)));
const BUDGET = 1536 * 1024;

/** A .coop file made for a test: an effect or an engine with the header fields the sum reads. */
function makeCoop(o: {
  kind: 1 | 2;
  load: number;
  imports?: number;
  scratch?: number;
  svg?: boolean;
  /** An effect: COOP_FX_F_SCREEN in its flags. An engine: its screen pointer is relocated. */
  screen?: boolean;
  voiceBytes?: number;
}): Uint8Array {
  const svg = o.svg ? new TextEncoder().encode('<svg/>\0') : new Uint8Array(0);
  const h = (svg.length + 3) & ~3;
  const imageAt = h + 144;
  const imageSize = 64;
  const entry = 16;
  const relocAt = imageAt + imageSize;
  const extAt = relocAt + 12;
  const bytes = new Uint8Array(extAt + 12);
  const view = new DataView(bytes.buffer);
  bytes.set(svg, 0);
  bytes.set(new TextEncoder().encode('COOP'), h);
  view.setUint16(h + 4, 1, true);
  view.setUint16(h + 6, 144, true);
  bytes[h + 18] = o.kind;
  view.setUint32(h + 36, o.scratch ? 0x20 : 0, true);
  view.setUint32(h + 76, imageSize, true); // bss_offset
  view.setUint32(h + 80, o.load - imageSize, true); // bss_size
  view.setUint32(h + 84, imageSize, true);
  view.setUint32(h + 92, entry, true);
  view.setUint32(h + 100, imageAt, true);
  view.setUint32(h + 104, relocAt, true);
  view.setUint32(h + 108, 1, true);
  view.setUint32(h + 116, o.imports ?? 0, true);
  view.setUint32(h + 132, svg.length, true);
  view.setUint32(h + 140, extAt, true);
  if (o.kind === 2) view.setUint32(imageAt + entry + 8, o.screen ? 1 : 0, true);
  else view.setUint32(imageAt + entry + 12, o.voiceBytes ?? 0, true);
  // One relocation: the engine's screen pointer, or an unrelated word.
  view.setUint32(relocAt, o.kind === 1 && o.screen ? entry + 32 : 0, true);
  bytes[relocAt + 4] = 1;
  if (o.scratch) {
    view.setUint16(extAt, 7, true);
    view.setUint16(extAt + 2, 4, true);
    view.setUint32(extAt + 4, o.scratch, true);
  }
  return bytes;
}

describe('the heap sums of the core', () => {
  it('rounds a block up to 8 bytes with its 8-byte header', () => {
    expect([1, 8, 9, 240].map(blk)).toEqual([16, 16, 24, 248]);
  });

  it('takes one block for the five position buffers', () => {
    expect(positionBuffers(0)).toBe(0);
    expect(positionBuffers(134784)).toBe(673960); // the core's fxbuf_mem for tape_echo's buffer (device-tests.md e9)
  });
});

describe('coopMemoryOf', () => {
  it('charges an old-model effect five relocations', () => {
    // tape_echo 0.1 (firmware/plugins/tape_echo/README.md): load 143,652 bytes; the e7 gate's "needs 718480".
    const m = coopMemoryOf(makeCoop({ kind: 2, load: 143652 }));
    expect(m).toEqual({ kind: 'fx', image: 5 * blk(143652 + 3 + 32), scratch: 0, voices: 0, screen: 0 });
    expect(m.image).toBe(718480);
  });

  it('charges a scratch effect one relocation and rounds its buffer to 32 bytes', () => {
    const m = coopMemoryOf(makeCoop({ kind: 2, load: 1000, imports: 2, scratch: 1001, svg: true, screen: true }));
    expect(m).toEqual({ kind: 'fx', image: blk(1000 + 3 + 24 + 32), scratch: 1024, voices: 0, screen: 248 });
    expect(setBytes([m])).toBe(m.image + 248 + blk(5 * 1024 + 32));
  });

  it('counts a screen object only for a file with both an SVG and a screen', () => {
    expect(coopMemoryOf(makeCoop({ kind: 2, load: 1000, svg: true, screen: false })).screen).toBe(0);
    expect(coopMemoryOf(makeCoop({ kind: 2, load: 1000, svg: false, screen: true })).screen).toBe(0);
    expect(coopMemoryOf(makeCoop({ kind: 1, load: 1000, svg: true, screen: false })).screen).toBe(0);
    expect(coopMemoryOf(makeCoop({ kind: 1, load: 1000, svg: true, screen: true })).screen).toBe(248);
  });

  it('adds an engine its six voices, each rounded up to 32 bytes', () => {
    expect(coopMemoryOf(makeCoop({ kind: 1, load: 2000, voiceBytes: 0 })).voices).toBe(0);
    expect(coopMemoryOf(makeCoop({ kind: 1, load: 2000, voiceBytes: 100 })).voices).toBe(blk(128 * 6 + 32));
    expect(coopMemoryOf(makeCoop({ kind: 1, load: 2000, voiceBytes: 8192 })).voices).toBe(blk(8192 * 6 + 32));
  });

  it('refuses bytes that are not a coop file', () => {
    expect(() => coopMemoryOf(new Uint8Array(200))).toThrow(CoopFileError);
    expect(() => coopMemoryOf(new TextEncoder().encode('<svg/>'))).toThrow(CoopFileError);
    const feature = makeCoop({ kind: 2, load: 100 });
    feature[18] = 3;
    expect(() => coopMemoryOf(feature)).toThrow(/kind 3/);
  });
});

describe('the catalogue files', () => {
  const ids = readdirSync(PLUGINS).filter((id) => existsSync(coopFile(id)));

  it('reads the memory of every .coop file in plugins/', () => {
    expect(ids.length).toBeGreaterThan(0);
    for (const id of ids) {
      const m = coopMemoryOf(read(id));
      expect(m.image, id).toBeGreaterThan(0);
      expect(m.image % 8, id).toBe(0);
      expect(m.scratch % 32, id).toBe(0);
      expect(m.kind === 'synth' ? m.scratch : m.voices, id).toBe(0);
      // No file alone is over the budget, or it could never load.
      expect(setBytes([m]), id).toBeLessThanOrEqual(BUDGET);
    }
  });

  /**
   * The core's own figure for this set in the emulator: `plugmem budget 1572864 used 1450224` (firmware/docs/
   * device-tests.md, the e9 gate of 2026-11-11). That run had tape_echo 0.2 (load 12,604 bytes, 9 imports:
   * firmware/plugins/tape_echo/README.md); the catalogue holds 0.3, whose tempo sync added 200 bytes and one import,
   * 208 bytes of heap. With 0.2's image in tape_echo's place the sum is the core's to the byte.
   *
   * The sizes pin the builds the figure was checked with. When one of these files is rebuilt this test steps aside
   * (the test above still reads the new file) until the sizes and the totals here are brought up to date.
   */
  const GATE: Record<string, number> = {
    bitcrusher: 12292,
    cs_hall: 23184,
    cs_plate: 19332,
    drive: 19516,
    ensemble: 19928,
    freq_shift: 23396,
    rings: 113048,
    saw: 6520,
    tape_drive: 22384,
    tape_echo: 23892,
    wavefolder: 14304,
  };
  const unchanged = Object.entries(GATE).every(([id, size]) => existsSync(coopFile(id)) && read(id).length === size);

  it.skipIf(!unchanged)('adds the 11-file set up to what the core reported', () => {
    const set = Object.keys(GATE).map((id) => [id, coopMemoryOf(read(id))] as const);
    expect(setBytes(set.map(([, m]) => m))).toBe(1_450_432);
    const echo02 = blk(12604 + 3 + 12 * 9 + 32);
    const withEcho02 = set.map(([id, m]): CoopMemory => (id === 'tape_echo' ? { ...m, image: echo02 } : m));
    expect(setBytes(withEcho02)).toBe(1_450_224);
    // The buffers are sized by tape_echo, the largest of the nine effects: the core's `fxbuf 134784 fxbuf_mem 673960`.
    expect(Math.max(...set.map(([, m]) => m.scratch))).toBe(134784);
    // saw alone: the e7 gate's `plugmem ... used 2320`, before the core counted screen objects.
    expect(coopMemoryOf(read('saw')).image).toBe(2320);
  });
});

const fx = (image: number, scratch = 0): CoopMemory => ({ kind: 'fx', image, scratch, voices: 0, screen: 0 });
const onDevice = (file: string, size: number, more: Partial<DeviceCoopFile> = {}): DeviceCoopFile => ({
  file,
  size,
  kind: 'fx',
  loaded: true,
  enabled: true,
  reason: '',
  ...more,
});
const KNOWN: KnownCoopFile[] = [
  { file: 'a.coop', sizeBytes: 1000, memory: fx(100_000, 32_000) },
  { file: 'b.coop', sizeBytes: 2000, memory: fx(200_000, 64_000) },
  { file: 'c.coop', sizeBytes: 3000, memory: { kind: 'synth', image: 600_000, scratch: 0, voices: 1_000, screen: 248 } },
];

describe('budgetOfCore', () => {
  it('knows the cores with a budget and has none before e7', () => {
    expect(budgetOfCore('e10')).toEqual({ bytes: BUDGET, known: true });
    expect(budgetOfCore('e7')).toEqual({ bytes: BUDGET, known: true });
    expect(budgetOfCore('e6')).toBeNull();
    expect(budgetOfCore('e3')).toBeNull();
  });

  it('guesses for a core it does not know', () => {
    expect(budgetOfCore('e99')).toEqual({ bytes: BUDGET, known: false });
    expect(budgetOfCore(null)).toEqual({ bytes: BUDGET, known: false });
  });
});

describe('levelOf', () => {
  it('warns above 92 % and is over above the budget', () => {
    expect(levelOf(920, 1000)).toBe('fits');
    expect(levelOf(921, 1000)).toBe('nearly');
    expect(levelOf(1000, 1000)).toBe('nearly');
    expect(levelOf(1001, 1000)).toBe('over');
  });
});

describe('planMemory', () => {
  it('adds up the files it knows, with the position buffers once', () => {
    const files = [onDevice('a.coop', 1000), onDevice('b.coop', 2000), onDevice('c.coop', 3000, { kind: 'synth' })];
    const plan = planMemory(files, KNOWN, { build: 'e10' }) as MemoryPlan;
    const buffers = positionBuffers(64_000);
    expect(plan).toMatchObject({ budget: BUDGET, budgetKnown: true, approximate: false, shared: buffers, level: 'fits' });
    expect(plan.used).toBe(100_000 + 200_000 + 601_248 + buffers);
    expect(plan.rows.map((r) => [r.file, r.bytes, r.exact])).toEqual([
      ['a.coop', 100_000, true],
      ['b.coop', 200_000, true],
      ['c.coop', 601_248, true],
    ]);
    expect(plan.loadedNow).toEqual({ bytes: plan.used, fromDevice: false, exact: true });
  });

  it('does not depend on the order of the files', () => {
    const files = [onDevice('a.coop', 1000), onDevice('b.coop', 2000), onDevice('c.coop', 3000)];
    expect(planMemory([...files].reverse(), KNOWN, { build: 'e10' })?.used).toBe(planMemory(files, KNOWN, { build: 'e10' })?.used);
  });

  it('says what removing a file gives back: the buffers shrink only without the largest', () => {
    const plan = planMemory([onDevice('a.coop', 1000), onDevice('b.coop', 2000)], KNOWN, { build: 'e10' }) as MemoryPlan;
    const frees = Object.fromEntries(plan.rows.map((r) => [r.file, r.frees]));
    expect(frees['a.coop']).toBe(100_000);
    expect(frees['b.coop']).toBe(200_000 + positionBuffers(64_000) - positionBuffers(32_000));
    expect(largestRows(plan).map((r) => r.file)).toEqual(['b.coop', 'a.coop']);
  });

  it('leaves a switched-off file out of the next boot and counts one that failed or is new', () => {
    const plan = planMemory(
      [
        onDevice('a.coop', 1000, { enabled: false }),
        onDevice('b.coop', 2000, { loaded: false, reason: 'plugin memory full' }),
        onDevice('c.coop', 0, { loaded: false, kind: null }),
      ],
      KNOWN,
      { build: 'e10' },
    ) as MemoryPlan;
    expect(plan.used).toBe(200_000 + 601_248 + positionBuffers(64_000));
    expect(plan.approximate).toBe(false);
    expect(plan.rows[0]).toMatchObject({ counted: false, frees: 0, bytes: 100_000 });
    // Loaded now: a.coop alone, still switched on at the last boot.
    expect(plan.loadedNow.bytes).toBe(100_000 + positionBuffers(32_000));
  });

  it('estimates a file it does not have from its size, and says the total is approximate', () => {
    const files = [onDevice('a.coop', 1000), onDevice('mine.coop', 5000), onDevice('a.coop', 1001)];
    const plan = planMemory(files, KNOWN.slice(0, 1), { build: 'e10' }) as MemoryPlan;
    expect(plan.approximate).toBe(true);
    expect(plan.rows.map((r) => r.exact)).toEqual([true, false, false]);
    expect(plan.rows[1]?.bytes).toBe(ownBytes(estimateFromSize(5000, 'fx')));
    expect(estimateFromSize(5000, 'fx').image).toBe(5 * blk(5032));
    expect(estimateFromSize(5000, 'synth').image).toBe(blk(5032));
    expect(estimateFromSize(0, null).image).toBe(0);
  });

  it('takes the budget, the bytes in use and what a refused file needs from the inventory text', () => {
    const files = [
      onDevice('a.coop', 1000),
      onDevice('big.coop', 9000, { loaded: false, reason: 'over the plugin memory budget (needs 702 KB, 118 KB of 1536 KB left)' }),
    ];
    expect(reportFromInventory(files)).toEqual({ budget: BUDGET, used: BUDGET - 118 * 1024, exact: false, needs: { 'big.coop': 702 * 1024 } });
    const plan = planMemory(files, KNOWN, { build: 'e99' }) as MemoryPlan;
    expect(plan).toMatchObject({ budget: BUDGET, budgetKnown: true, level: 'fits', approximate: true });
    expect(plan.rows[1]).toMatchObject({ bytes: 702 * 1024, exact: false });
    expect(plan.loadedNow).toEqual({ bytes: BUDGET - 118 * 1024, fromDevice: true, exact: false });
    expect(reportFromInventory([onDevice('a.coop', 1000, { reason: 'plugin memory full' })])).toBeNull();
  });

  it('gives the one loaded file it does not know what the OP-1 says is in use beyond the others', () => {
    const report = { budget: 2_000_000, used: 100_000 + positionBuffers(32_000) + 55_000, exact: true, needs: {} };
    const plan = planMemory([onDevice('a.coop', 1000), onDevice('mine.coop', 5000)], KNOWN, { build: 'e10', report }) as MemoryPlan;
    expect(plan.budget).toBe(2_000_000);
    expect(plan.rows[1]).toMatchObject({ bytes: 55_000, exact: true });
    expect(plan.approximate).toBe(false);
    expect(plan.used).toBe(report.used);
  });

  it('adds the file about to be sent, in place of one of the same name', () => {
    const files = [onDevice('a.coop', 1000), onDevice('b.coop', 1999)];
    const add: KnownCoopFile = { file: 'b.coop', sizeBytes: 2000, memory: fx(1_400_000) };
    const plan = planMemory(files, KNOWN, { build: 'e10', add }) as MemoryPlan;
    expect(plan.rows.map((r) => r.file)).toEqual(['a.coop', 'b.coop']);
    expect(plan.used).toBe(100_000 + positionBuffers(32_000) + 1_400_000);
    expect(plan).toMatchObject({ level: 'over', approximate: false });
    expect(planMemory(files, KNOWN, { build: 'e10', add: { ...add, file: 'new.coop', memory: fx(8) } })?.rows).toHaveLength(3);
  });

  it('has nothing to say for a core without a budget', () => {
    expect(planMemory([onDevice('a.coop', 1000)], KNOWN, { build: 'e6' })).toBeNull();
  });
});

describe('reportFromStats', () => {
  const stats =
    'plugins seen 12 loaded 10 failed 2\n' +
    'plugmem budget 1572864 used 1103984 over_budget 2 heap_short 0 skipped 0 badname 0 tape_node 524288 heap_free_boot 14636460\n' +
    'plugfile tape_echo.coop failed needs 718480: over the plugin memory budget\n' +
    'plugfile off.coop disabled needs 0\n' +
    'top seq 1\n';

  it('reads the plugmem line and what each refused file needs', () => {
    expect(reportFromStats(stats)).toEqual({ budget: 1572864, used: 1103984, exact: true, needs: { 'tape_echo.coop': 718480 } });
  });

  it('is null without a whole plugmem line', () => {
    expect(reportFromStats('plugins seen 1\nplugmem budget 1572864 used 11')).toBeNull();
  });
});

describe('formatKb', () => {
  it('writes whole KB with thousands separators', () => {
    expect(formatKb(1_450_224)).toBe('1,416 KB');
    expect(formatKb(BUDGET)).toBe('1,536 KB');
  });
});
