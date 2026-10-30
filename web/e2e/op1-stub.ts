import type { Page } from '@playwright/test';

export interface StubOptions {
  /** False for firmware that shows the port but never answers HELLO. */
  answer?: boolean;
  /** Drop the OP-1's ports when it is asked for the inventory. */
  disconnectOnList?: boolean;
  /** Show no OP-1 port at all, as in disk mode. */
  noPorts?: boolean;
  /** The LIST text before the restart, and after it (default: unchanged). */
  inventory: string;
  inventoryAfterRestart?: string;
  /** The LIST text after a RESTART that removed a plugin (default: unchanged). */
  inventoryAfterRemove?: string;
  /** The stored switched-off list. */
  off?: string;
  /** Hold the reply to the DATA message at this file offset until `op1Stub.release()`. */
  holdDataAt?: number;
}

/** What a test can do to and read from the stub, as `window.op1Stub` in the page. */
export interface Op1Stub {
  /** A key on the COMMIT prompt. */
  press(key: 'T1' | 'T2'): void;
  /** Lets a held DATA reply go. */
  release(): void;
  /** The commands received, by number. */
  commands: number[];
  /** Plugin paths removed after T1. */
  removed: string[];
  /** Files installed after T1, with their bytes. */
  installed: { path: string; bytes: number[] }[];
  /** The COMMIT prompt is up. */
  prompting: boolean;
}

declare global {
  interface Window {
    op1Stub: Op1Stub;
  }
}

/**
 * Replaces Web MIDI in the page with a fake OP-1 that answers the kennel protocol the way
 * hundekraft 3.x does (see docs/research/2026-10-29-hundekraft-usb-and-plugins.md): HELLO, LIST,
 * OFF_GET, and ABORT, BEGIN, DATA, END and COMMIT with a T1/T2 prompt. After T1 it restarts: its
 * ports go away and come back, and LIST then answers `inventoryAfterRestart`.
 */
