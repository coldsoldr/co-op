/**
 * The coop firmware's plugin memory: what each .coop file takes of it at boot and whether a set of files fits.
 *
 * The core loads the files of /yaffs2/plugins from heap 0 and stops at a budget (firmware/core/sdram/plugin_host.cc
 * load_one and load_engine, firmware/docs/plugins.md "e7" and "e8"); a file that would go over it is refused at boot
 * ("over the plugin memory budget") and only loads once something else is removed. The sums here are the core's own:
 *
 *     blk(n) = (n + 15) & ~7                                the heap block MALLOC(n) takes
 *     cap    = bss_offset + bss_size + 3 + 12 * imports + 32
 *     effect, old model:   5 * blk(cap)                     one relocation per position
 *     effect, scratch:     blk(cap), and the five position buffers once for all scratch effects together:
 *                          blk(5 * largest + 32), largest = the biggest 32-rounded "scratch" among them
 *     synth engine:        blk(cap) + blk(stride * 6 + 32), stride = voice_bytes rounded up to 32 (nothing when 0)
 *     with a screen:       + blk(240), the core's screen object (engine_screen.cc struct escr)
 *
 * The total of a set does not depend on the order; which file the core refuses does (it loads in directory order).
 */
import type { CoopMemory } from '@co-op/shared/plugins';

export type { CoopMemory };

/** The budget by core: the tag HELLO reports. A core before e7 has none. */
const BUDGET_BY_CORE: Record<string, number> = {
  e7: 1536 * 1024,
  e8: 1536 * 1024,
  e9: 1536 * 1024,
  e10: 1536 * 1024,
  e11: 1728 * 1024,
};
/** For a core this table does not know yet: the newest budget it does know (the note under the bar says so). */
const NEWEST_KNOWN_BUDGET = 1728 * 1024;

/**
 * The budget of the core with this build tag ("e10", or "e10-something"), whether the table knows the core, or null
 * for a core before e7, which loads without a budget.
 */
export function budgetOfCore(build: string | null | undefined): { bytes: number; known: boolean } | null {
  const tag = /^e(\d+)/.exec(build ?? '');
  if (!tag) return { bytes: NEWEST_KNOWN_BUDGET, known: false };
  if (Number(tag[1]) < 7) return null;
  const bytes = BUDGET_BY_CORE[tag[0]];
  return bytes === undefined ? { bytes: NEWEST_KNOWN_BUDGET, known: false } : { bytes, known: true };
}

const POSITIONS = 5;
const VOICES = 6;
const ARENA_ALIGN = 32;
const STUB_SIZE = 12;
const VOICE_BYTES_MAX = 8192;
/** sizeof(struct escr): 32 object words, 10 vtable words, a 56-byte path, two pointers, int16 and three bytes, padded. */
const SCREEN_OBJECT_BYTES = 240;
const HEADER_SIZE = 144;
const KIND_SYNTH = 1;
const KIND_FX = 2;
const FLAG_SCRATCH = 0x20;
const EXT_SCRATCH = 7;
const FX_F_SCREEN = 1;
const R_ABS32 = 1;
const RELOC_SIZE = 12;

/** The heap block MALLOC(n) takes: the size with its 8-byte header, rounded up to 8. */
export const blk = (n: number): number => ((n + 15) & ~7) >>> 0;
const round32 = (n: number): number => ((n + 31) & ~31) >>> 0;
/** The one block that holds the five position buffers, each `each` bytes (32-rounded); 0 when no scratch effect loads. */
export const positionBuffers = (each: number): number => (each > 0 ? blk(POSITIONS * each + ARENA_ALIGN) : 0);

export class CoopFileError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'CoopFileError';
  }
}

