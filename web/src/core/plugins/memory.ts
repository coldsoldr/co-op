/**
 * The coop firmware's plugin memory: what each .coop file takes of it at boot and whether a set of files fits.
 *
 * The core loads the files of /yaffs2/plugins into a memory with a budget (firmware/core/sdram/plugin_host.cc
 * load_one and load_engine; firmware/docs/plugins.md "e7", "e8" and "e12"); a file that would go over it is refused
 * at boot ("over the plugin memory budget") and only loads once something else is removed. Two rules, by core:
 *
 * e7..e11 (`heap`: the stock's heap 0, 1536 KiB, 1728 KiB on e11):
 *
 *     blk(n) = (n + 15) & ~7                                the heap block MALLOC(n) takes
 *     cap    = bss_offset + bss_size + 3 + 12 * imports
 *     effect, old model:   5 * blk(cap + 32)                one relocation per position
 *     effect, scratch:     blk(cap + 32), and the five position buffers once for all scratch effects together:
 *                          blk(5 * E + 32), E = the biggest 32-rounded "scratch" among them
 *     synth engine:        blk(cap + 32) + blk(stride * 6 + 32), stride = voice_bytes rounded up to 32 (nothing when 0)
 *     with a screen:       + blk(240), the core's screen object (engine_screen.cc struct escr)
 *
 * e12 and later (`arena`: coop's own memory, 5760 KiB):
 *
 *     r32(n) = (n + 31) & ~31                               every arena block
 *     effect, old model:   5 * r32(cap)
 *     effect, scratch:     r32(cap), and ONE position buffer E in the budget for all scratch effects together
 *     synth engine:        r32(cap) + 6 * stride
 *     with a screen:       + r32(240) = 256
 *     with controls:       + r32(1384) = 1408, the control table (engine_ui.cc struct ctls)
 *
 * The total of a set does not depend on the order; which file the core refuses does. e12 loads in name order, so
 * there the refused files can be named; before it the order is the directory's, which the site cannot see.
 */
import type { CoopMemory } from '@co-op/shared/plugins';

export type { CoopMemory };

/** How a core counts: against the stock's heap (e7..e11) or in coop's own arena (e12 on). */
export type MemoryRule = 'heap' | 'arena';

/** The budget by core: the tag HELLO reports. A core before e7 has none. */
const BUDGET_BY_CORE: Record<string, number> = {
  e7: 1536 * 1024,
  e8: 1536 * 1024,
  e9: 1536 * 1024,
  e10: 1536 * 1024,
  e11: 1728 * 1024,
  e12: 5760 * 1024,
};
/** For a core this table does not know yet: the newest budget it does know (the note under the bar says so). */
const NEWEST_KNOWN_BUDGET = 5760 * 1024;
/** The first core that counts in the arena. */
const ARENA_FROM = 12;

const coreNumber = (build: string | null | undefined): number | null => {
  const tag = /^e(\d+)/.exec(build ?? '');
  return tag ? Number(tag[1]) : null;
};

/**
 * The budget of the core with this build tag ("e10", or "e10-something"), whether the table knows the core, or null
 * for a core before e7, which loads without a budget.
 */
export function budgetOfCore(build: string | null | undefined): { bytes: number; known: boolean } | null {
  const n = coreNumber(build);
  if (n === null) return { bytes: NEWEST_KNOWN_BUDGET, known: false };
  if (n < 7) return null;
  const bytes = BUDGET_BY_CORE[`e${n}`];
  return bytes === undefined ? { bytes: NEWEST_KNOWN_BUDGET, known: false } : { bytes, known: true };
}

/** The rule of the core with this build tag; a core the site does not know counts the newest way. */
export function ruleOfCore(build: string | null | undefined): MemoryRule {
  const n = coreNumber(build);
  return n !== null && n < ARENA_FROM ? 'heap' : 'arena';
}

