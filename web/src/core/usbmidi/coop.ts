/**
 * The coop firmware's USB channel (firmware/docs/sysex.md): the same SysEx header and 7-bit framing as kennel's, its
 * own command numbers. Files go straight to the OP-1's internal disk; there is no commit and no restart command, so
 * every change to the plugins takes effect at the next power cycle. Built on the kennel session (one message in
 * flight, replies matched by command and seq, resends), so only the payloads differ.
 */
import { crc32, readU32le, u32le } from './framing';
import type { Inventory, InventoryKind, InventoryRecord, InventoryState } from './inventory';
import type { Session } from './session';

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

const STATUS_OK = 0;
const STATUS_OFFSET = 5;
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
 * per file: file, kind, name, boot, next, version, author, size, reason. `name` is the plugin id (the file without
 * `.coop`), as kennel's records name plugins.
 */
export function parseCoopInventory(text: string): Inventory & { build: string | null } {
  const lines = text.split('\n').filter((l) => l.length > 0);
  const header = lines.shift() ?? '';
  if (!header.startsWith('coop-inventory 1')) throw new Error('not a coop inventory');
  const build = header.split('\t').find((f) => f.startsWith('build '))?.slice(6) ?? null;
  const records = lines.map((line): InventoryRecord => {
    const [file = '', kind = '', , boot = '', next = '', version = '', author = '', size = '', reason = ''] = line.split('\t');
    return {
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
  return { core: null, presets: null, records, build };
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

export interface PutOptions {
  /** Called after every chunk with the bytes written so far. */
  onProgress?: (written: number, total: number) => void;
}

/**
 * Writes a plugin file to /yaffs2/plugins: PUT_BEGIN (create, truncate), PUT_DATA in order, PUT_END with the size and
 * the zlib CRC-32, which the firmware checks. A resent chunk the OP-1 had already written comes back as an offset
 * mismatch that names the next offset; that counts as written.
 */
export async function coopPutPlugin(session: Session, file: string, bytes: Uint8Array, options: PutOptions = {}): Promise<void> {
  checkFile(file);
  const begin = await session.ask(CoopCommand.PUT_BEGIN, encoder.encode(COOP_PLUGIN_DIR + file));
  if (begin.status !== STATUS_OK) throw new CoopRefusedError(begin.cmd, begin.status, `writing ${file}`);
  let offset = 0;
  while (offset < bytes.length) {
    const chunk = bytes.subarray(offset, offset + COOP_CHUNK);
    const payload = new Uint8Array(4 + chunk.length);
    payload.set(u32le(offset), 0);
    payload.set(chunk, 4);
    const reply = await session.ask(CoopCommand.PUT_DATA, payload);
    const next = reply.payload.length >= 4 ? readU32le(reply.payload) : -1;
    const done = offset + chunk.length;
    if (reply.status === STATUS_OK || (reply.status === STATUS_OFFSET && next === done)) {
      offset = done;
      options.onProgress?.(offset, bytes.length);
      continue;
    }
    throw new CoopRefusedError(reply.cmd, reply.status, `writing ${file} at byte ${offset}`);
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
