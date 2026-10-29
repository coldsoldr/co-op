/**
 * The device side of the kennel USB MIDI protocol, for tests: answers HELLO, LIST and OFF_GET, and
 * stages plugin files through ABORT, BEGIN, DATA, END and COMMIT, as
 * docs/research/2026-10-29-hundekraft-usb-and-plugins.md describes. It can be told to go silent,
 * lose or drop single messages, damage a byte, put realtime bytes inside SysEx or hand replies over
 * in pieces; a test presses T1 or T2 on its COMMIT prompt.
 */
import { HEADER, MAX_DATA, MAX_MESSAGE, crc32, pack7, u32le, unpack7 } from '../src/core/usbmidi/framing';
import type { MidiPort } from '../src/core/usbmidi/session';

/** What happens to one request: `drop` never reaches the OP-1, `lose` is handled but its reply is lost. */
export type Fate = 'drop' | 'lose' | undefined;

export interface FakeOp1Options {
  version?: number;
  /** The HELLO capabilities byte; null for a build that sends only the version. Default 3. */
  caps?: number | null;
  inventory?: string;
  /** The stored switched-off list (OFF_GET 0) and the one in effect since boot (OFF_GET 1). */
  off?: string;
  offAtBoot?: string;
  /** Never answers anything: stock firmware, op1r, hundekraft 2.7 and older. */
  silent?: boolean;
  /** Drops this many replies, from the first. */
  loseReplies?: number;
  /** Decides the fate of each request; called with the request and how many came before it. */
  fate?: (request: Request, index: number) => Fate;
  /** Flips the low bit of the file byte at this offset as it is written, like a damaged transfer. */
  damageAt?: number;
  /** Presses this key on the COMMIT prompt once it has answered "wait key" `afterPolls` times (default 1). */
  press?: 'T1' | 'T2' | undefined;
  afterPolls?: number;
  /** Puts MIDI clock bytes before, inside and after each reply. */
  realtime?: boolean;
  /** Hands each reply over in runs of this many bytes. */
  splitInto?: number;
  /** Delay before each reply, in ms. */
  replyDelayMs?: number;
}

export interface Request {
  cmd: number;
  seq: number;
  payload: Uint8Array;
}

/** A file the OP-1 received whole: staged until COMMIT, installed after T1. */
export interface StagedFile {
  path: string;
  bytes: Uint8Array;
}

const OK = 0;
const FORMAT = 1;
const STATE = 2;
const PATH = 3;
const SIZE = 4;
const OFFSET = 5;
const CRC = 6;
const WAIT_KEY = 9;
const PAGE = 64;
/** The device takes up to 127 boot blocks of 128 KiB in one file. */
const MAX_FILE = 127 * 0x20000;
const CAP_PLUGINS = 2;

/** The commands that change something on the OP-1 (everything but HELLO, LIST and OFF_GET). */
export const WRITE_COMMANDS: readonly number[] = [2, 3, 4, 5, 6, 9, 10, 11];

const encoder = new TextEncoder();
const decoder = new TextDecoder();
const readU32 = (b: Uint8Array, at = 0): number =>
  ((b[at] ?? 0) | ((b[at + 1] ?? 0) << 8) | ((b[at + 2] ?? 0) << 16) | ((b[at + 3] ?? 0) << 24)) >>> 0;

/** The device's allow-list, written from the rules in the research notes. */
function pathAllowed(path: string, plugins: boolean): boolean {
  if (path.length === 0 || path.length >= 96 || !/^[A-Za-z0-9_./-]+$/.test(path)) return false;
  if (path.startsWith('/') || path.endsWith('/') || path.includes('..') || path.includes('//') || /te-boot/i.test(path)) return false;
  if (path === 'OP1_vdk.ldr' || path.startsWith('content/')) return true;
  return plugins && /^plugins\/[^/.][^/]*\.(knl|op1p)$/.test(path);
}

export class FakeOp1 implements MidiPort {
  /** Every request received, in order (dropped ones too). */
  readonly requests: Request[] = [];
  closed = false;
  /** Files received whole since the last ABORT. */
  staged: StagedFile[] = [];
  /** The files COMMIT installed after T1; null until then. */
  installed: StagedFile[] | null = null;
  /** The COMMIT prompt is on the OP-1's screen. */
  prompting = false;
  /** Decides the fate of each request; starts as the option, and a test may change it. */
  fate: FakeOp1Options['fate'];
  private listener: ((bytes: Uint8Array) => void) | null = null;
  private lose: number;
  /** The file being received. */
  private open: { path: string; size: number; crc: number; bytes: number[] } | null = null;
  private confirmed = false;
  private polls = 0;
  /** After T1 the OP-1 restarts and answers nothing more. */
  private restarted = false;

  constructor(private readonly options: FakeOp1Options = {}) {
    this.lose = options.loseReplies ?? 0;
    this.fate = options.fate;
  }

  /** The write commands received. */
  get writes(): Request[] {
    return this.requests.filter((r) => WRITE_COMMANDS.includes(r.cmd));
  }

  send(bytes: Uint8Array): void {
    if (bytes.length > MAX_MESSAGE) throw new Error(`fake OP-1: ${bytes.length}-byte SysEx stops its MIDI input`);
    if (HEADER.some((b, i) => bytes[i] !== b) || bytes[bytes.length - 1] !== 0xf7) throw new Error('fake OP-1: not ours');
    const request = {
      cmd: bytes[5] as number,
      seq: (bytes[6] as number) | ((bytes[7] as number) << 7),
      payload: unpack7(bytes.subarray(8, bytes.length - 1)),
    };
    const fate = this.fate?.(request, this.requests.length);
    this.requests.push(request);
    if (this.options.silent || this.restarted || fate === 'drop') return;
    const body = this.answer(request);
    if (fate === 'lose' || this.lose > 0) {
      if (fate !== 'lose') this.lose--;
      return;
    }
    this.reply(request.cmd, request.seq, body);
  }