const POSITIONS = 5;
/** e12: the position buffers the core carves when they fit (one of them is in the budget). */
const ARENA_BUFFERS = 4;
const VOICES = 6;
const ARENA_ALIGN = 32;
const STUB_SIZE = 12;
const VOICE_BYTES_MAX = 8192;
/** sizeof(struct escr): 32 object words, 10 vtable words, a 56-byte path, two pointers, int16 and three bytes, padded. */
const SCREEN_OBJECT_BYTES = 240;
/** sizeof(struct ctls), the control table e12 counts for a file with controls. */
const CONTROL_TABLE_BYTES = 1384;
const HEADER_SIZE = 144;
const KIND_SYNTH = 1;
const KIND_FX = 2;
const FLAG_SCRATCH = 0x20;
const EXT_CONTROLS = 6;
const EXT_SCRATCH = 7;
const FX_F_SCREEN = 1;
const R_ABS32 = 1;
const RELOC_SIZE = 12;

/** The heap block MALLOC(n) takes: the size with its 8-byte header, rounded up to 8. */
export const blk = (n: number): number => ((n + 15) & ~7) >>> 0;
/** An arena block: rounded up to 32. */
export const r32 = (n: number): number => ((n + 31) & ~31) >>> 0;
/**
 * What the budget counts for the position buffers of a set whose largest is `each` bytes (32-rounded): the one heap
 * block that holds all five before e12, one buffer from e12; 0 when no scratch effect loads.
 */
export const positionBuffers = (each: number, rule: MemoryRule = 'heap'): number =>
  each <= 0 ? 0 : rule === 'heap' ? blk(POSITIONS * each + ARENA_ALIGN) : each;

export class CoopFileError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'CoopFileError';
  }
}

/**
 * The facts of a .coop file both rules count from, read from its header (firmware/tools/plugin/reloc.py HEADER),
 * its extension table and its entry object. Only what the sum needs is checked; checkCoop (shared) is the file
 * check. Throws CoopFileError for a file it cannot read.
 */
export function coopMemoryOf(bytes: Uint8Array): CoopMemory {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const isMagic = (at: number): boolean =>
    at + 4 <= bytes.length && bytes[at] === 0x43 && bytes[at + 1] === 0x4f && bytes[at + 2] === 0x4f && bytes[at + 3] === 0x50;
  // A file with a screen starts with the SVG text and its NUL; the header follows on the next 4-byte boundary.
  let h = 0;
  if (!isMagic(0)) {
    const nul = bytes.indexOf(0);
    h = (nul + 4) & ~3;
    if (nul < 0 || bytes[0] !== 0x3c || !isMagic(h)) throw new CoopFileError('not a coop plugin (no COOP header)');
  }
  if (bytes.length < h + HEADER_SIZE) throw new CoopFileError('too short for the coop header');
  const u32 = (at: number): number => {
    if (at < 0 || at + 4 > bytes.length) throw new CoopFileError('a header offset points outside the file');
    return view.getUint32(at, true);
  };
  const kind = bytes[h + 18];
  if (kind !== KIND_SYNTH && kind !== KIND_FX) throw new CoopFileError(`kind ${kind} is not an effect or a synth engine`);
  const flags = u32(h + 36);
  const loadSize = u32(h + 76) + u32(h + 80); // bss_offset + bss_size
  const imageSize = u32(h + 84);
  const entry = u32(h + 92);
  const image = u32(h + 100);
  const relocs = u32(h + 104);
  const relocCount = u32(h + 108);
  const imports = u32(h + 116);
  const hasSvg = h !== 0 && u32(h + 132) !== 0;
  const ext = u32(h + 140);
  /** A word of the entry object, as it is in the file (before relocation); 0 past the image, where .bss starts. */
  const entryWord = (offset: number): number => (entry + offset + 4 <= imageSize ? u32(image + entry + offset) : 0);
  const cap = loadSize + 3 + STUB_SIZE * imports;

  // The extension table: u16 tag, u16 length, the bytes, padding to 4; tag 0 ends it.
  let scratch = 0;
  let controls = false;
  for (let at = ext; ext !== 0; ) {
    const word = u32(at);
    const tag = word & 0xffff;
    const length = word >>> 16;
    if (tag === 0) break;
    if (tag === EXT_SCRATCH && length === 4) scratch = r32(u32(at + 4));
    // The control declaration is text; an empty one declares nothing and gets no table.
    if (tag === EXT_CONTROLS && length > 0 && at + 4 < bytes.length) controls = bytes[at + 4] !== 0;
    at += 4 + ((length + 3) & ~3);
  }

  if (kind === KIND_SYNTH) {
    const voiceBytes = entryWord(12);
    // coop_engine_t.screen (+32) is a pointer: set when the word is, or when a relocation fills it at load.
    let hasScreen = entryWord(32) !== 0;
    for (let i = 0; i < relocCount && !hasScreen; i++) {
      const at = relocs + i * RELOC_SIZE;
      hasScreen = u32(at) === entry + 32 && bytes[at + 4] === R_ABS32;
    }
    // Over the limit the core refuses the engine; it then holds no voice state.
    return { kind: 'synth', cap, scratch: 0, voice: voiceBytes <= VOICE_BYTES_MAX ? r32(voiceBytes) : 0, screen: hasSvg && hasScreen, controls };
  }

  if (flags & FLAG_SCRATCH) {
    if (scratch === 0) throw new CoopFileError('the scratch flag is set but the file names no buffer size');
  } else scratch = 0;
  return { kind: 'fx', cap, scratch, voice: 0, screen: hasSvg && (entryWord(8) & FX_F_SCREEN) !== 0, controls };
}