/**
 * What a .coop file takes, read from its header (firmware/tools/plugin/reloc.py HEADER) and its entry object. Only
 * what the sum needs is checked; checkCoop (shared) is the file check. Throws CoopFileError for a file it cannot read.
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
  const once = blk(loadSize + 3 + STUB_SIZE * imports + ARENA_ALIGN);
  const screen = blk(SCREEN_OBJECT_BYTES);

  if (kind === KIND_SYNTH) {
    const voiceBytes = entryWord(12);
    // Over the limit the core refuses the engine; it then holds nothing.
    const stride = voiceBytes <= VOICE_BYTES_MAX ? round32(voiceBytes) : 0;
    // coop_engine_t.screen (+32) is a pointer: set when the word is, or when a relocation fills it at load.
    let hasScreen = entryWord(32) !== 0;
    for (let i = 0; i < relocCount && !hasScreen; i++) {
      const at = relocs + i * RELOC_SIZE;
      hasScreen = u32(at) === entry + 32 && bytes[at + 4] === R_ABS32;
    }
    return {
      kind: 'synth',
      image: once,
      scratch: 0,
      voices: stride ? blk(stride * VOICES + 32) : 0,
      screen: hasSvg && hasScreen ? screen : 0,
    };
  }

  let scratch = 0;
  if (flags & FLAG_SCRATCH) {
    // The extension table: u16 tag, u16 length, the bytes, padding to 4; tag 0 ends it.
    for (let at = ext; ext !== 0; ) {
      const word = u32(at);
      const tag = word & 0xffff;
      const length = word >>> 16;
      if (tag === 0) break;
      if (tag === EXT_SCRATCH && length === 4) scratch = round32(u32(at + 4));
      at += 4 + ((length + 3) & ~3);
    }
    if (scratch === 0) throw new CoopFileError('the scratch flag is set but the file names no buffer size');
  }
  return {
    kind: 'fx',
    image: flags & FLAG_SCRATCH ? once : POSITIONS * once,
    scratch,
    voices: 0,
    screen: hasSvg && entryWord(8) & FX_F_SCREEN ? screen : 0,
  };
}

/** What the file holds for itself: its image, voice state and screen object, without the shared position buffers. */
export const ownBytes = (m: CoopMemory): number => m.image + m.voices + m.screen;

/** What a set of files takes together: each one's own bytes and the position buffers once, sized by the largest. */
export function setBytes(files: readonly CoopMemory[]): number {
  return files.reduce((n, m) => n + ownBytes(m), 0) + positionBuffers(Math.max(0, ...files.map((m) => m.scratch)));
}

/**
 * A stand-in for a file whose header the site has not got: only its size (and perhaps its kind) is known. A scratch
 * effect or an engine takes about its file size once; an old-model effect five times that, so an effect of unknown
 * build is counted the old way (the larger figure) and a file of unknown kind as an effect. Its working memory
 * (.bss, position buffer, voice state) is not in the file size, so this can be far too low; totals that use it are
 * marked approximate.
 */
export function estimateFromSize(size: number, kind: 'fx' | 'synth' | null): CoopMemory {
  const once = size > 0 ? blk(size + ARENA_ALIGN) : 0;
  return { kind: kind ?? 'fx', image: kind === 'synth' ? once : POSITIONS * once, scratch: 0, voices: 0, screen: 0 };
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
  /** 0 for a file that came after the boot: the inventory only sizes what the boot read. */
  size: number;
  kind: 'fx' | 'synth' | null;
  /** The last boot loaded it. */
  loaded: boolean;
  /** The next boot will try to load it (not switched off). */
  enabled: boolean;
  /** The loader's text for a file it refused. */
  reason: string;
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
}

const REFUSAL = /\(needs (\d+) KB, (\d+) KB of (\d+) KB left\)/;

/**
 * The budget and the bytes in use, from the inventory's text for a file refused for memory: "... (needs 300 KB,
 * 122 KB of 1536 KB left)". The core rounds to whole KB, so this is within 1 KB. Null when no file was refused so.
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
  /** Its own bytes (image, voice state, screen object). */
  bytes: number;
  /** What the next boot gets back when this file is removed or switched off: its own bytes and, when it is the
   * effect with the largest buffer, what the position buffers shrink by. 0 for a file the next boot skips anyway. */
  frees: number;
  /** False for an estimate from the file size. */
  exact: boolean;
  /** Counted in the next boot's total. */
  counted: boolean;
}

export interface MemoryPlan {
  budget: number;
  /** False when the budget is a guess for a core the table does not know. */
  budgetKnown: boolean;
  /** What the files the next boot loads take together. */
  used: number;
  level: MemoryLevel;
  /** True when a file's cost is an estimate from its size, or the budget is a guess. */
  approximate: boolean;
  /** The position buffers' block within `used` (0 without scratch effects). */
  shared: number;
  /** The effect whose buffer sizes them: the one with the largest. Null without scratch effects. */
  sharedBy: string | null;
  rows: MemoryRow[];
  /** The bytes the loaded plugins hold now: the OP-1's own figure where it gives one, else the same sum. */
  loadedNow: { bytes: number; fromDevice: boolean; exact: boolean };
}

