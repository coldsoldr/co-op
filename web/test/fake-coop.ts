/**
 * The device side of the coop firmware's USB channel, for tests (firmware/core/sdram/coop_sysex.cc,
 * firmware/docs/sysex.md): HELLO, INVENTORY in 64-byte pages, PUT_BEGIN / PUT_DATA / PUT_END with the size and CRC
 * check, DELETE, PLUGIN_SET. Files live in a map; what the boot scan found is given up front, as on the device,
 * where a file written after the boot reads as `new` until the next power cycle. A reply can be lost once
 * (`loseReplyOf`), to test the resend of a chunk the OP-1 had already written.
 *
 * Without `queue` every message is answered at once, inside `send`. With it the fake keeps the core's receive queue
 * (`QN` 4 there): a message waits its turn, is answered `serviceMs` after the one before, holds its place until its
 * reply has left, and one that finds the queue full is dropped without a reply (`drops`), as `rx_byte` does.
 */
import { HEADER, MAX_MESSAGE, crc32, pack7, unpack7 } from '../src/core/usbmidi/framing';
import type { MidiPort } from '../src/core/usbmidi/session';

export interface CoopBootRecord {
  kind: 'fx' | 'synth';
  name: string;
  state: 'loaded' | 'failed' | 'disabled';
  version?: string;
  author?: string;
  reason?: string;
  /** e12: the row's need and scratch columns (default 0). */
  need?: number;
  scratch?: number;
}

export interface FakeCoopOptions {
  build?: string;
  /**
   * e12's inventory: the header's budget, used, fxbuf, fxbufs and clip fields, and the need and scratch columns on
   * every row. Without it the inventory is as e3..e11 gave it.
   */
  memory?: { budget: number; used: number; fxbuf?: number; fxbufs?: number; clip?: number };
  caps?: number;
  /** Plugin files on the device, by file name. */
  files?: Record<string, Uint8Array>;
  /** What the boot scan made of each file. */
  boot?: Record<string, CoopBootRecord>;
  /** File names on the settings file's off list. */
  disabled?: string[];
  /** The settings menu is open on the device: PLUGIN_SET is refused. */
  menuOpen?: boolean;
  /** Answer nothing (a kennel or stock OP-1). */
  silent?: boolean;
  /**
   * /yaffs2/user/coop_stats.txt, served by FILE_GET. A function is asked on every request (with how many FILE_GETs
   * came before), as the core rewrites the file every few seconds.
   */
  stats?: string | ((request: number) => string);
  /** The depth of the receive queue; without it every message is answered at once. 1: a device with one in flight. */
  queue?: number;
  /** How long the device takes over each queued message (default 1 ms). */
  serviceMs?: number;
}

const OK = 0;
const FORMAT = 1;
const STATE = 2;
const PATH = 3;
const SIZE = 4;
const OFFSET = 5;
const CRC = 6;
const IO = 7;
const dec = new TextDecoder();
const enc = new TextEncoder();
const u32 = (b: Uint8Array, at = 0): number =>
  ((b[at] ?? 0) | ((b[at + 1] ?? 0) << 8) | ((b[at + 2] ?? 0) << 16) | ((b[at + 3] ?? 0) << 24)) >>> 0;
const le = (v: number): number[] => [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff];

export class FakeCoop implements MidiPort {
  readonly files: Map<string, Uint8Array>;
  readonly disabled: Set<string>;
  /** Every message sent; `queued` is how many the queue held once this one was in it (none: lost, dropped, or no queue). */
  readonly requests: { cmd: number; payload: Uint8Array; queued?: number }[] = [];
  menuOpen: boolean;
  /** The reply to the n-th request of this command is lost once, and those of the `count` - 1 after it. */
  loseReplyOf: { cmd: number; nth: number; count?: number } | null = null;
  /** The n-th request of this command never reaches the device, once. */
  loseRequestOf: { cmd: number; nth: number } | null = null;
  /** The n-th chunk the device writes fails, once: status 7, and the file is closed. */
  failWriteOf: number | null = null;
  /** Messages dropped because the queue was full. */
  drops = 0;
  closed = false;
  private listener: ((bytes: Uint8Array) => void) | null = null;
  private put: { path: string; bytes: number[] } | null = null;
  private inventory = '';
  private readonly queue: { cmd: number; seq: number; payload: Uint8Array; nth: number }[] = [];
  private serving = false;
  private dataWrites = 0;

  constructor(private readonly options: FakeCoopOptions = {}) {
    this.files = new Map(Object.entries(options.files ?? {}));
    this.disabled = new Set(options.disabled ?? []);
    this.menuOpen = options.menuOpen ?? false;
  }

  send(bytes: Uint8Array): void {
    if (bytes.length > MAX_MESSAGE) throw new Error(`fake coop: ${bytes.length}-byte SysEx`);
    if (HEADER.some((b, i) => bytes[i] !== b)) throw new Error('fake coop: not ours');
    const cmd = bytes[5] as number;
    const seq = (bytes[6] as number) | ((bytes[7] as number) << 7);
    const payload = unpack7(bytes.subarray(8, bytes.length - 1));
    const request: (typeof this.requests)[number] = { cmd, payload };
    this.requests.push(request);
    if (this.options.silent) return;
    const nth = this.requests.filter((r) => r.cmd === cmd).length;
    if (this.loseRequestOf && this.loseRequestOf.cmd === cmd && this.loseRequestOf.nth === nth) {
      this.loseRequestOf = null;
      return;
    }
    const message = { cmd, seq, payload, nth };
    const depth = this.options.queue;
    if (depth === undefined) {
      this.serve(message);
      return;
    }
    if (this.queue.length >= depth) {
      this.drops++;
      return;
    }
    this.queue.push(message);
    request.queued = this.queue.length;
    void this.drain();
  }