/** What the file holds for itself under a rule: its image, voice state, screen object and control table, without the shared position buffer. */
export function ownBytes(m: CoopMemory, rule: MemoryRule): number {
  const copies = m.kind === 'fx' && m.scratch === 0 ? POSITIONS : 1;
  if (rule === 'heap') {
    return copies * blk(m.cap + ARENA_ALIGN) + (m.voice ? blk(m.voice * VOICES + 32) : 0) + (m.screen ? blk(SCREEN_OBJECT_BYTES) : 0);
  }
  return copies * r32(m.cap) + VOICES * m.voice + (m.screen ? r32(SCREEN_OBJECT_BYTES) : 0) + (m.controls ? r32(CONTROL_TABLE_BYTES) : 0);
}

/** What one file adds to a set: its own bytes, and the position buffer it asks for (0: none). */
export interface Cost {
  own: number;
  scratch: number;
}
export const costOf = (m: CoopMemory, rule: MemoryRule): Cost => ({ own: ownBytes(m, rule), scratch: m.scratch });

const sumOf = (costs: readonly Cost[], rule: MemoryRule): number =>
  costs.reduce((n, c) => n + c.own, 0) + positionBuffers(Math.max(0, ...costs.map((c) => c.scratch)), rule);

/** What a set of files takes together: each one's own bytes and the position buffers once, sized by the largest. */
export const setBytes = (files: readonly CoopMemory[], rule: MemoryRule): number =>
  sumOf(
    files.map((m) => costOf(m, rule)),
    rule,
  );

/**
 * A stand-in for a file whose header the site has not got and the OP-1 does not size: only its file size (and
 * perhaps its kind) is known. A scratch effect or an engine takes about its file size once; an old-model effect five
 * times that, so an effect of unknown build is counted the old way (the larger figure) and a file of unknown kind as
 * an effect. Its working memory (.bss, position buffer, voice state) is not in the file size, so this can be far too
 * low; totals that use it are marked approximate.
 */
export function estimateFromSize(size: number, kind: 'fx' | 'synth' | null, rule: MemoryRule): Cost {
  const once = size <= 0 ? 0 : rule === 'heap' ? blk(size + ARENA_ALIGN) : r32(size);
  return { own: kind === 'synth' ? once : POSITIONS * once, scratch: 0 };
}

