import { existsSync, readdirSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';
import {
  blk,
  budgetOfCore,
  clipSeconds,
  coopMemoryOf,
  CoopFileError,
  estimateFromSize,
  formatKb,
  largestRows,
  levelOf,
  ownBytes,
  planMemory,
  positionBuffers,
  r32,
  reportFromInventory,
  reportFromStats,
  ruleOfCore,
  setBytes,
  type CoopMemory,
  type DeviceCoopFile,
  type DeviceMemoryReport,
  type KnownCoopFile,
  type MemoryPlan,
} from './memory';

const PLUGINS = fileURLToPath(new URL('../../../../plugins', import.meta.url));
const coopFile = (id: string): string => `${PLUGINS}/${id}/${id}.coop`;
const read = (id: string): Uint8Array => new Uint8Array(readFileSync(coopFile(id)));
const BUDGET = 1536 * 1024;
const E12 = 5760 * 1024;

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
  /** The control declaration (extension 6), a text; '' is an entry that declares nothing. */
  controls?: string;
}): Uint8Array {
  const svg = o.svg ? new TextEncoder().encode('<svg/>\0') : new Uint8Array(0);
  const h = (svg.length + 3) & ~3;
  const imageAt = h + 144;
  const imageSize = 64;
  const entry = 16;
  const relocAt = imageAt + imageSize;
  const extAt = relocAt + 12;
  const text = new TextEncoder().encode(`${o.controls ?? ''}\0`);
  const bytes = new Uint8Array(extAt + 8 + 4 + ((text.length + 3) & ~3) + 4);
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
  // The extension table: the scratch size (or an author, tag 2, in its place), then the controls, then the end.
  view.setUint16(extAt, o.scratch ? 7 : 2, true);
  view.setUint16(extAt + 2, 4, true);
  view.setUint32(extAt + 4, o.scratch ?? 0x00414141, true);
  if (o.controls !== undefined) {
    view.setUint16(extAt + 8, 6, true);
    view.setUint16(extAt + 10, text.length, true);
    bytes.set(text, extAt + 12);
  }
  return bytes;
}

describe('the sums of the cores', () => {
  it('rounds a heap block up to 8 bytes with its 8-byte header, an arena block up to 32', () => {
    expect([1, 8, 9, 240].map(blk)).toEqual([16, 16, 24, 248]);
    expect([1, 32, 33, 240, 1384].map(r32)).toEqual([32, 32, 64, 256, 1408]);
  });

  it('counts one block for the five position buffers before e12, one buffer from e12', () => {
    expect(positionBuffers(0)).toBe(0);
    expect(positionBuffers(0, 'arena')).toBe(0);
    expect(positionBuffers(134784)).toBe(673960); // the core's fxbuf_mem for tape_echo's buffer (device-tests.md e9)
    expect(positionBuffers(134784, 'arena')).toBe(134784);
  });

  it('picks the rule by the core', () => {
    expect(['e7', 'e10', 'e11'].map(ruleOfCore)).toEqual(['heap', 'heap', 'heap']);
    expect(['e12', 'e12-test', 'e13', 'e99'].map(ruleOfCore)).toEqual(['arena', 'arena', 'arena', 'arena']);
    // A core the site cannot name counts the newest way.
    expect(ruleOfCore(null)).toBe('arena');
  });
});