  /** The main loop's pass: every queued message in turn, each leaving the queue once it is answered. */
  private async drain(): Promise<void> {
    if (this.serving) return;
    this.serving = true;
    for (let head = this.queue[0]; head; head = this.queue[0]) {
      await new Promise((resolve) => setTimeout(resolve, this.options.serviceMs ?? 1));
      this.serve(head);
      this.queue.shift();
    }
    this.serving = false;
  }

  private serve({ cmd, seq, payload, nth }: { cmd: number; seq: number; payload: Uint8Array; nth: number }): void {
    const body = this.answer(cmd, payload);
    const lose = this.loseReplyOf;
    if (lose && lose.cmd === cmd && nth >= lose.nth) {
      if (nth >= lose.nth + (lose.count ?? 1) - 1) this.loseReplyOf = null;
      return;
    }
    const message = [...HEADER, cmd | 0x40, seq & 0x7f, seq >> 7, ...pack7(Uint8Array.from(body)), 0xf7];
    if (message.length > MAX_MESSAGE) throw new Error(`fake coop: reply of ${message.length} bytes`);
    this.listener?.(Uint8Array.from(message));
  }

  /** The bytes of the file being written that the device holds. */
  get written(): number {
    return this.put?.bytes.length ?? 0;
  }

  onSysex(cb: (bytes: Uint8Array) => void): void {
    this.listener = cb;
  }

  close(): void {
    this.closed = true;
  }

  private pathOf(payload: Uint8Array): string | null {
    const path = dec.decode(payload);
    return /^\/yaffs2\/(plugins|user)\/[^/]+$/.test(path) && !path.includes('..') ? path : null;
  }

  private inventoryText(): string {
    const boot = this.options.boot ?? {};
    const memory = this.options.memory;
    const rows = [...this.files.keys()].sort().map((file) => {
      const b = boot[file];
      const fields = [
        file,
        b?.kind ?? '?',
        b?.name ?? '',
        b?.state ?? 'new',
        this.disabled.has(file) ? 'off' : 'on',
        b?.version ?? '',
        b?.author ?? '',
        String(this.files.get(file)?.length ?? 0),
        b?.reason ?? '',
        ...(memory ? [String(b?.need ?? 0), String(b?.scratch ?? 0)] : []),
      ];
      return `${fields.join('\t')}\n`;
    });
    const more = memory
      ? `\tbudget ${memory.budget}\tused ${memory.used}\tfxbuf ${memory.fxbuf ?? 0}\tfxbufs ${memory.fxbufs ?? 0}\tclip ${memory.clip ?? 4194304}`
      : '';
    return `coop-inventory 1\tbuild ${this.options.build ?? 'e3'}\tprotocol 1${more}\n${rows.join('')}`;
  }

  private answer(cmd: number, p: Uint8Array): number[] {
    switch (cmd) {
      case 0x20: {
        if (p.length < 5) return [FORMAT];
        const stats = this.options.stats;
        if (stats === undefined || dec.decode(p.subarray(4)) !== '/yaffs2/user/coop_stats.txt') return [PATH];
        const all = enc.encode(typeof stats === 'string' ? stats : stats(this.requests.filter((r) => r.cmd === 0x20).length - 1));
        const off = u32(p);
        return [OK, ...le(all.length), ...all.subarray(off, off + 64)];
      }
      case 0x21:
        return [OK, ...enc.encode('coop'), 1, 0, this.options.caps ?? 0x3f, ...enc.encode(this.options.build ?? 'e3')];
      case 0x28: {
        if (p.length < 4) return [FORMAT];
        const off = u32(p);
        if (off === 0) this.inventory = this.inventoryText();
        const all = enc.encode(this.inventory);
        return [OK, ...le(all.length), ...all.subarray(off, off + 64)];
      }
      case 0x22: {
        const path = this.pathOf(p);
        if (!path) return [PATH];
        this.put = { path, bytes: [] };
        return [OK];
      }
      case 0x23: {
        const put = this.put;
        if (!put) return [STATE];
        if (u32(p) !== put.bytes.length) return [OFFSET, ...le(put.bytes.length)];
        if (++this.dataWrites === this.failWriteOf) {
          this.put = null;
          return [IO, ...le(put.bytes.length)];
        }
        put.bytes.push(...p.subarray(4));
        return [OK, ...le(put.bytes.length)];
      }
      case 0x24: {
        const put = this.put;
        if (!put) return [STATE];
        this.put = null;
        const data = Uint8Array.from(put.bytes);
        if (u32(p) !== data.length) return [SIZE];
        if (u32(p, 4) !== crc32(data)) return [CRC];
        this.files.set(put.path.slice(put.path.lastIndexOf('/') + 1), data);
        return [OK];
      }
      case 0x25: {
        const path = this.pathOf(p);
        if (!path) return [PATH];
        return this.files.delete(path.slice(path.lastIndexOf('/') + 1)) ? [OK] : [PATH];
      }
      case 0x29: {
        const name = dec.decode(p.subarray(1));
        if (p.length < 2 || name.includes('/')) return [PATH];
        if (this.menuOpen) return [STATE];
        if (p[0]) this.disabled.delete(name);
        else this.disabled.add(name);
        return [OK, p[0] ? 1 : 0];
      }
      default:
        return [FORMAT];
    }
  }
}