const sameFile = (a: string, b: string): boolean => a === b;

/**
 * The memory of each file on the OP-1 and of the set the next boot loads (every file not switched off; `add` is a
 * file about to be sent, which replaces one of the same name).
 *
 * A file is exact when co-op has its header: its name and size match a known file's, or it is `add`. A file that
 * came after the boot has no size in the inventory; under a known name it is taken as that file. Anything else is
 * estimated: from what the OP-1 says it needs (a refused file), from the OP-1's used figure when it is the only
 * loaded file co-op does not know, or from its size.
 */
export function planMemory(
  files: readonly DeviceCoopFile[],
  known: readonly KnownCoopFile[],
  options: { build?: string | null | undefined; report?: DeviceMemoryReport | null; add?: KnownCoopFile } = {},
): MemoryPlan | null {
  const report = options.report ?? reportFromInventory(files);
  const table = budgetOfCore(options.build);
  if (!table && !report) return null;
  const budget = report?.budget ?? (table as { bytes: number }).bytes;
  const budgetKnown = report !== null || (table?.known ?? false);

  const resolve = (f: DeviceCoopFile): { memory: CoopMemory; exact: boolean } | null => {
    const named = known.filter((k) => sameFile(k.file, f.file));
    const match = named.find((k) => k.sizeBytes === f.size) ?? (f.size === 0 ? named[0] : undefined);
    return match ? { memory: match.memory, exact: true } : null;
  };
  const resolved = files.map((f) => ({ f, hit: resolve(f) }));
  const lump = (bytes: number, kind: 'fx' | 'synth' | null): CoopMemory => ({ kind: kind ?? 'fx', image: bytes, scratch: 0, voices: 0, screen: 0 });
  // The one loaded file co-op does not know holds whatever the known loaded ones do not.
  const strangers = resolved.filter((r) => !r.hit && r.f.loaded);
  const knownLoaded = setBytes(resolved.filter((r) => r.hit && r.f.loaded).map((r) => (r.hit as { memory: CoopMemory }).memory));
  const rest = report && strangers.length === 1 ? report.used - knownLoaded : 0;

  let entries = resolved.map(({ f, hit }) => {
    if (hit) return { f, ...hit };
    const need = report?.needs[f.file];
    if (need) return { f, memory: lump(need, f.kind), exact: report.exact };
    if (rest > 0 && f.loaded) return { f, memory: lump(rest, f.kind), exact: report?.exact ?? false };
    return { f, memory: estimateFromSize(f.size, f.kind), exact: false };
  });
  if (options.add) {
    const add = options.add;
    entries = entries.filter((e) => !sameFile(e.f.file, add.file));
    entries.push({
      f: { file: add.file, size: add.sizeBytes, kind: add.memory.kind, loaded: false, enabled: true, reason: '' },
      memory: add.memory,
      exact: true,
    });
  }

  const next = entries.filter((e) => e.f.enabled);
  const used = setBytes(next.map((e) => e.memory));
  const rows = entries.map((e): MemoryRow => {
    const without = setBytes(next.filter((n) => n !== e).map((n) => n.memory));
    return { file: e.f.file, bytes: ownBytes(e.memory), frees: e.f.enabled ? used - without : 0, exact: e.exact, counted: e.f.enabled };
  });
  const largest = next.reduce<(typeof next)[number] | null>((top, e) => (e.memory.scratch > (top?.memory.scratch ?? 0) ? e : top), null);
  const loaded = entries.filter((e) => e.f.loaded);
  const loadedSum = setBytes(loaded.map((e) => e.memory));
  return {
    budget,
    budgetKnown,
    used,
    level: levelOf(used, budget),
    approximate: !budgetKnown || next.some((e) => !e.exact),
    shared: positionBuffers(largest?.memory.scratch ?? 0),
    sharedBy: largest?.f.file ?? null,
    rows,
    loadedNow: report
      ? { bytes: report.used, fromDevice: true, exact: report.exact }
      : { bytes: loadedSum, fromDevice: false, exact: loaded.every((e) => e.exact) },
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
