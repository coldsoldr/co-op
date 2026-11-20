/**
 * The coop firmware's USB channel (firmware/docs/sysex.md): the same SysEx header and 7-bit framing as kennel's, its
 * own command numbers. Files go straight to the OP-1's internal disk; there is no commit and no restart command, so
 * every change to the plugins takes effect at the next power cycle. Built on the kennel session (one message in
 * flight, replies matched by command and seq, resends), so only the payloads differ. The one exception is a file's
 * PUT_DATA, which goes with a few chunks in flight (COOP_WINDOW).
 */
import { crc32, readU32le, u32le } from './framing';
import type { Inventory, InventoryKind, InventoryRecord, InventoryState } from './inventory';
import type { Reply } from './framing';
import type { Session } from './session';
import { reportFromStats, type DeviceMemoryReport } from '../plugins/memory';

/** coop's status codes (firmware/docs/sysex.md). */
const COOP_STATUS: Record<number, string> = {
  1: 'a malformed request',
  2: 'busy: the settings menu is open on the OP-1, or nothing is being written',
  3: 'a bad path or name',
  4: 'a size mismatch, or the list is full',
  5: 'an out-of-order chunk',
  6: 'a checksum mismatch',
  7: 'a write error on the OP-1',
};

/** The OP-1 answered a coop command with a status other than ok. */
export class CoopRefusedError extends Error {
  constructor(
    readonly cmd: number,
    readonly status: number,
    what: string,
  ) {
    super(`the OP-1 refused ${what}: ${COOP_STATUS[status] ?? `status ${status}`}`);
    this.name = 'CoopRefusedError';
  }
}

export const CoopCommand = {
  FILE_GET: 0x20,
  HELLO: 0x21,
  PUT_BEGIN: 0x22,
  PUT_DATA: 0x23,
  PUT_END: 0x24,
  DELETE: 0x25,
  INVENTORY: 0x28,
  PLUGIN_SET: 0x29,
} as const;

/** HELLO capability bits. */
export const CoopCaps = { FILE_READ: 1, FILE_WRITE: 2, SLOT2: 4, RUN: 8, INVENTORY: 16, PLUGIN_SET: 32 } as const;

export const COOP_PLUGIN_DIR = '/yaffs2/plugins/';
/** The bytes of one PUT_DATA; the firmware takes up to 72, the host keeps every message under 96 bytes. */
export const COOP_CHUNK = 64;
/**
 * How many messages the core queues between two passes of its main loop (`QN`, firmware/core/sdram/coop_sysex.cc);
 * one more is dropped without a reply. Every core that writes files (P1b to e11) has this queue.
 */
export const COOP_QUEUE = 4;
/**
 * The PUT_DATA chunks in flight at once. One under the queue: the slot of the chunk being answered is free only
 * after its reply has left, so the chunk that reply releases can arrive while that slot still counts.
 */
export const COOP_WINDOW = 3;

const STATUS_OK = 0;
const STATUS_OFFSET = 5;
const STATUS_IO = 7;
const encoder = new TextEncoder();
const decoder = new TextDecoder();

export interface CoopHello {
  protocol: number;
  caps: number;
  /** The package tag, such as "e3". */
  build: string;
}

/** The reply to HELLO, or null when the device is not coop (kennel answers coop's HELLO with an error, or not at all). */
export function parseCoopHello(payload: Uint8Array): CoopHello | null {
  if (payload.length < 7 || decoder.decode(payload.subarray(0, 4)) !== 'coop') return null;
  return {
    protocol: (payload[4] as number) | ((payload[5] as number) << 8),
    caps: payload[6] as number,
    build: decoder.decode(payload.subarray(7)),
  };
}

export async function coopHello(session: Session): Promise<CoopHello | null> {
  const reply = await session.ask(CoopCommand.HELLO);
  return reply.status === STATUS_OK ? parseCoopHello(reply.payload) : null;
}

/** A plugin file name the firmware accepts: `<id>.coop`, no directory. */
export const COOP_FILE = /^[a-z0-9_-]{1,15}\.coop$/;

function checkFile(file: string): void {
  if (!COOP_FILE.test(file)) throw new Error(`"${file}" is not a coop plugin file name`);
}