describe('coopMemoryOf', () => {
  it('charges an old-model effect five relocations', () => {
    // tape_echo 0.1 (firmware/plugins/tape_echo/README.md): load 143,652 bytes; the e7 gate's "needs 718480".
    const m = coopMemoryOf(makeCoop({ kind: 2, load: 143652 }));
    expect(m).toEqual({ kind: 'fx', cap: 143655, scratch: 0, voice: 0, screen: false, controls: false });
    expect(ownBytes(m, 'heap')).toBe(718480);
    expect(ownBytes(m, 'arena')).toBe(5 * r32(143655));
  });

  it('charges a scratch effect one relocation and rounds its buffer to 32 bytes', () => {
    const m = coopMemoryOf(makeCoop({ kind: 2, load: 1000, imports: 2, scratch: 1001, svg: true, screen: true }));
    expect(m).toEqual({ kind: 'fx', cap: 1000 + 3 + 24, scratch: 1024, voice: 0, screen: true, controls: false });
    expect(ownBytes(m, 'heap')).toBe(blk(1027 + 32) + 248);
    expect(setBytes([m], 'heap')).toBe(blk(1027 + 32) + 248 + blk(5 * 1024 + 32));
    expect(ownBytes(m, 'arena')).toBe(1056 + 256);
    expect(setBytes([m], 'arena')).toBe(1056 + 256 + 1024);
  });

  it('counts a screen object only for a file with both an SVG and a screen', () => {
    expect(coopMemoryOf(makeCoop({ kind: 2, load: 1000, svg: true, screen: false })).screen).toBe(false);
    expect(coopMemoryOf(makeCoop({ kind: 2, load: 1000, svg: false, screen: true })).screen).toBe(false);
    expect(coopMemoryOf(makeCoop({ kind: 1, load: 1000, svg: true, screen: false })).screen).toBe(false);
    expect(coopMemoryOf(makeCoop({ kind: 1, load: 1000, svg: true, screen: true })).screen).toBe(true);
  });

  it('adds an engine its six voices, each rounded up to 32 bytes', () => {
    expect(coopMemoryOf(makeCoop({ kind: 1, load: 2000, voiceBytes: 0 })).voice).toBe(0);
    const m = coopMemoryOf(makeCoop({ kind: 1, load: 2000, voiceBytes: 100 }));
    expect(m.voice).toBe(128);
    expect(ownBytes(m, 'heap')).toBe(blk(2003 + 32) + blk(128 * 6 + 32));
    expect(ownBytes(m, 'arena')).toBe(r32(2003) + 6 * 128);
    expect(coopMemoryOf(makeCoop({ kind: 1, load: 2000, voiceBytes: 8192 })).voice).toBe(8192);
    // Over the limit the core refuses the engine.
    expect(coopMemoryOf(makeCoop({ kind: 1, load: 2000, voiceBytes: 8193 })).voice).toBe(0);
  });

  it('sees the controls a file declares, which e12 counts a table for and the cores before do not', () => {
    const plain = coopMemoryOf(makeCoop({ kind: 1, load: 2000 }));
    const empty = coopMemoryOf(makeCoop({ kind: 1, load: 2000, controls: '' }));
    const engine = coopMemoryOf(makeCoop({ kind: 1, load: 2000, controls: 'click steps 2' }));
    const effect = coopMemoryOf(makeCoop({ kind: 2, load: 2000, scratch: 64, controls: 'fn_turn word 5' }));
    expect([plain.controls, empty.controls, engine.controls, effect.controls]).toEqual([false, false, true, true]);
    expect(ownBytes(engine, 'arena') - ownBytes(plain, 'arena')).toBe(1408);
    expect(ownBytes(engine, 'heap')).toBe(ownBytes(plain, 'heap'));
    expect(effect.scratch).toBe(64);
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
      expect(m.cap, id).toBeGreaterThan(0);
      expect(m.scratch % 32, id).toBe(0);
      expect(m.voice % 32, id).toBe(0);
      expect(m.kind === 'synth' ? m.scratch : m.voice, id).toBe(0);
      // No file alone is over the e12 budget, or it could never load.
      expect(setBytes([m], 'arena'), id).toBeLessThanOrEqual(E12);
    }
  });

  /**
   * The cores' own figures in the emulator. The sizes pin the builds they were checked with: when one of these files
   * is rebuilt its test steps aside (the test above still reads the new file) until the sizes and the totals here are
   * brought up to date.
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
  /**
   * The 13 files of the e11 and e12 gates of 2026-11-21 (firmware/docs/device-tests.md, the idle entry): the eight
   * rebuilt on 2026-11-20 (tape_echo 0.4, cs_hall 0.3, cs_plate 0.3, fickle 0.3, petri 0.3, tape_drive 0.6,
   * bitcrusher 0.3, rings 0.10) and the five that were not. GATE above keeps the e9 gate's older builds, so its
   * test steps aside for good now.
   */
  const ALL: Record<string, number> = {
    bitcrusher: 13076,
    cs_hall: 23936,
    cs_plate: 19956,
    drive: 19516,
    ensemble: 19928,
    fickle: 38164,
    freq_shift: 23396,
    petri: 37792,
    rings: 113816,
    saw: 6520,
    tape_drive: 22660,
    tape_echo: 25188,
    wavefolder: 14304,
  };
  const unchanged = (set: Record<string, number>): boolean =>
    Object.entries(set).every(([id, size]) => existsSync(coopFile(id)) && read(id).length === size);
  const memories = (set: Record<string, number>): (readonly [string, CoopMemory])[] =>
    Object.keys(set).map((id) => [id, coopMemoryOf(read(id))] as const);

  /**
   * `plugmem budget 1572864 used 1450224` (firmware/docs/device-tests.md, the e9 gate of 2026-11-11). That run had
   * tape_echo 0.2 (load 12,604 bytes, 9 imports: firmware/plugins/tape_echo/README.md); the catalogue holds 0.3,
   * whose tempo sync added 200 bytes and one import, 208 bytes of heap. With 0.2's image in tape_echo's place the
   * sum is the core's to the byte.
   */
  it.skipIf(!unchanged(GATE))('adds the 11-file set up to what the e9 core reported', () => {
    const set = memories(GATE);
    expect(setBytes(set.map(([, m]) => m), 'heap')).toBe(1_450_432);
    const withEcho02 = set.map(([id, m]): CoopMemory => (id === 'tape_echo' ? { ...m, cap: 12604 + 3 + 12 * 9 } : m));
    expect(setBytes(withEcho02, 'heap')).toBe(1_450_224);
    // The buffers are sized by tape_echo, the largest of the nine effects: the core's `fxbuf 134784 fxbuf_mem 673960`.
    expect(Math.max(...set.map(([, m]) => m.scratch))).toBe(134784);
    // saw alone: the e7 gate's `plugmem ... used 2320`, before the core counted screen objects.
    expect(blk(coopMemoryOf(read('saw')).cap + 32)).toBe(2320);
  });

  /**
   * e11: `plugmem budget 1769472 used 1510352` with these 13 files, first and second boot (the builds before them:
   * 1,502,128). e10 charges the same and allows 1,572,864.
   */
  it.skipIf(!unchanged(ALL))('adds the 13 files up to what the e11 core reported', () => {
    expect(setBytes(memories(ALL).map(([, m]) => m), 'heap')).toBe(1_510_352);
  });

  /**
   * e12: `plugmem budget 5898240 used 989248`: need 854,464 and one buffer of 134,784 (the builds before: 980,992).
   * Alone on the NAND the core reported rings `used 668512` and tape_echo `used 150528` (15,744 and its buffer).
   */
  it.skipIf(!unchanged(ALL))('adds the 13 files up to what the e12 core reported', () => {
    const set = memories(ALL);
    const need = Object.fromEntries(set.map(([id, m]) => [id, ownBytes(m, 'arena')]));
    expect(Object.values(need).reduce((a, b) => a + b, 0)).toBe(854_464);
    expect(setBytes(set.map(([, m]) => m), 'arena')).toBe(989_248);
    expect(need.rings).toBe(668_512);
    expect(need.tape_echo).toBe(15_744);
    expect(set.every(([, m]) => m.controls && m.screen)).toBe(true);
  });
});