export async function stubOp1(page: Page, options: StubOptions): Promise<void> {
  await page.addInitScript(
    ({ answer, disconnectOnList, noPorts, inventory, inventoryAfterRestart, inventoryAfterRemove, off, holdDataAt }) => {
      const HEADER = [0xf0, 0x00, 0x20, 0x76, 0x7d];
      const pack7 = (raw: number[]): number[] => {
        const out: number[] = [];
        for (let i = 0; i < raw.length; i += 7) {
          const group = raw.slice(i, i + 7);
          out.push(group.reduce((high, b, k) => high | ((b >> 7) << k), 0), ...group.map((b) => b & 0x7f));
        }
        return out;
      };
      const unpack7 = (packed: number[]): number[] => {
        const out: number[] = [];
        for (let i = 0; i < packed.length; ) {
          const high = packed[i++] as number;
          for (let k = 0; k < 7 && i < packed.length; k++) out.push((packed[i++] as number) | (((high >> k) & 1) << 7));
        }
        return out;
      };
      const u32 = (v: number): number[] => [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff];
      const rd32 = (b: number[], at = 0): number =>
        ((b[at] ?? 0) | ((b[at + 1] ?? 0) << 8) | ((b[at + 2] ?? 0) << 16) | ((b[at + 3] ?? 0) << 24)) >>> 0;
      const crc32 = (bytes: number[]): number => {
        let crc = 0xffffffff;
        for (const byte of bytes) {
          crc ^= byte;
          for (let k = 0; k < 8; k++) crc = crc & 1 ? 0xedb88320 ^ (crc >>> 1) : crc >>> 1;
        }
        return (crc ^ 0xffffffff) >>> 0;
      };
      const pageOf = (text: number[], offset: number): number[] =>
        offset > text.length ? [5] : [0, ...u32(text.length), ...text.slice(offset, offset + 64)];
      const encode = (text: string): number[] => [...new TextEncoder().encode(text)];
      let listText = encode(inventory);
      const offText = encode(off);

      // The device's session: the open file, what was staged, the prompt.
      let open: { path: string; size: number; crc: number; bytes: number[] } | null = null;
      let staged: { path: string; bytes: number[] }[] = [];
      let confirmed = false;
      let deletes: string[] = [];
      let held: (() => void) | null = null;

      const stub: Op1Stub = {
        commands: [],
        removed: [],
        installed: [],
        prompting: false,
        press(key) {
          // After T1 the prompt takes no more keys.
          if (!stub.prompting || confirmed) return;
          if (key === 'T1') confirmed = true;
          else {
            open = null;
            staged = [];
            stub.prompting = false;
          }
        },
        release() {
          const go = held;
          held = null;
          go?.();
        },
      };
      window.op1Stub = stub;

      const access = new EventTarget() as EventTarget & { inputs: Map<string, unknown>; outputs: Map<string, unknown> };
      const input = { name: 'OP-1 Midi Device', state: 'connected', onmidimessage: null as null | ((e: unknown) => void), open: () => Promise.resolve(), close: () => Promise.resolve() };
      const setState = (state: string): void => {
        input.state = state;
        output.state = state;
        access.dispatchEvent(new Event('statechange'));
      };
      /** After T1: the reply goes out, the ports go away, and the OP-1 comes back with its new inventory. */
      const restart = (after: string | null = inventoryAfterRestart): void => {
        setTimeout(() => setState('disconnected'), 50);
        setTimeout(() => {
          if (after !== null) listText = encode(after);
          setState('connected');
        }, 600);
      };

      const handle = (cmd: number, payload: number[]): number[] => {
        switch (cmd) {
          case 1:
            return [0, ...u32(1), 3];
          case 2: {
            if (open) return [2];
            const path = new TextDecoder().decode(Uint8Array.from(payload.slice(8, -1)));
            if (!/^plugins\/[^/.][^/]*\.(knl|op1p)$/.test(path)) return [3];
            if (staged.some((f) => f.path === path)) return [2];
            open = { path, size: rd32(payload), crc: rd32(payload, 4), bytes: [] };
            return [0];
          }
          case 3: {
            if (!open) return [2];
            if (payload.length > 76) return [1];
            if (rd32(payload) !== open.bytes.length) return [5, ...u32(open.bytes.length)];
            open.bytes.push(...payload.slice(4));
            return [0, ...u32(open.bytes.length)];
          }
          case 4: {
            if (!open) return [2];
            const file = open;
            open = null;
            if (file.bytes.length !== file.size) return [4];
            if (crc32(file.bytes) !== file.crc) return [6];
            staged.push({ path: file.path, bytes: file.bytes });
            return [0];
          }
          case 5:
            if (open || staged.length === 0) return [2];
            if (confirmed) {
              stub.installed = staged;
              staged = [];
              stub.prompting = false;
              confirmed = false;
              restart();
              return [0];
            }
            stub.prompting = true;
            return [9];
          case 6:
            open = null;
            staged = [];
            return [0];
          case 10:
            deletes.push(new TextDecoder().decode(Uint8Array.from(payload.slice(0, -1))));
            return [0];
          case 11:
            if (confirmed) {
              stub.removed = deletes;
              deletes = [];
              stub.prompting = false;
              confirmed = false;
              restart(inventoryAfterRemove);
              return [0];
            }
            stub.prompting = true;
            return [9];
          case 7:
            return pageOf(listText, rd32(payload));
          case 8:
            return pageOf(offText, (payload[1] ?? 0) | ((payload[2] ?? 0) << 8));
          default:
            return [1];
        }
      };

      const output = {
        name: 'OP-1 Midi Device',
        state: 'connected',
        open: () => Promise.resolve(),
        close: () => Promise.resolve(),
        send(data: Uint8Array) {
          const bytes = [...data];
          if (bytes.length > 96) throw new Error('a SysEx over 96 bytes stops the OP-1’s MIDI input');
          const cmd = bytes[5] as number;
          const payload = unpack7(bytes.slice(8, -1));
          stub.commands.push(cmd);
          if (output.state === 'disconnected') return;
          if (cmd === 7 && disconnectOnList) {
            setState('disconnected');
            return;
          }
          if (!answer) return;
          const body = handle(cmd, payload);
          const reply = [...HEADER, cmd | 0x40, bytes[6] as number, bytes[7] as number, ...pack7(body), 0xf7];
          // A clock byte inside the SysEx, as a real OP-1 may send.
          reply.splice(3, 0, 0xf8);
          const deliver = (): void => {
            setTimeout(() => input.onmidimessage?.({ data: Uint8Array.from(reply) }), 1);
          };
          if (cmd === 3 && holdDataAt !== null && rd32(payload) === holdDataAt) held = deliver;
          else deliver();
        },
      };
      access.inputs = new Map(noPorts ? [] : [['in', input]]);
      access.outputs = new Map(noPorts ? [] : [['out', output]]);
      Object.defineProperty(navigator, 'requestMIDIAccess', {
        configurable: true,
        value: () => Promise.resolve(access),
      });
    },
    {
      answer: options.answer ?? true,
      disconnectOnList: options.disconnectOnList ?? false,
      noPorts: options.noPorts ?? false,
      inventory: options.inventory,
      inventoryAfterRestart: options.inventoryAfterRestart ?? null,
inventoryAfterRemove: options.inventoryAfterRemove ?? null,
      off: options.off ?? '',
      holdDataAt: options.holdDataAt ?? null,
    },
  );
}