const KINDS: Record<string, InventoryKind> = { fx: 'fx', synth: 'synth' };
const BOOT_STATES: Record<string, InventoryState> = { loaded: 'loaded', failed: 'failed', disabled: 'off', new: 'pending' };

/**
 * The text INVENTORY serves, as an Inventory (inventory.ts): a header `coop-inventory 1<TAB>build ...`, then one line
 * per file: file, kind, name, boot, next, version, author, size, reason, and from e12 need and scratch. `name` is the
 * plugin id (the file without `.coop`), as kennel's records name plugins. An e12 header also gives the plugin memory's
 * budget and what is in use (`memory`).
 */
export function parseCoopInventory(text: string): Inventory & { build: string | null } {
  const lines = text.split('\n').filter((l) => l.length > 0);
  const header = lines.shift() ?? '';
  if (!header.startsWith('coop-inventory 1')) throw new Error('not a coop inventory');
  // Header fields are found by name; e12 appended budget, used, fxbuf, fxbufs and clip (firmware/docs/sysex.md).
  const field = (name: string): string | null => header.split('\t').find((f) => f.startsWith(`${name} `))?.slice(name.length + 1) ?? null;
  const count = (name: string): number | null => (/^\d+$/.test(field(name) ?? '') ? Number(field(name)) : null);
  const build = field('build');
  const budget = count('budget');
  const used = count('used');
  const memory =
    budget !== null && budget > 0 && used !== null
      ? { budget, used, fxbuf: count('fxbuf') ?? 0, fxbufs: count('fxbufs') ?? 0, clip: count('clip') }
      : null;
  const records = lines.map((line): InventoryRecord => {
    // e12 appended need and scratch after the reason; before it a row ends there.
    const [file = '', kind = '', , boot = '', next = '', version = '', author = '', size = '', reason = '', need = '', scratch = ''] =
      line.split('\t');
    return {
      ...(/^\d+$/.test(need) ? { need: Number(need), scratch: /^\d+$/.test(scratch) ? Number(scratch) : 0 } : {}),
      kind: KINDS[kind] ?? null,
      name: file.replace(/\.coop$/, ''),
      source: 'coop',
      path: `plugins/${file}`,
      version,
      author,
      size: /^\d+$/.test(size) ? Number(size) : 0,
      crc: 0,
      state: BOOT_STATES[boot] ?? null,
      claims: [],
      reason,
      enabled: next !== 'off',
    };
  });
  return { core: null, presets: null, records, build, ...(memory ? { memory } : {}) };
}

/** Reads the inventory page by page (64 bytes a reply). */
export async function readCoopInventory(session: Session): Promise<Inventory & { build: string | null }> {
  const parts: Uint8Array[] = [];
  let got = 0;
  let total: number;
  do {
    const reply = await session.ask(CoopCommand.INVENTORY, Uint8Array.from(u32le(got)));
    if (reply.status !== STATUS_OK) throw new CoopRefusedError(reply.cmd, reply.status, 'the plugin inventory');
    total = readU32le(reply.payload);
    const piece = reply.payload.subarray(4, 4 + Math.max(0, total - got));
    if (piece.length === 0 && got < total) throw new Error('the OP-1 sent an empty piece of the plugin inventory');
    parts.push(piece);
    got += piece.length;
  } while (got < total);
  const all = new Uint8Array(got);
  let at = 0;
  for (const p of parts) {
    all.set(p, at);
    at += p.length;
  }
  return parseCoopInventory(decoder.decode(all));
}

/** The core's statistics file: rewritten every few seconds, with the `plugmem` line (firmware/docs/plugins.md "e7"). */
export const COOP_STATS_PATH = '/yaffs2/user/coop_stats.txt';
/** The most of the statistics file one pass reads (the core writes at most 16 KiB). */
const STATS_MAX = 32 * 1024;
/** Where the plugmem line was last found, so the next look starts there instead of at the top of the file. */
let plugmemHint: number | null = null;

