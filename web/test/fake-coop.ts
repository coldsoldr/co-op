/**
 * The device side of the coop firmware's USB channel, for tests (firmware/core/sdram/coop_sysex.cc,
 * firmware/docs/sysex.md): HELLO, INVENTORY in 64-byte pages, PUT_BEGIN / PUT_DATA / PUT_END with the size and CRC
 * check, DELETE, PLUGIN_SET. Files live in a map; what the boot scan found is given up front, as on the device,
 * where a file written after the boot reads as `new` until the next power cycle. A reply can be lost once
 * (`loseReplyOf`), to test the resend of a chunk the OP-1 had already written.
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
}

export interface FakeCoopOptions {
  build?: string;
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
}

const OK = 0;
const FORMAT = 1;
const STATE = 2;
const PATH = 3;
const SIZE = 4;
const OFFSET = 5;
const CRC = 6;
const dec = new TextDecoder();
const enc = new TextEncoder();
const u32 = (b: Uint8Array, at = 0): number =>
  ((b[at] ?? 0) | ((b[at + 1] ?? 0) << 8) | ((b[at + 2] ?? 0) << 16) | ((b[at + 3] ?? 0) << 24)) >>> 0;
const le = (v: number): number[] => [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff];

export class FakeCoop implements MidiPort {
  readonly files: Map<string, Uint8Array>;
  readonly disabled: Set<string>;
  readonly requests: { cmd: number; payload: Uint8Array }[] = [];
  menuOpen: boolean;
  /** The reply to the n-th request of this command is lost once. */
  loseReplyOf: { cmd: number; nth: number } | null = null;
  closed = false;
  private listener: ((bytes: Uint8Array) => void) | null = null;
  private put: { path: string; bytes: number[] } | null = null;
  private inventory = '';

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
    this.requests.push({ cmd, payload });
    if (this.options.silent) return;
    const body = this.answer(cmd, payload);
    const nth = this.requests.filter((r) => r.cmd === cmd).length;
    if (this.loseReplyOf && this.loseReplyOf.cmd === cmd && this.loseReplyOf.nth === nth) {
      this.loseReplyOf = null;
      return;
    }
    const message = [...HEADER, cmd | 0x40, seq & 0x7f, seq >> 7, ...pack7(Uint8Array.from(body)), 0xf7];
    if (message.length > MAX_MESSAGE) throw new Error(`fake coop: reply of ${message.length} bytes`);
    this.listener?.(Uint8Array.from(message));
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
      ];
      return `${fields.join('\t')}\n`;
    });
    return `coop-inventory 1\tbuild ${this.options.build ?? 'e3'}\tprotocol 1\n${rows.join('')}`;
  }

  private answer(cmd: number, p: Uint8Array): number[] {
    switch (cmd) {
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