/** The bar's three states: it fits, it fits but little is left, it does not fit. */
export type MemoryLevel = 'fits' | 'nearly' | 'over';
/** Above this share of the budget the bar warns (hundekraft's workshop page uses the same mark). */
export const NEARLY_FULL = 0.92;
export const levelOf = (used: number, budget: number): MemoryLevel =>
  used > budget ? 'over' : used > budget * NEARLY_FULL ? 'nearly' : 'fits';

/** A plugin file co-op has the header of: one from the catalogue, or one this page sent to the OP-1. */
export interface KnownCoopFile {
  file: string;
  sizeBytes: number;
  memory: CoopMemory;
}

/** A file on the OP-1, as its inventory names it. */
export interface DeviceCoopFile {
  file: string;
  /** Before e12, 0 for a file that came after the boot: the inventory only sized what the boot read. */
  size: number;
  kind: 'fx' | 'synth' | null;
  /** The last boot loaded it. */
  loaded: boolean;
  /** The next boot will try to load it (not switched off). */
  enabled: boolean;
  /** The loader's text for a file it refused. */
  reason: string;
  /** e12: what the OP-1 says the file takes by itself; absent or 0 when the boot did not read it. */
  need?: number;
  /** e12: its position buffer, by the OP-1's count (0: none, or not read). */
  scratch?: number;
}

/** What the OP-1 itself says about its plugin memory, where it does. */
export interface DeviceMemoryReport {
  budget: number;
  /** Bytes the loaded plugins hold now. */
  used: number;
  /** False when it comes from the inventory's refusal text, which rounds to whole KB. */
  exact: boolean;
  /** What a file the boot refused would take, by file name. */
  needs: Record<string, number>;
  /** e12: how many position buffers exist (0..4), with `fxbuf` bytes each. */
  fxbufs?: number;
  fxbuf?: number;
  /** e12: the stereo clipboard's pool in bytes (the right channel of lifted tape, int16 at 44.1 kHz). */
  clip?: number;
}

const REFUSAL = /\(needs (\d+) KB, (\d+) KB of (\d+) KB left\)/;

/**
 * The budget and the bytes in use, from the inventory's text for a file refused for memory: "... (needs 300 KB,
 * 122 KB of 1536 KB left)". The core rounds to whole KB, so this is within 1 KB. Null when no file was refused so.
 * (An e12 inventory gives both exactly in its header; this is for the cores before.)
 */
export function reportFromInventory(files: readonly DeviceCoopFile[]): DeviceMemoryReport | null {
  let report: DeviceMemoryReport | null = null;
  for (const f of files) {
    const m = REFUSAL.exec(f.reason);
    if (!m) continue;
    const budget = Number(m[3]) * 1024;
    report ??= { budget, used: budget - Number(m[2]) * 1024, exact: false, needs: {} };
    report.needs[f.file] = Number(m[1]) * 1024;
  }
  return report;
}

/**
 * The `plugmem` line of the core's statistics file (/yaffs2/user/coop_stats.txt, plugin_host.cc coop_plugin_report)
 * and the `plugfile` lines after it: the exact budget, the bytes in use and what each refused file needs. Null when
 * the text has no whole plugmem line.
 */
export function reportFromStats(text: string): DeviceMemoryReport | null {
  const line = /^plugmem budget (\d+) used (\d+) over_budget \d+ heap_short \d+( .*)?$/m.exec(text);
  if (!line) return null;
  const needs: Record<string, number> = {};
  for (const m of text.matchAll(/^plugfile (\S+\.coop) \w+ needs (\d+)(?::.*)?$/gm)) {
    if (Number(m[2]) > 0) needs[m[1] as string] = Number(m[2]);
  }
  return { budget: Number(line[1]), used: Number(line[2]), exact: true, needs };
}