/** What starts at "plugmem " in `text`: the line and its plugfile lines, once the text holds them whole. */
function plugmemBlock(text: string, atEnd: boolean): { at: number; block: string } | null {
  const at = text.indexOf('\nplugmem ');
  if (at < 0) return null;
  const lines = text.slice(at + 1).split('\n');
  lines.pop(); // what follows the last newline: an unfinished line, or nothing
  const stop = lines.findIndex((line, i) => i > 0 && !line.startsWith('plugfile'));
  if (stop >= 0) return { at: at + 1, block: lines.slice(0, stop).join('\n') };
  // Every whole line so far belongs to the block, so only the end of the file closes it.
  return atEnd && lines.length > 0 ? { at: at + 1, block: lines.join('\n') } : null;
}

/** Reads the statistics file from `from` in 64-byte steps until the plugmem block is whole, or the file ends. */
async function scanForPlugmem(session: Session, from: number): Promise<{ at: number; report: DeviceMemoryReport } | null> {
  const path = encoder.encode(COOP_STATS_PATH);
  let text = '';
  for (let offset = from; offset - from < STATS_MAX; ) {
    const payload = new Uint8Array(4 + path.length);
    payload.set(u32le(offset), 0);
    payload.set(path, 4);
    const reply = await session.ask(CoopCommand.FILE_GET, payload);
    if (reply.status !== STATUS_OK) return null;
    const total = readU32le(reply.payload);
    const piece = reply.payload.subarray(4);
    text += decoder.decode(piece);
    offset += piece.length;
    const atEnd = piece.length === 0 || offset >= total;
    const found = plugmemBlock(text, atEnd);
    if (found) {
      const report = reportFromStats(found.block);
      return report ? { at: from + found.at, report } : null;
    }
    if (atEnd) return null;
  }
  return null;
}

/**
 * The OP-1's own account of its plugin memory: the budget, the bytes the loaded plugins hold and what each refused
 * file needs, from the `plugmem` line of the statistics file. Null when the core has no such file or line (before
 * e7) or the device does not serve files.
 *
 * The core rewrites the file every few seconds and a read goes 64 bytes a message, so one pass can mix two versions
 * of the file. The line is therefore read twice, the second time straight from where it was found, and only a
 * figure both passes agree on is returned.
 */
export async function readCoopMemoryReport(session: Session): Promise<DeviceMemoryReport | null> {
  const near = (at: number): number => Math.max(0, at - 128);
  let first = plugmemHint === null ? null : await scanForPlugmem(session, near(plugmemHint));
  first ??= await scanForPlugmem(session, 0);
  for (let tries = 0; first && tries < 2; tries++) {
    const again = await scanForPlugmem(session, near(first.at));
    if (again && JSON.stringify(again.report) === JSON.stringify(first.report)) {
      plugmemHint = again.at;
      return again.report;
    }
    first = again;
  }
  return null;
}

export interface PutOptions {
  /** Called with the bytes the OP-1 has confirmed so far, each time that count grows. */
  onProgress?: (written: number, total: number) => void;
  /** The chunks in flight at once: COOP_WINDOW unless given, never more than COOP_QUEUE - 1; 1 sends one at a time. */
  window?: number;
}

/** One PUT_DATA: the offset, then up to COOP_CHUNK bytes from it. */
function dataPayload(bytes: Uint8Array, offset: number): Uint8Array {
  const chunk = bytes.subarray(offset, offset + COOP_CHUNK);
  const payload = new Uint8Array(4 + chunk.length);
  payload.set(u32le(offset), 0);
  payload.set(chunk, 4);
  return payload;
}

/** The offset a PUT_DATA reply names: the next byte the OP-1 expects. */
const nextOffset = (reply: Reply): number => (reply.payload.length >= 4 ? readU32le(reply.payload) : -1);

/**
 * Sends chunks with up to `window` unanswered and returns the count the OP-1 confirmed when the run ended: all of
 * the file, or less after anything but a plain ok (a reply that did not come in time, a chunk the OP-1 did not take),
 * which the caller then sends one at a time. The OP-1 writes a chunk only at the offset it expects, so an ok that
 * names the end of a chunk confirms every byte before it, also those whose own reply was lost. A write error closes
 * the file on the OP-1 and is thrown here, where its status is known.
 */
