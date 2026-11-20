import type { Page } from '@playwright/test';

export interface CoopStubOptions {
  /** The build tag HELLO and the inventory give (default e10). */
  build?: string;
  /** The inventory's rows, as coopRow makes them. */
  rows: string[];
  /** /yaffs2/user/coop_stats.txt, served by FILE_GET; without it the file is not there. */
  stats?: string;
}

/** What a test can read from the stub, as `window.coopStub` in the page. */
export interface CoopStub {
  /** The files written, by name. */
  puts: string[];
}

declare global {
  interface Window {
    coopStub: CoopStub;
  }
}

/** One inventory row: file, kind, name, boot, next, version, author, size, reason (firmware/docs/sysex.md). */
export function coopRow(o: {
  file: string;
  kind?: 'fx' | 'synth' | '?';
  boot?: 'loaded' | 'failed' | 'disabled' | 'new';
  next?: 'on' | 'off';
  version?: string;
  size?: number;
  reason?: string;
}): string {
  const name = o.file.replace(/\.coop$/, '').replaceAll('_', ' ');
  return [o.file, o.kind ?? 'fx', name, o.boot ?? 'loaded', o.next ?? 'on', o.version ?? '0.1', 'coldsoldr', String(o.size ?? 0), o.reason ?? ''].join('\t');
}

/**
 * Replaces Web MIDI in the page with a fake OP-1 that answers the coop firmware's USB channel
 * (firmware/docs/sysex.md): HELLO, INVENTORY in 64-byte pages, FILE_GET for the statistics file, and PUT_BEGIN,
 * PUT_DATA and PUT_END, after which the file is in the inventory as new.
 */
export async function stubCoop(page: Page, options: CoopStubOptions): Promise<void> {
  await page.addInitScript(
    ({ build, rows, stats }) => {
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
      const rd32 = (b: number[]): number => ((b[0] ?? 0) | ((b[1] ?? 0) << 8) | ((b[2] ?? 0) << 16) | ((b[3] ?? 0) << 24)) >>> 0;
      const le = (v: number): number[] => [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff];
      const text = (s: string): number[] => [...new TextEncoder().encode(s)];
      const pageOf = (all: number[], offset: number): number[] => [0, ...le(all.length), ...all.slice(offset, offset + 64)];

      const stub: CoopStub = { puts: [] };
      window.coopStub = stub;
      let list = rows;
      let writing: { file: string; got: number } | null = null;

      const handle = (cmd: number, p: number[]): number[] => {
        switch (cmd) {
          case 0x21:
            return [0, ...text('coop'), 1, 0, 0x3f, ...text(build)];
          case 0x28:
            return pageOf(text(`coop-inventory 1\tbuild ${build}\tprotocol 1\n${list.map((r) => `${r}\n`).join('')}`), rd32(p));
          case 0x20:
            return stats === null ? [3] : pageOf(text(stats), rd32(p));
          case 0x22: {
            const path = new TextDecoder().decode(Uint8Array.from(p));
            writing = { file: path.slice(path.lastIndexOf('/') + 1), got: 0 };
            return [0];
          }
          case 0x23:
            if (!writing) return [2];
            writing.got += p.length - 4;
            return [0, ...le(writing.got)];
          case 0x24: {
            if (!writing) return [2];
            const { file } = writing;
            writing = null;
            stub.puts.push(file);
            list = [...list.filter((r) => !r.startsWith(`${file}\t`)), `${file}\t?\t\tnew\ton\t\t\t0\t`];
            return [0];
          }
          default:
            return [1];
        }
      };

      const input: { name: string; state: string; onmidimessage: ((e: { data: Uint8Array }) => void) | null; open: () => Promise<void>; close: () => Promise<void> } = {
        name: 'OP-1 Midi Device',
        state: 'connected',
        onmidimessage: null,
        open: () => Promise.resolve(),
        close: () => Promise.resolve(),
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
          const reply = [...HEADER, cmd | 0x40, bytes[6] as number, bytes[7] as number, ...pack7(handle(cmd, unpack7(bytes.slice(8, -1)))), 0xf7];
          setTimeout(() => input.onmidimessage?.({ data: Uint8Array.from(reply) }), 1);
        },
      };
      const access = {
        inputs: new Map([['in', input]]),
        outputs: new Map([['out', output]]),
        onstatechange: null,
        addEventListener: () => undefined,
        removeEventListener: () => undefined,
      };
      Object.defineProperty(navigator, 'requestMIDIAccess', { configurable: true, value: () => Promise.resolve(access) });
    },
    { build: options.build ?? 'e10', rows: options.rows, stats: options.stats ?? null },
  );
}