export interface MemoryRow {
  file: string;
  /** Its own bytes (image, voice state, screen object, control table). */
  bytes: number;
  /** What the next boot gets back when this file is removed or switched off: its own bytes and, when it is the
   * effect with the largest buffer, what the position buffers shrink by. 0 for a file the next boot skips anyway. */
  frees: number;
  /** False for an estimate. */
  exact: boolean;
  /** Counted in the next boot's total. */
  counted: boolean;
}

export interface MemoryPlan {
  rule: MemoryRule;
  budget: number;
  /** False when the budget is a guess for a core the table does not know. */
  budgetKnown: boolean;
  /** What the files the next boot loads take together. */
  used: number;
  level: MemoryLevel;
  /** True when a file's cost is an estimate, or the budget is a guess. */
  approximate: boolean;
  /** The position buffers' share of `used` (0 without scratch effects). */
  shared: number;
  /** The effect whose buffer sizes them: the one with the largest. Null without scratch effects. */
  sharedBy: string | null;
  /**
   * e12, with scratch effects: how many of the four effect positions get a buffer at the next boot (the budget
   * counts one; the others are carved while they fit). Null before e12 or without scratch effects.
   */
  positions: number | null;
  /**
   * e12, over the budget: the files the next boot leaves out. The core loads in name order and skips each file that
   * would take it over. Empty before e12, where the order is the directory's.
   */
  refused: string[];
  rows: MemoryRow[];
  /** The bytes the loaded plugins hold now: the OP-1's own figure where it gives one, else the same sum. */
  loadedNow: { bytes: number; fromDevice: boolean; exact: boolean };
}

/** Bytewise ascending, as the core sorts file names. */
const byName = (a: string, b: string): number => (a < b ? -1 : a > b ? 1 : 0);

/**
 * The memory of each file on the OP-1 and of the set the next boot loads (every file not switched off; `add` is a
 * file about to be sent, or several, each replacing one of the same name).
 *
 * A file is exact when the OP-1 sizes it itself (e12's `need` and `scratch`) or co-op has its header: its name and
 * size match a known file's, or it is `add`. Before e12 a file that came after the boot has no size in the
 * inventory; under a known name it is taken as that file. Anything else is estimated: from what the OP-1 says it
 * needs (a refused file), from the OP-1's used figure when it is the only loaded file co-op does not know, or from
 * its size.
 */