const fx = (cap: number, scratch: number): CoopMemory => ({ kind: 'fx', cap, scratch, voice: 0, screen: false, controls: false });
const onDevice = (file: string, size: number, more: Partial<DeviceCoopFile> = {}): DeviceCoopFile => ({
  file,
  size,
  kind: 'fx',
  loaded: true,
  enabled: true,
  reason: '',
  ...more,
});
const A = fx(100_000, 32_000);
const B = fx(200_000, 64_000);
const C: CoopMemory = { kind: 'synth', cap: 600_000, scratch: 0, voice: 160, screen: true, controls: true };
const KNOWN: KnownCoopFile[] = [
  { file: 'a.coop', sizeBytes: 1000, memory: A },
  { file: 'b.coop', sizeBytes: 2000, memory: B },
  { file: 'c.coop', sizeBytes: 3000, memory: C },
];
const heap = (m: CoopMemory): number => ownBytes(m, 'heap');
const arena = (m: CoopMemory): number => ownBytes(m, 'arena');

describe('budgetOfCore', () => {
  it('knows the cores with a budget and has none before e7', () => {
    expect(budgetOfCore('e10')).toEqual({ bytes: BUDGET, known: true });
    expect(budgetOfCore('e7')).toEqual({ bytes: BUDGET, known: true });
    expect(budgetOfCore('e11')).toEqual({ bytes: 1728 * 1024, known: true });
    expect(budgetOfCore('e12')).toEqual({ bytes: 5_898_240, known: true });
    expect(budgetOfCore('e6')).toBeNull();
    expect(budgetOfCore('e3')).toBeNull();
  });

  it('guesses for a core it does not know', () => {
    expect(budgetOfCore('e99')).toEqual({ bytes: E12, known: false });
    expect(budgetOfCore(null)).toEqual({ bytes: E12, known: false });
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

describe('planMemory before e12', () => {
  it('adds up the files it knows, with the position buffers once', () => {
    const files = [onDevice('a.coop', 1000), onDevice('b.coop', 2000), onDevice('c.coop', 3000, { kind: 'synth' })];
    const plan = planMemory(files, KNOWN, { build: 'e10' }) as MemoryPlan;
    const buffers = positionBuffers(64_000);
    expect(plan).toMatchObject({ rule: 'heap', budget: BUDGET, budgetKnown: true, approximate: false, shared: buffers, sharedBy: 'b.coop', level: 'fits' });
    expect(plan.used).toBe(heap(A) + heap(B) + heap(C) + buffers);
    expect(plan.rows.map((r) => [r.file, r.bytes, r.exact])).toEqual([
      ['a.coop', heap(A), true],
      ['b.coop', heap(B), true],
      ['c.coop', heap(C), true],
    ]);
    expect(plan.loadedNow).toEqual({ bytes: plan.used, fromDevice: false, exact: true });
    // Only e12 can say which file is left out and how many positions get a buffer.
    expect(plan.positions).toBeNull();
    expect(plan.refused).toEqual([]);
    expect(planMemory(files, KNOWN, { build: 'e11' })).toMatchObject({ rule: 'heap', budget: 1728 * 1024, used: plan.used });
  });

  it('does not depend on the order of the files', () => {
    const files = [onDevice('a.coop', 1000), onDevice('b.coop', 2000), onDevice('c.coop', 3000)];
    expect(planMemory([...files].reverse(), KNOWN, { build: 'e10' })?.used).toBe(planMemory(files, KNOWN, { build: 'e10' })?.used);
  });

  it('says what removing a file gives back: the buffers shrink only without the largest', () => {
    const plan = planMemory([onDevice('a.coop', 1000), onDevice('b.coop', 2000)], KNOWN, { build: 'e10' }) as MemoryPlan;
    const frees = Object.fromEntries(plan.rows.map((r) => [r.file, r.frees]));
    expect(frees['a.coop']).toBe(heap(A));
    expect(frees['b.coop']).toBe(heap(B) + positionBuffers(64_000) - positionBuffers(32_000));
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
    expect(plan.used).toBe(heap(B) + heap(C) + positionBuffers(64_000));
    expect(plan.approximate).toBe(false);
    expect(plan.rows[0]).toMatchObject({ counted: false, frees: 0, bytes: heap(A) });
    // Loaded now: a.coop alone, still switched on at the last boot.
    expect(plan.loadedNow.bytes).toBe(heap(A) + positionBuffers(32_000));
  });

  it('estimates a file it does not have from its size, and says the total is approximate', () => {
    const files = [onDevice('a.coop', 1000), onDevice('mine.coop', 5000), onDevice('a.coop', 1001)];
    const plan = planMemory(files, KNOWN.slice(0, 1), { build: 'e10' }) as MemoryPlan;
    expect(plan.approximate).toBe(true);
    expect(plan.rows.map((r) => r.exact)).toEqual([true, false, false]);
    expect(plan.rows[1]?.bytes).toBe(5 * blk(5032));
    expect(estimateFromSize(5000, 'fx', 'heap')).toEqual({ own: 5 * blk(5032), scratch: 0 });
    expect(estimateFromSize(5000, 'synth', 'heap').own).toBe(blk(5032));
    expect(estimateFromSize(5000, 'synth', 'arena').own).toBe(5024);
    expect(estimateFromSize(0, null, 'heap').own).toBe(0);
  });

  it('takes the budget, the bytes in use and what a refused file needs from the inventory text', () => {
    const files = [
      onDevice('a.coop', 1000),
      onDevice('big.coop', 9000, { loaded: false, reason: 'over the plugin memory budget (needs 702 KB, 118 KB of 1536 KB left)' }),
    ];
    expect(reportFromInventory(files)).toEqual({ budget: BUDGET, used: BUDGET - 118 * 1024, exact: false, needs: { 'big.coop': 702 * 1024 } });
    const plan = planMemory(files, KNOWN, { build: 'e10' }) as MemoryPlan;
    expect(plan).toMatchObject({ budget: BUDGET, budgetKnown: true, level: 'fits', approximate: true });
    expect(plan.rows[1]).toMatchObject({ bytes: 702 * 1024, exact: false });
    expect(plan.loadedNow).toEqual({ bytes: BUDGET - 118 * 1024, fromDevice: true, exact: false });
    expect(reportFromInventory([onDevice('a.coop', 1000, { reason: 'plugin memory full' })])).toBeNull();
  });

  it('gives the one loaded file it does not know what the OP-1 says is in use beyond the others', () => {
    const report = { budget: 2_000_000, used: heap(A) + positionBuffers(32_000) + 55_000, exact: true, needs: {} };
    const plan = planMemory([onDevice('a.coop', 1000), onDevice('mine.coop', 5000)], KNOWN, { build: 'e10', report }) as MemoryPlan;
    expect(plan.budget).toBe(2_000_000);
    expect(plan.rows[1]).toMatchObject({ bytes: 55_000, exact: true });
    expect(plan.approximate).toBe(false);
    expect(plan.used).toBe(report.used);
  });

  it('adds the file about to be sent, in place of one of the same name', () => {
    const files = [onDevice('a.coop', 1000), onDevice('b.coop', 1999)];
    const add: KnownCoopFile = { file: 'b.coop', sizeBytes: 2000, memory: fx(1_400_000, 64) };
    const plan = planMemory(files, KNOWN, { build: 'e10', add }) as MemoryPlan;
    expect(plan.rows.map((r) => r.file)).toEqual(['a.coop', 'b.coop']);
    expect(plan.used).toBe(heap(A) + positionBuffers(32_000) + heap(add.memory));
    expect(plan).toMatchObject({ level: 'over', approximate: false, refused: [] });
    expect(planMemory(files, KNOWN, { build: 'e10', add: { ...add, file: 'new.coop', memory: fx(8, 32) } })?.rows).toHaveLength(3);
  });

  it('has nothing to say for a core without a budget', () => {
    expect(planMemory([onDevice('a.coop', 1000)], KNOWN, { build: 'e6' })).toBeNull();
  });
});

describe('planMemory on e12', () => {
  const report = (used: number, budget = E12): DeviceMemoryReport => ({ budget, used, exact: true, needs: {}, fxbuf: 64_000, fxbufs: 4, clip: 4_194_304 });

  it('counts arena blocks, the control table and one position buffer', () => {
    const files = [onDevice('a.coop', 1000), onDevice('b.coop', 2000), onDevice('c.coop', 3000, { kind: 'synth' })];
    const plan = planMemory(files, KNOWN, { build: 'e12' }) as MemoryPlan;
    expect(plan).toMatchObject({ rule: 'arena', budget: E12, budgetKnown: true, approximate: false, shared: 64_000, sharedBy: 'b.coop', positions: 4 });
    expect(arena(C)).toBe(r32(600_000) + 6 * 160 + 256 + 1408);
    expect(plan.used).toBe(arena(A) + arena(B) + arena(C) + 64_000);
    // Without the effect with the largest buffer the one buffer shrinks to the next largest.
    expect(plan.rows[1]?.frees).toBe(arena(B) + 64_000 - 32_000);
  });

  it('takes each file as the OP-1 sizes it, also one co-op does not have', () => {
    const files = [
      onDevice('a.coop', 1000, { need: 111_104, scratch: 32_000 }),
      onDevice('mine.coop', 5000, { need: 55_008, scratch: 96_000 }),
      // Not read by the boot: sized from its header, or estimated.
      onDevice('b.coop', 2000, { loaded: false, need: 0, scratch: 0 }),
      onDevice('new.coop', 700, { loaded: false, need: 0, scratch: 0, kind: null }),
    ];
    const plan = planMemory(files, KNOWN, { build: 'e12', report: report(111_104 + 55_008 + 96_000) }) as MemoryPlan;
    expect(plan.rows.map((r) => [r.file, r.bytes, r.exact])).toEqual([
      ['a.coop', 111_104, true],
      ['mine.coop', 55_008, true],
      ['b.coop', arena(B), true],
      ['new.coop', 5 * r32(700), false],
    ]);
    expect(plan.used).toBe(111_104 + 55_008 + arena(B) + 5 * r32(700) + 96_000);
    expect(plan.sharedBy).toBe('mine.coop');
    expect(plan.approximate).toBe(true);
    expect(plan.loadedNow).toEqual({ bytes: 111_104 + 55_008 + 96_000, fromDevice: true, exact: true });
  });

  it('uses the budget an unknown later core reports, with the newest rule and nothing guessed', () => {
    const files = [onDevice('a.coop', 1000, { need: 100_000, scratch: 32_000 })];
    expect(planMemory(files, KNOWN, { build: 'e14', report: report(132_000, 8_000_000) })).toMatchObject({
      rule: 'arena',
      budget: 8_000_000,
      budgetKnown: true,
      approximate: false,
      used: 132_000,
    });
    // Without the header's figures the newest budget the site knows stands in, and the plan says so.
    expect(planMemory([onDevice('a.coop', 1000)], KNOWN, { build: 'e14' })).toMatchObject({ rule: 'arena', budget: E12, budgetKnown: false, approximate: true });
  });

  it('names the files the boot leaves out: it loads in name order and skips each that does not fit', () => {
    const big = (file: string, need: number, scratch = 0): DeviceCoopFile => onDevice(file, 1, { need, scratch });
    const files = [big('zeta.coop', 300_000), big('alpha.coop', 400_000, 100_000), big('mid.coop', 500_000, 200_000), big('beta.coop', 150_000)];
    const plan = planMemory(files, [], { build: 'e12', report: report(0, 1_000_000) }) as MemoryPlan;
    expect(plan.level).toBe('over');
    // alpha 400 + buffer 100 = 500; beta 650; mid would add 500 and grow the buffer by 100; zeta takes it to 950.
    expect(plan.refused).toEqual(['mid.coop']);
    expect(planMemory(files.slice(0, 2), [], { build: 'e12', report: report(0, 1_000_000) })?.refused).toEqual([]);
  });

  it('says how many effect positions get a buffer when the set leaves no room for all four', () => {
    const files = [onDevice('fx.coop', 1, { need: 100_000, scratch: 200_000 }), onDevice('big.coop', 1, { need: 400_000, scratch: 0 })];
    const at = (budget: number): number | null | undefined => planMemory(files, [], { build: 'e12', report: report(0, budget) })?.positions;
    expect(at(1_300_000)).toBe(4);
    expect(at(1_299_999)).toBe(3);
    expect(at(700_000)).toBe(1);
    expect(planMemory([files[1] as DeviceCoopFile], [], { build: 'e12', report: report(0) })?.positions).toBeNull();
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

describe('formatKb and clipSeconds', () => {
  it('writes whole KB with thousands separators', () => {
    expect(formatKb(1_450_224)).toBe('1,416 KB');
    expect(formatKb(BUDGET)).toBe('1,536 KB');
  });

  it('turns the clipboard pool into seconds of stereo tape', () => {
    expect(clipSeconds(4_194_304)).toBe(47);
    expect(clipSeconds(1_572_864)).toBe(17);
  });
});