  onSysex(cb: (bytes: Uint8Array) => void): void {
    this.listener = cb;
  }

  close(): void {
    this.closed = true;
  }

  /** A key on the COMMIT prompt: T1 installs at the next COMMIT, T2 drops what was staged. */
  pressKey(key: 'T1' | 'T2'): void {
    if (!this.prompting) return;
    if (key === 'T1') {
      this.confirmed = true;
    } else {
      this.abort();
      this.prompting = false;
    }
  }

  /** Delivers raw bytes to the host, as if the OP-1 had sent them. */
  inject(bytes: Uint8Array | number[]): void {
    this.listener?.(Uint8Array.from(bytes));
  }

  /** Sends a reply with any cmd, seq and body (status first). */
  reply(cmd: number, seq: number, body: number[]): void {
    let message = [...HEADER, cmd | 0x40, seq & 0x7f, seq >> 7, ...pack7(Uint8Array.from(body)), 0xf7];
    if (message.length > MAX_MESSAGE) throw new Error(`fake OP-1: reply of ${message.length} bytes`);
    if (this.options.realtime) message = [0xf8, ...message.slice(0, 7), 0xf8, 0xfe, ...message.slice(7), 0xf8];
    const size = this.options.splitInto ?? message.length;
    const runs: Uint8Array[] = [];
    for (let i = 0; i < message.length; i += size) runs.push(Uint8Array.from(message.slice(i, i + size)));
    setTimeout(() => {
      for (const run of runs) this.listener?.(run);
    }, this.options.replyDelayMs ?? 0);
  }

  private abort(): void {
    this.open = null;
    this.staged = [];
  }

  private answer({ cmd, payload }: Request): number[] {
    const caps = this.options.caps === undefined ? 3 : this.options.caps;
    const inventory = caps !== null && (caps & 1) !== 0;
    switch (cmd) {
      case 1:
        return [OK, ...u32le(this.options.version ?? 1), ...(caps === null ? [] : [caps])];
      case 2:
        return this.begin(payload, caps !== null && (caps & CAP_PLUGINS) !== 0);
      case 3:
        return this.data(payload);
      case 4:
        return this.end();
      case 5:
        return this.commit();
      case 6:
        this.abort();
        return [OK];
      case 7: {
        if (!inventory) return [FORMAT];
        return page(encoder.encode(this.options.inventory ?? ''), readU32(payload));
      }
      case 8: {
        if (!inventory) return [FORMAT];
        const text = payload[0] === 1 ? this.options.offAtBoot : this.options.off;
        return page(encoder.encode(text ?? ''), (payload[1] ?? 0) | ((payload[2] ?? 0) << 8));
      }
      default:
        return [FORMAT];
    }
  }

  /** u32 size, u32 CRC-32, path, NUL. */
  private begin(payload: Uint8Array, plugins: boolean): number[] {
    if (this.open) return [STATE];
    if (payload.length < 10 || payload[payload.length - 1] !== 0) return [FORMAT];
    const path = decoder.decode(payload.subarray(8, payload.length - 1));
    if (!pathAllowed(path, plugins)) return [PATH];
    const size = readU32(payload);
    if (size > MAX_FILE) return [SIZE];
    // The same path twice in one session is refused.
    if (this.staged.some((f) => f.path === path)) return [STATE];
    this.open = { path, size, crc: readU32(payload, 4), bytes: [] };
    return [OK];
  }

  /** u32 offset and the bytes from there; the reply carries the bytes written so far. */
  private data(payload: Uint8Array): number[] {
    const file = this.open;
    if (!file) return [STATE];
    if (payload.length < 4 || payload.length > 4 + MAX_DATA) return [FORMAT];
    const offset = readU32(payload);
    const written = file.bytes.length;
    if (offset !== written) return [OFFSET, ...u32le(written)];
    const piece = payload.subarray(4);
    if (written + piece.length > file.size) return [SIZE, ...u32le(written)];
    piece.forEach((b, i) => file.bytes.push(written + i === this.options.damageAt ? b ^ 1 : b));
    return [OK, ...u32le(file.bytes.length)];
  }

  /** Closes the file; it is staged only when its size and CRC-32 match what BEGIN said. */
  private end(): number[] {
    const file = this.open;
    if (!file) return [STATE];
    this.open = null;
    const bytes = Uint8Array.from(file.bytes);
    if (bytes.length !== file.size) return [SIZE];
    if (crc32(bytes) !== file.crc) return [CRC];
    this.staged.push({ path: file.path, bytes });
    return [OK];
  }

  /** "wait key" until T1 (then ok, and the OP-1 restarts) or T2 (then "state": nothing staged). */
  private commit(): number[] {
    if (this.open || this.staged.length === 0) return [STATE];
    if (this.confirmed) {
      this.installed = this.staged;
      this.staged = [];
      this.prompting = false;
      this.restarted = true;
      return [OK];
    }
    this.prompting = true;
    this.polls++;
    const { press, afterPolls = 1 } = this.options;
    if (press && this.polls >= afterPolls) {
      const key = press;
      // Pressed once this reply is out, so the next COMMIT sees it.
      queueMicrotask(() => this.pressKey(key));
    }
    return [WAIT_KEY];
  }
}

/** A LIST or OFF_GET reply body: status, u32 total, up to 64 bytes from the offset. */
function page(text: Uint8Array, offset: number): number[] {
  if (offset > text.length) return [OFFSET];
  return [OK, ...u32le(text.length), ...text.subarray(offset, offset + PAGE)];
}
