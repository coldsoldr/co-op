/**
 * The device side of the kennel USB MIDI protocol, for tests: answers HELLO, LIST and OFF_GET as
 * docs/research/2026-10-29-hundekraft-usb-and-plugins.md describes, and can be told to go silent,
 * lose replies, put realtime bytes inside SysEx or hand replies over in pieces.
 */
import { HEADER, MAX_MESSAGE, pack7, u32le, unpack7 } from '../src/core/usbmidi/framing';
import type { MidiPort } from '../src/core/usbmidi/session';

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

const OK = 0;
const FORMAT = 1;
const OFFSET = 5;
const PAGE = 64;

const encoder = new TextEncoder();

export class FakeOp1 implements MidiPort {
  /** Every request received, in order. */
  readonly requests: Request[] = [];
  closed = false;
  private listener: ((bytes: Uint8Array) => void) | null = null;
  private lose: number;

  constructor(private readonly options: FakeOp1Options = {}) {
    this.lose = options.loseReplies ?? 0;
  }

  send(bytes: Uint8Array): void {
    if (bytes.length > MAX_MESSAGE) throw new Error(`fake OP-1: ${bytes.length}-byte SysEx stops its MIDI input`);
    if (HEADER.some((b, i) => bytes[i] !== b) || bytes[bytes.length - 1] !== 0xf7) throw new Error('fake OP-1: not ours');
    const request = {
      cmd: bytes[5] as number,
      seq: (bytes[6] as number) | ((bytes[7] as number) << 7),
      payload: unpack7(bytes.subarray(8, bytes.length - 1)),
    };
    this.requests.push(request);
    if (this.options.silent) return;
    const body = this.answer(request);
    if (this.lose > 0) {
      this.lose--;
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

  private answer({ cmd, payload }: Request): number[] {
    const caps = this.options.caps === undefined ? 3 : this.options.caps;
    const inventory = caps !== null && (caps & 1) !== 0;
    switch (cmd) {
      case 1:
        return [OK, ...u32le(this.options.version ?? 1), ...(caps === null ? [] : [caps])];
      case 7: {
        if (!inventory) return [FORMAT];
        const offset = (payload[0] ?? 0) | ((payload[1] ?? 0) << 8) | ((payload[2] ?? 0) << 16) | ((payload[3] ?? 0) << 24);
        return page(encoder.encode(this.options.inventory ?? ''), offset);
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
}

/** A LIST or OFF_GET reply body: status, u32 total, up to 64 bytes from the offset. */
function page(text: Uint8Array, offset: number): number[] {
  if (offset > text.length) return [OFFSET];
  return [OK, ...u32le(text.length), ...text.subarray(offset, offset + PAGE)];
}