async function putWindowed(
  session: Session,
  file: string,
  bytes: Uint8Array,
  window: number,
  progress: (written: number) => void,
): Promise<number> {
  let acked = 0;
  await session.pipeline(async (send) => {
    let flights: { end: number; reply: Promise<Reply | null> }[] = [];
    let sent = 0;
    for (;;) {
      while (flights.length < window && sent < bytes.length) {
        const end = Math.min(sent + COOP_CHUNK, bytes.length);
        flights.push({ end, reply: send(CoopCommand.PUT_DATA, dataPayload(bytes, sent)) });
        sent = end;
      }
      if (flights.length === 0) return;
      const { flight, reply } = await Promise.race(flights.map((f) => f.reply.then((r) => ({ flight: f, reply: r }))));
      if (!reply) return;
      if (reply.status === STATUS_IO) throw new CoopRefusedError(reply.cmd, reply.status, `writing ${file} near byte ${acked}`);
      if (reply.status !== STATUS_OK || nextOffset(reply) !== flight.end) return;
      acked = flight.end;
      flights = flights.filter((f) => f.end > acked);
      progress(acked);
    }
  });
  return acked;
}

/**
 * Writes a plugin file to /yaffs2/plugins: PUT_BEGIN (create, truncate), PUT_DATA in order, PUT_END with the size and
 * the zlib CRC-32, which the firmware checks.
 *
 * The chunks go COOP_WINDOW at a time (the core queues COOP_QUEUE messages and answers each in turn). When that run
 * stops short, the rest of the file goes as before, one chunk in flight with resends, from the last confirmed byte.
 * There a reply that names an offset further on, with ok or as an offset mismatch, means the OP-1 had already
 * written up to it (chunks whose replies were lost): that counts as written.
 */
export async function coopPutPlugin(session: Session, file: string, bytes: Uint8Array, options: PutOptions = {}): Promise<void> {
  checkFile(file);
  const window = Math.max(1, Math.min(Math.floor(options.window ?? COOP_WINDOW), COOP_QUEUE - 1));
  const begin = await session.ask(CoopCommand.PUT_BEGIN, encoder.encode(COOP_PLUGIN_DIR + file));
  if (begin.status !== STATUS_OK) throw new CoopRefusedError(begin.cmd, begin.status, `writing ${file}`);
  const progress = (written: number): void => options.onProgress?.(written, bytes.length);
  let offset = window > 1 ? await putWindowed(session, file, bytes, window, progress) : 0;
  while (offset < bytes.length) {
    const reply = await session.ask(CoopCommand.PUT_DATA, dataPayload(bytes, offset));
    const next = nextOffset(reply);
    // Only bytes of this file, in order, are ever on the OP-1, so its count is taken as it is; PUT_END checks them.
    const written = (reply.status === STATUS_OK || reply.status === STATUS_OFFSET) && next > offset && next <= bytes.length;
    if (!written) throw new CoopRefusedError(reply.cmd, reply.status, `writing ${file} at byte ${offset}`);
    offset = next;
    progress(offset);
  }
  const end = new Uint8Array(8);
  end.set(u32le(bytes.length), 0);
  end.set(u32le(crc32(bytes)), 4);
  const reply = await session.ask(CoopCommand.PUT_END, end);
  if (reply.status !== STATUS_OK) throw new CoopRefusedError(reply.cmd, reply.status, `finishing ${file}`);
}

/** Deletes a plugin file. Status 3 means it was not there (or could not be removed). */
export async function coopDeletePlugin(session: Session, file: string): Promise<void> {
  checkFile(file);
  const reply = await session.ask(CoopCommand.DELETE, encoder.encode(COOP_PLUGIN_DIR + file));
  if (reply.status !== STATUS_OK) throw new CoopRefusedError(reply.cmd, reply.status, `removing ${file}`);
}

/**
 * Switches a plugin on or off for the next boot. Status 2: the settings menu is open on the OP-1 (close it and try
 * again); 4: the list of switched-off files is full.
 */
export async function coopSetPlugin(session: Session, file: string, enabled: boolean): Promise<void> {
  checkFile(file);
  const name = encoder.encode(file);
  const payload = new Uint8Array(1 + name.length);
  payload[0] = enabled ? 1 : 0;
  payload.set(name, 1);
  const reply = await session.ask(CoopCommand.PLUGIN_SET, payload);
  if (reply.status !== STATUS_OK) throw new CoopRefusedError(reply.cmd, reply.status, `switching ${file} ${enabled ? 'on' : 'off'}`);
}
