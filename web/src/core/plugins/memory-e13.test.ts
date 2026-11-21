/**
 * Core e13: plugin memory in two places, heap 0 and coop's arena, by a boot policy, each with a budget
 * (firmware/docs/plugins.md "e13", "The rule for the site"; firmware/docs/sysex.md "INVENTORY on e13").
 */
import { existsSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';
import { FakeCoop } from '../../../test/fake-coop';
import { deviceFilesOf, deviceReportOf, updateMemoryQuestion } from '../usbmidi/coop-update';
import { parseCoopInventory, readCoopInventory } from '../usbmidi/coop';
import { createSession } from '../usbmidi/session';
import {
  DEFAULT_PLACEMENT,
  blk,
  budgetOfCore,
  coopMemoryOf,
  ownBytes,
  parsePlacement,
  planMemory,
  ruleOfCore,
  setPools,
  shortNote,
  type CoopMemory,
  type DeviceCoopFile,
  type DeviceMemoryReport,
  type KnownCoopFile,
  type MemoryPlan,
  type Placement,
} from './memory';

const HEAP = 1728 * 1024;
const ARENA = 5760 * 1024;
const place = (text: string): Placement => parsePlacement(text) as Placement;
const ALL_HEAP = place('eng=heap,fx=heap,voice=heap,buf=heap');
const ALL_ARENA = place('eng=arena,fx=arena,voice=arena,buf=arena');
const FX_ARENA = place('eng=heap,fx=arena,voice=heap,buf=arena');

const fxOf = (cap: number, scratch: number): CoopMemory => ({ kind: 'fx', cap, scratch, voice: 0, screen: true, controls: true });

/**
 * The facts of the 13 catalogue files the e13 gate ran with (rings 0.10 and tape_echo 0.4, the files of commit
 * 2f1b8e9), as the plugin index carried them. The emulator's figures for four policies are the vectors below.
 */
const GATE_SET: Record<string, CoopMemory> = {
  bitcrusher: fxOf(4467, 32),
  cs_hall: fxOf(18431, 86752),
  cs_plate: fxOf(13039, 77664),
  drive: fxOf(10083, 1600),
  ensemble: fxOf(11387, 10400),
  fickle: fxOf(26567, 134784),
  freq_shift: fxOf(13303, 4640),
  petri: fxOf(28431, 134752),
  rings: { kind: 'synth', cap: 666831, scratch: 0, voice: 0, screen: true, controls: true },
  saw: { kind: 'synth', cap: 2275, scratch: 0, voice: 0, screen: true, controls: true },
  tape_drive: fxOf(13235, 3392),
  tape_echo: fxOf(14051, 134784),
  wavefolder: fxOf(10511, 960),
};
const gate = Object.values(GATE_SET);

describe('the e13 policy', () => {
  it('reads the header’s place field', () => {
    expect(parsePlacement('eng=heap,fx=heap,voice=heap,buf=arena')).toEqual(DEFAULT_PLACEMENT);
    expect(parsePlacement('buf=heap,voice=arena,fx=arena,eng=heap')).toEqual({ eng: 'heap', fx: 'arena', voice: 'arena', buf: 'heap' });
    for (const bad of [null, '', 'eng=heap', 'eng=heap,fx=heap,voice=heap,buf=l1', 'all=heap']) expect(parsePlacement(bad), String(bad)).toBeNull();
  });

  it('is the rule of e13 and of any later core, with heap 0’s budget as the one the table gives', () => {
    expect(['e13', 'e13-probe', 'e14'].map(ruleOfCore)).toEqual(['placed', 'placed', 'placed']);
    expect(ruleOfCore('e12')).toBe('arena');
    expect(budgetOfCore('e13')).toEqual({ bytes: HEAP, known: true });
  });
});

describe('the e13 sums', () => {
  it('counts an image, voice state and the buffers where the policy puts them', () => {
    const engine: CoopMemory = { kind: 'synth', cap: 2000, scratch: 0, voice: 160, screen: true, controls: true };
    const effect = fxOf(1000, 4096);
    const old: CoopMemory = { kind: 'fx', cap: 1000, scratch: 0, voice: 0, screen: false, controls: false };
    // Default: images and voices in heap 0 as heap blocks, screens and controls in the arena with one buffer.
    expect(setPools([engine], 'placed')).toEqual({ heap: blk(2032) + blk(6 * 160 + 32), arena: 256 + 1408 });
    expect(setPools([effect], 'placed')).toEqual({ heap: blk(1032), arena: 256 + 1408 + 4096 });
    expect(setPools([old], 'placed')).toEqual({ heap: 5 * blk(1032), arena: 0 });
    // Everything in the arena is e12's sum.
    expect(setPools([engine, effect, old], 'placed', ALL_ARENA)).toEqual({ heap: 0, arena: setPools([engine, effect, old], 'arena').arena });
    // Buffers in heap 0: all four in one block. Voices in the arena: six of them, no block header.
    expect(setPools([effect], 'placed', ALL_HEAP)).toEqual({ heap: blk(1032) + blk(4 * 4096 + 32), arena: 256 + 1408 });
    expect(setPools([engine], 'placed', place('eng=heap,fx=heap,voice=arena,buf=arena'))).toEqual({ heap: blk(2032), arena: 6 * 160 + 256 + 1408 });
    expect(setPools([engine, effect], 'placed', FX_ARENA)).toEqual({ heap: blk(2032) + blk(992), arena: 1024 + 2 * (256 + 1408) + 4096 });
    expect(ownBytes(engine, 'placed')).toBe(blk(2032) + blk(992) + 1664);
  });

  it('gives the emulator’s figures for the 13 gate files under each policy', () => {
    // plugmem ... heap_used / arena_used (firmware/docs/plugins.md "e13", the gate; device-tests.md "e13").
    expect(setPools(gate, 'placed')).toEqual({ heap: 833_168, arena: 156_416 });
    expect(setPools(gate, 'placed', ALL_HEAP)).toEqual({ heap: 1_372_344, arena: 21_632 });
    expect(setPools(gate, 'placed', ALL_ARENA)).toEqual({ heap: 0, arena: 989_248 });
    expect(setPools(gate, 'placed', FX_ARENA)).toEqual({ heap: 669_192, arena: 320_096 });
    // The inventory's rows of that run: rings need 668536 heap 666872, tape_echo need 15760 heap 14096.
    const rings = setPools([GATE_SET.rings as CoopMemory], 'placed');
    expect([rings.heap + rings.arena, rings.heap]).toEqual([668_536, 666_872]);
    expect(ownBytes(GATE_SET.tape_echo as CoopMemory, 'placed')).toBe(15_760);
    expect(blk((GATE_SET.tape_echo as CoopMemory).cap + 32)).toBe(14_096);
  });

  /**
   * Today's catalogue (rings 0.11, tape_echo 0.5) by the same rule: not gated in the emulator. The sizes pin the
   * builds; when one of the files is rebuilt this steps aside until the figures are brought up to date.
   */
  const PLUGINS = fileURLToPath(new URL('../../../../plugins', import.meta.url));
  const NOW: Record<string, number> = {
    bitcrusher: 13076,
    cs_hall: 23936,
    cs_plate: 19956,
    drive: 19516,
    ensemble: 19928,
    fickle: 38164,
    freq_shift: 23396,
    petri: 37792,
    rings: 114948,
    saw: 6520,
    tape_drive: 22660,
    tape_echo: 25444,
    wavefolder: 14304,
  };
  const file = (id: string): string => `${PLUGINS}/${id}/${id}.coop`;
  const unchanged = Object.entries(NOW).every(([id, size]) => existsSync(file(id)) && readFileSync(file(id)).length === size);

  it.skipIf(!unchanged)('adds up the 13 catalogue files as they are now', () => {
    const now = Object.keys(NOW).map((id) => coopMemoryOf(new Uint8Array(readFileSync(file(id)))));
    expect(setPools(now, 'placed')).toEqual({ heap: 834_520, arena: 156_416 });
    expect(setPools(now, 'placed', ALL_HEAP)).toEqual({ heap: 1_373_696, arena: 21_632 });
    expect(setPools(now, 'placed', ALL_ARENA)).toEqual({ heap: 0, arena: 990_592 });
    expect(setPools(now, 'placed', FX_ARENA)).toEqual({ heap: 670_288, arena: 320_352 });
  });
});

const onDevice = (file: string, more: Partial<DeviceCoopFile> = {}): DeviceCoopFile => ({ file, size: 1000, kind: 'fx', loaded: true, enabled: true, reason: '', ...more });
const report = (heapUsed: number, arenaUsed: number, more: Partial<DeviceMemoryReport> = {}): DeviceMemoryReport => ({
  budget: HEAP,
  used: heapUsed,
  exact: true,
  needs: {},
  pools: { heap: { budget: HEAP, used: heapUsed }, arena: { budget: ARENA, used: arenaUsed } },
  place: DEFAULT_PLACEMENT,
  placeSrc: 'default',
  ...more,
});
const known = (id: string): KnownCoopFile => ({ file: `${id}.coop`, sizeBytes: 1000, memory: GATE_SET[id] as CoopMemory });

describe('planMemory on e13', () => {
  it('shows the memory with the least room, and the other beside it', () => {
    const files = ['rings', 'tape_echo', 'saw'].map((id) => onDevice(`${id}.coop`, { kind: GATE_SET[id]?.kind ?? 'fx' }));
    const plan = planMemory(files, ['rings', 'tape_echo', 'saw'].map(known), { build: 'e13' }) as MemoryPlan;
    const pools = setPools([GATE_SET.rings, GATE_SET.tape_echo, GATE_SET.saw] as CoopMemory[], 'placed');
    expect(plan).toMatchObject({ rule: 'placed', budget: HEAP, used: pools.heap, level: 'fits', approximate: false, place: DEFAULT_PLACEMENT, placeSrc: null });
    expect(plan.pools).toEqual([
      { pool: 'heap', used: pools.heap, budget: HEAP, level: 'fits', holds: 'code and voices' },
      { pool: 'arena', used: pools.arena, budget: ARENA, level: 'fits', holds: 'buffers, screens and controls' },
    ]);
    // A row is the file's bytes in both memories; what removing it gives back is counted in the memory shown.
    expect(plan.rows[0]).toMatchObject({ file: 'rings.coop', bytes: 668_536, frees: 666_872 });
    expect(plan).toMatchObject({ shared: 134_784, sharedBy: 'tape_echo.coop', positions: 4, refused: [] });
    expect(shortNote(plan)).toBeNull();
  });

  it('takes the budgets, the policy and each file’s two parts from the OP-1', () => {
    const files = [
      onDevice('rings.coop', { kind: 'synth', need: 668_536, heap: 666_872, scratch: 0 }),
      onDevice('mine.coop', { need: 50_000, heap: 0, scratch: 64_000 }),
    ];
    const own = report(666_872, 1_664 + 50_000 + 64_000, { pools: { heap: { budget: 700_000, used: 666_872 }, arena: { budget: 4_000_000, used: 115_664 } }, budget: 700_000 });
    const plan = planMemory(files, [], { build: 'e15', report: own }) as MemoryPlan;
    expect(plan).toMatchObject({ rule: 'placed', budget: 700_000, used: 666_872, budgetKnown: true, approximate: false, level: 'nearly' });
    expect(plan.pools.map((p) => [p.pool, p.used, p.budget, p.level])).toEqual([
      ['heap', 666_872, 700_000, 'nearly'],
      ['arena', 115_664, 4_000_000, 'fits'],
    ]);
    expect(plan.rows.map((r) => [r.file, r.bytes, r.exact])).toEqual([
      ['rings.coop', 668_536, true],
      ['mine.coop', 50_000, true],
    ]);
    expect(plan.loadedNow).toEqual({ bytes: 666_872, fromDevice: true, exact: true });
  });

  it('follows a policy file: everything in the arena is one memory again, everything in heap 0 leaves screens there', () => {
    const files = [onDevice('rings.coop', { kind: 'synth' }), onDevice('tape_echo.coop')];
    const set = ['rings', 'tape_echo'].map(known);
    const arena = planMemory(files, set, { build: 'e13', report: report(0, 0, { place: ALL_ARENA, placeSrc: 'file', budget: ARENA, used: 0 }) }) as MemoryPlan;
    expect(arena.pools.map((p) => [p.pool, p.holds])).toEqual([['arena', 'code, voices, buffers, screens and controls']]);
    expect(arena).toMatchObject({ budget: ARENA, placeSrc: 'file', place: ALL_ARENA });
    expect(arena.used).toBe(setPools([GATE_SET.rings, GATE_SET.tape_echo] as CoopMemory[], 'arena').arena);

    const heap = planMemory(files, set, { build: 'e13', report: report(0, 0, { place: ALL_HEAP, placeSrc: 'file' }) }) as MemoryPlan;
    expect(heap.pools.map((p) => [p.pool, p.holds])).toEqual([
      ['heap', 'code, voices and buffers'],
      ['arena', 'screens and controls'],
    ]);
    expect(heap.shared).toBe(blk(4 * 134_784 + 32));
    // With the buffers in heap 0 all four always exist.
    expect(heap.positions).toBeNull();

    const split = planMemory(files, set, { build: 'e13', report: report(0, 0, { place: FX_ARENA, placeSrc: 'malformed' }) }) as MemoryPlan;
    expect(split.pools.map((p) => p.holds).sort()).toEqual(['effect code, buffers, screens and controls', 'engine code and voices']);
    expect(split.placeSrc).toBe('malformed');
  });

  it('is over when either memory is, says which, and names the files the boot leaves out', () => {
    // Heap 0 short: two engines of 400 KB of code where 700 KB is allowed; the arena has room.
    const big = (file: string, heap: number, arena: number, scratch = 0): DeviceCoopFile => onDevice(file, { need: heap + arena, heap, scratch });
    const tight = (heap: number, arena: number): DeviceMemoryReport =>
      report(0, 0, { pools: { heap: { budget: heap, used: 0 }, arena: { budget: arena, used: 0 } }, budget: heap });
    const code = planMemory([big('a.coop', 400_000, 2_000), big('b.coop', 400_000, 2_000), big('c.coop', 100_000, 2_000)], [], { build: 'e13', report: tight(700_000, 1_000_000) }) as MemoryPlan;
    expect(code).toMatchObject({ level: 'over', budget: 700_000, used: 900_000, refused: ['b.coop'] });
    expect(code.pools.map((p) => [p.pool, p.level])).toEqual([
      ['heap', 'over'],
      ['arena', 'fits'],
    ]);
    expect(shortNote(code)).toBe('What is short is the memory for code and voices.');

    // The arena short: buffers and screens over, heap 0 with room. The bar turns to the arena.
    const buffers = planMemory([big('a.coop', 10_000, 2_000, 300_000), big('b.coop', 10_000, 250_000)], [], { build: 'e13', report: tight(700_000, 500_000) }) as MemoryPlan;
    expect(buffers).toMatchObject({ level: 'over', budget: 500_000, used: 552_000, refused: ['b.coop'] });
    expect(buffers.pools[0]).toMatchObject({ pool: 'arena', level: 'over' });
    expect(shortNote(buffers)).toBe('What is short is the memory for buffers, screens and controls.');

    // Both short.
    const both = planMemory([big('a.coop', 800_000, 600_000)], [], { build: 'e13', report: tight(700_000, 500_000) }) as MemoryPlan;
    expect(both.refused).toEqual(['a.coop']);
    expect(shortNote(both)).toMatch(/^What is short is the memory for .*, and the one for .*\.$/);
  });

  it('checks a file about to be sent against both memories', () => {
    const files = [onDevice('rings.coop', { kind: 'synth', need: 668_536, heap: 666_872 })];
    const own = report(666_872, 1_664, { pools: { heap: { budget: 700_000, used: 666_872 }, arena: { budget: ARENA, used: 1_664 } }, budget: 700_000 });
    // tape_echo: 14 KB of code, which heap 0 has room for; with fickle's 27 KB the last by name no longer fits.
    expect(planMemory(files, [], { build: 'e13', report: own, add: known('tape_echo') })).toMatchObject({ level: 'nearly', used: 666_872 + 14_096 });
    const over = planMemory(files, [], { build: 'e13', report: own, add: [known('tape_echo'), known('fickle')] }) as MemoryPlan;
    expect(over).toMatchObject({ level: 'over', refused: ['tape_echo.coop'] });
    expect(shortNote(over)).toBe('What is short is the memory for code and voices.');
  });

  it('says how many effect positions get a buffer by the arena’s room', () => {
    const files = [onDevice('fx.coop', { need: 110_000, heap: 100_000, scratch: 200_000 })];
    const at = (arena: number): number | null | undefined =>
      planMemory(files, [], { build: 'e13', report: report(0, 0, { pools: { heap: { budget: HEAP, used: 0 }, arena: { budget: arena, used: 0 } } }) })?.positions;
    expect(at(810_000)).toBe(4);
    expect(at(809_999)).toBe(3);
    expect(at(210_000)).toBe(1);
  });

  it('counts an e12 inventory, and a later core’s that names one budget, e12’s way', () => {
    const files = [onDevice('a.coop', { need: 100_000, scratch: 32_000 })];
    const one: DeviceMemoryReport = { budget: ARENA, used: 132_000, exact: true, needs: {} };
    expect(planMemory(files, [], { build: 'e12', report: one })).toMatchObject({ rule: 'arena', used: 132_000, place: null });
    expect(planMemory(files, [], { build: 'e14', report: one })).toMatchObject({ rule: 'arena', used: 132_000, budget: ARENA });
    expect(planMemory(files, [], { build: 'e12', report: one })?.pools).toHaveLength(1);
  });
});

describe('the e13 inventory', () => {
  const header =
    'coop-inventory 1\tbuild e13\tprotocol 1\tbudget 1769472\tused 833168\tfxbuf 134784\tfxbufs 4\tclip 4194304' +
    '\theap_budget 1769472\theap_used 833168\tarena_budget 5898240\tarena_used 156416\tplace eng=heap,fx=heap,voice=heap,buf=arena\tplace_src default\n';
  const rows =
    'rings.coop\tsynth\trings\tloaded\ton\t0.10\tcoldsoldr\t113816\t\t668536\t0\t666872\n' +
    'tape_echo.coop\tfx\ttape echo\tloaded\ton\t0.4\tcoldsoldr\t25188\t\t15760\t134784\t14096\n';

  it('reads both budgets, the policy and each row’s heap part', () => {
    const inv = parseCoopInventory(header + rows);
    expect(inv.memory).toEqual({
      budget: 1769472,
      used: 833168,
      fxbuf: 134784,
      fxbufs: 4,
      clip: 4194304,
      pools: { heap: { budget: 1769472, used: 833168 }, arena: { budget: 5898240, used: 156416 } },
      place: DEFAULT_PLACEMENT,
      placeSrc: 'default',
    });
    expect(inv.records.map((r) => [r.name, r.need, r.scratch, r.heap])).toEqual([
      ['rings', 668536, 0, 666872],
      ['tape_echo', 15760, 134784, 14096],
    ]);
    const plan = planMemory(deviceFilesOf(inv), [], { build: inv.build, report: deviceReportOf(inv) }) as MemoryPlan;
    expect(plan).toMatchObject({ rule: 'placed', budget: 1769472, used: 666872 + 14096, placeSrc: 'default' });
    expect(plan.pools[1]).toMatchObject({ pool: 'arena', used: 1664 + 1664 + 134784, budget: 5898240 });
    expect(plan.loadedNow).toEqual({ bytes: 833168, fromDevice: true, exact: true });
  });

  it('still reads an e12 inventory and one from before, with nothing of e13 made up', () => {
    const e12 = parseCoopInventory(
      'coop-inventory 1\tbuild e12\tprotocol 1\tbudget 5898240\tused 980992\tfxbuf 134784\tfxbufs 4\tclip 4194304\n' + 'rings.coop\tsynth\trings\tloaded\ton\t0.5\tcoldsoldr\t113048\t\t667712\t0\n',
    );
    expect(e12.memory).toEqual({ budget: 5898240, used: 980992, fxbuf: 134784, fxbufs: 4, clip: 4194304 });
    expect(e12.records[0]).toMatchObject({ need: 667712, scratch: 0 });
    expect(e12.records[0]).not.toHaveProperty('heap');
    expect(deviceReportOf(e12)).not.toHaveProperty('pools');
    const e10 = parseCoopInventory('coop-inventory 1\tbuild e10\tprotocol 1\nsaw.coop\tsynth\tsaw\tloaded\ton\t0.2\tcoldsoldr\t6520\t\n');
    expect(e10.memory).toBeUndefined();
    expect(e10.records[0]).not.toHaveProperty('need');
    expect(deviceReportOf(e10)).toBeNull();
  });

  it('keeps a policy it cannot read out, and one budget when a figure is missing', () => {
    const inv = parseCoopInventory('coop-inventory 1\tbuild e13\tprotocol 1\tbudget 1769472\tused 5\theap_budget 1769472\tplace eng=l1\tplace_src file\n');
    expect(inv.memory).toEqual({ budget: 1769472, used: 5, fxbuf: 0, fxbufs: 0, clip: null, placeSrc: 'file' });
  });

  it('comes page by page from the fake device, with the heap column', async () => {
    const device = new FakeCoop({
      build: 'e13',
      files: { 'rings.coop': new Uint8Array(100) },
      boot: { 'rings.coop': { kind: 'synth', name: 'rings', state: 'loaded', version: '0.10', need: 668536, heap: 666872 } },
      memory: { budget: 1769472, used: 666872, e13: { heapBudget: 1769472, heapUsed: 666872, arenaBudget: 5898240, arenaUsed: 1664, place: 'eng=heap,fx=arena,voice=heap,buf=arena', placeSrc: 'file' } },
    });
    const inv = await readCoopInventory(createSession(device));
    expect(inv.memory).toMatchObject({ pools: { heap: { used: 666872 }, arena: { used: 1664 } }, place: FX_ARENA, placeSrc: 'file' });
    expect(inv.records[0]).toMatchObject({ need: 668536, heap: 666872 });
  });

  it('asks before an update that takes heap 0 over, and says which memory is short', () => {
    const inv = parseCoopInventory(header.replace('heap_budget 1769472', 'heap_budget 700000') + rows);
    const update = { plugin: { sizeBytes: 1000 } as never, file: 'tape_echo.coop', from: '0.4', to: '0.5' };
    const grown: KnownCoopFile = { file: 'tape_echo.coop', sizeBytes: 1000, memory: fxOf(40_000, 134784) };
    const question = updateMemoryQuestion(inv, [grown], [update]) as string;
    expect(question).toContain('those switched on need 690 KB; the OP-1 has 684 KB.');
    expect(question).toContain('What is short is the memory for code and voices.');
    expect(question).toContain('would leave out tape echo');
    expect(updateMemoryQuestion(inv, [{ ...grown, memory: fxOf(14_051, 134784) }], [update])).toBeNull();
  });
});