export function planMemory(
  files: readonly DeviceCoopFile[],
  known: readonly KnownCoopFile[],
  options: { build?: string | null | undefined; report?: DeviceMemoryReport | null; add?: KnownCoopFile | readonly KnownCoopFile[] } = {},
): MemoryPlan | null {
  const report = options.report ?? reportFromInventory(files);
  const table = budgetOfCore(options.build);
  if (!table && !report) return null;
  const rule = ruleOfCore(options.build);
  const budget = report?.budget ?? (table as { bytes: number }).bytes;
  const budgetKnown = report !== null || (table?.known ?? false);

  const resolve = (f: DeviceCoopFile): { cost: Cost; exact: boolean } | null => {
    if (f.need) return { cost: { own: f.need, scratch: f.scratch ?? 0 }, exact: true };
    const named = known.filter((k) => k.file === f.file);
    const match = named.find((k) => k.sizeBytes === f.size) ?? (f.size === 0 ? named[0] : undefined);
    return match ? { cost: costOf(match.memory, rule), exact: true } : null;
  };
  const resolved = files.map((f) => ({ f, hit: resolve(f) }));
  // The one loaded file co-op does not know holds whatever the known loaded ones do not.
  const strangers = resolved.filter((r) => !r.hit && r.f.loaded);
  const knownLoaded = sumOf(
    resolved.flatMap((r) => (r.hit && r.f.loaded ? [r.hit.cost] : [])),
    rule,
  );
  const rest = report && strangers.length === 1 ? report.used - knownLoaded : 0;

  let entries = resolved.map(({ f, hit }): { f: DeviceCoopFile; cost: Cost; exact: boolean } => {
    if (hit) return { f, ...hit };
    const need = report?.needs[f.file];
    if (need) return { f, cost: { own: need, scratch: 0 }, exact: report.exact };
    if (rest > 0 && f.loaded) return { f, cost: { own: rest, scratch: 0 }, exact: report?.exact ?? false };
    return { f, cost: estimateFromSize(f.size, f.kind, rule), exact: false };
  });
  for (const add of options.add === undefined ? [] : Array.isArray(options.add) ? (options.add as readonly KnownCoopFile[]) : [options.add as KnownCoopFile]) {
    // A file that replaces one switched off stays off: the switch goes by the name.
    const enabled = entries.find((e) => e.f.file === add.file)?.f.enabled ?? true;
    entries = entries.filter((e) => e.f.file !== add.file);
    entries.push({
      f: { file: add.file, size: add.sizeBytes, kind: add.memory.kind, loaded: false, enabled, reason: '' },
      cost: costOf(add.memory, rule),
      exact: true,
    });
  }

  const next = entries.filter((e) => e.f.enabled);
  const used = sumOf(
    next.map((e) => e.cost),
    rule,
  );
  const rows = entries.map((e): MemoryRow => {
    const without = sumOf(
      next.filter((n) => n !== e).map((n) => n.cost),
      rule,
    );
    return { file: e.f.file, bytes: e.cost.own, frees: e.f.enabled ? used - without : 0, exact: e.exact, counted: e.f.enabled };
  });
  const largest = next.reduce<(typeof next)[number] | null>((top, e) => (e.cost.scratch > (top?.cost.scratch ?? 0) ? e : top), null);
  const each = largest?.cost.scratch ?? 0;
  const own = next.reduce((n, e) => n + e.cost.own, 0);

  // e12: the boot walks the files by name and leaves out each one that would take the sum over the budget.
  const refused: string[] = [];
  if (rule === 'arena' && used > budget) {
    let sum = 0;
    let buffer = 0;
    for (const e of [...next].sort((a, b) => byName(a.f.file, b.f.file))) {
      const grown = Math.max(buffer, e.cost.scratch);
      if (sum + e.cost.own + grown - buffer > budget) refused.push(e.f.file);
      else {
        sum += e.cost.own + grown - buffer;
        buffer = grown;
      }
    }
  }

  const loaded = entries.filter((e) => e.f.loaded);
  return {
    rule,
    budget,
    budgetKnown,
    used,
    level: levelOf(used, budget),
    approximate: !budgetKnown || next.some((e) => !e.exact),
    shared: positionBuffers(each, rule),
    sharedBy: largest?.f.file ?? null,
    positions: rule === 'arena' && each > 0 ? Math.max(1, Math.min(ARENA_BUFFERS, Math.floor((budget - own) / each))) : null,
    refused,
    rows,
    loadedNow: report
      ? { bytes: report.used, fromDevice: true, exact: report.exact }
      : {
          bytes: sumOf(
            loaded.map((e) => e.cost),
            rule,
          ),
          fromDevice: false,
          exact: loaded.every((e) => e.exact),
        },
  };
}

/** The files that give the most back when removed, largest first: what to name when the set does not fit. */
export function largestRows(plan: MemoryPlan, count = 3): MemoryRow[] {
  return plan.rows
    .filter((r) => r.frees > 0)
    .sort((a, b) => b.frees - a.frees)
    .slice(0, count);
}

/** Memory for people, in whole KB: "1,416 KB". */
export const formatKb = (bytes: number): string => `${Math.round(bytes / 1024).toLocaleString('en-US')} KB`;

/** The seconds of stereo tape a clipboard pool of `bytes` lifts: its right channel, int16 at 44.1 kHz. */
export const clipSeconds = (bytes: number): number => Math.floor(bytes / (2 * 44100));
