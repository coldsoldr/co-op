import type { Page } from '@playwright/test';

export interface CoopStubOptions {
  /** The build tag HELLO and the inventory give (default e10). */
  build?: string;
  /** The inventory's rows, as coopRow makes them. */
  rows: string[];
  /** /yaffs2/user/coop_stats.txt, served by FILE_GET; without it the file is not there. */
  stats?: string;
  /**
   * e12's inventory: the header's budget, used, fxbuf, fxbufs and clip fields. The rows then carry need and scratch
   * (coopRow), and a file written after the boot is listed with its real size. Without it the inventory is e3..e11's.
   */
  memory?: {
    budget: number;
    used: number;
    fxbuf?: number;
    fxbufs?: number;
    clip?: number;
    /**
     * e13's part: the two budgets and the placement policy in the header, and a `heap` column on every row. Without
     * it the inventory is e12's.
     */
    e13?: { heapBudget: number; heapUsed: number; arenaBudget: number; arenaUsed: number; place?: string; placeSrc?: string };
  };
}

/** What a test can read from the stub, as `window.coopStub` in the page. */
export interface CoopStub {
  /** The files written, by name. */
  puts: string[];
  /** What arrived of each file written: its size and zlib CRC-32, counted by the stub from the chunks it took. */
  files: Record<string, { size: number; crc: number }>;
  /** The most messages that waited in the receive queue at once. */
  deepest: number;
  /** Messages dropped because the queue of four was full. */
  drops: number;
  /** The OP-1's ports the page has open now ('in', 'out'), and how many times it opened one. */
  openPorts: string[];
  opens: number;
}

declare global {
  interface Window {
    coopStub: CoopStub;
  }
}

/**
 * One inventory row: file, kind, name, boot, next, version, author, size, reason, and with `need` e12's two columns
 * after them, need and scratch (firmware/docs/sysex.md).
 */
export function coopRow(o: {
  file: string;
  kind?: 'fx' | 'synth' | '?';
  boot?: 'loaded' | 'failed' | 'disabled' | 'new';
  next?: 'on' | 'off';
  version?: string;
  size?: number;
  reason?: string;
  need?: number;
  scratch?: number;
  /** e13's column after those: the part of `need` in heap 0. */
  heap?: number;
}): string {
  const name = o.file.replace(/\.coop$/, '').replaceAll('_', ' ');
  return [o.file, o.kind ?? 'fx', name, o.boot ?? 'loaded', o.next ?? 'on', o.version ?? '0.1', 'coldsoldr', String(o.size ?? 0), o.reason ?? '', ...(o.need === undefined ? [] : [String(o.need), String(o.scratch ?? 0)]), ...(o.heap === undefined ? [] : [String(o.heap)])].join('\t');
}

/**
 * Replaces Web MIDI in the page with a fake OP-1 that answers the coop firmware's USB channel
 * (firmware/docs/sysex.md): HELLO, INVENTORY in 64-byte pages, FILE_GET for the statistics file, and PUT_BEGIN,
 * PUT_DATA and PUT_END, after which the file is in the inventory as new. As on the core, up to four messages wait in
 * a queue and are answered in turn, a fifth is dropped, a chunk is taken only at the offset the file has reached, and
 * PUT_END checks the size and the CRC-32.
 */
export async function stubCoop(page: Page, options: CoopStubOptions): Promise<void> {
  await page.addInitScript(
    ({ build, rows, stats, memory }) => {
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

      const crc32 = (data: number[]): number => {
        let c = 0xffffffff;
        for (const b of data) {
          c ^= b;
          for (let k = 0; k < 8; k++) c = (c >>> 1) ^ (0xedb88320 & -(c & 1));
        }
        return ~c >>> 0;
      };

      const stub: CoopStub = { puts: [], files: {}, deepest: 0, drops: 0, openPorts: [], opens: 0 };
      /** open() and close() of one port, kept in stub.openPorts. */
      const portState = (id: string) => ({
        open: (): Promise<void> => {
          stub.opens++;
          if (!stub.openPorts.includes(id)) stub.openPorts.push(id);
          return Promise.resolve();
        },
        close: (): Promise<void> => {
          stub.openPorts = stub.openPorts.filter((p) => p !== id);
          return Promise.resolve();
        },
      });
      window.coopStub = stub;
      let list = rows;
      let writing: { file: string; bytes: number[] } | null = null;

      const handle = (cmd: number, p: number[]): number[] => {
        switch (cmd) {
          case 0x21:
            return [0, ...text('coop'), 1, 0, 0x3f, ...text(build)];
          case 0x28:
          {
            const more = memory
              ? `\tbudget ${memory.budget}\tused ${memory.used}\tfxbuf ${memory.fxbuf ?? 0}\tfxbufs ${memory.fxbufs ?? 0}\tclip ${memory.clip ?? 4194304}${memory.e13 ? `\theap_budget ${memory.e13.heapBudget}\theap_used ${memory.e13.heapUsed}\tarena_budget ${memory.e13.arenaBudget}\tarena_used ${memory.e13.arenaUsed}\tplace ${memory.e13.place ?? 'eng=heap,fx=heap,voice=heap,buf=arena'}\tplace_src ${memory.e13.placeSrc ?? 'default'}` : ''}`
              : '';
            return pageOf(text(`coop-inventory 1\tbuild ${build}\tprotocol 1${more}\n${list.map((r) => `${r}\n`).join('')}`), rd32(p));
          }
          case 0x20:
            return stats === null ? [3] : pageOf(text(stats), rd32(p));
          case 0x22: {
            const path = new TextDecoder().decode(Uint8Array.from(p));
            writing = { file: path.slice(path.lastIndexOf('/') + 1), bytes: [] };
            return [0];
          }
          case 0x23: {
            if (!writing) return [2];
            if (rd32(p) !== writing.bytes.length) return [5, ...le(writing.bytes.length)];
            for (const b of p.slice(4)) writing.bytes.push(b);
            return [0, ...le(writing.bytes.length)];
          }
          case 0x24: {
            if (!writing) return [2];
            const { file, bytes } = writing;
            writing = null;
            const crc = crc32(bytes);
            if (rd32(p) !== bytes.length) return [4];
            if (rd32(p.slice(4)) !== crc) return [6];
            stub.puts.push(file);
            stub.files[file] = { size: bytes.length, crc };
            list = [...list.filter((r) => !r.startsWith(`${file}\t`)), `${file}\t?\t\tnew\ton\t\t\t${memory ? `${bytes.length}\t\t0\t0` : `0\t`}`];
            return [0];
          }
          default:
            return [1];
        }
      };

      /** The messages waiting, oldest first; each keeps its place until its reply has left. */
      const queue: number[][] = [];
      const serve = (): void => {
        const bytes = queue[0];
        if (!bytes) return;
        const cmd = bytes[5] as number;
        const reply = [...HEADER, cmd | 0x40, bytes[6] as number, bytes[7] as number, ...pack7(handle(cmd, unpack7(bytes.slice(8, -1)))), 0xf7];
        input.onmidimessage?.({ data: Uint8Array.from(reply) });
        queue.shift();
        if (queue.length > 0) setTimeout(serve, 1);
      };

      const input: { name: string; state: string; onmidimessage: ((e: { data: Uint8Array }) => void) | null; open: () => Promise<void>; close: () => Promise<void> } = {
        name: 'OP-1 Midi Device',
        state: 'connected',
        onmidimessage: null,
        ...portState('in'),
      };
      const output = {
        name: 'OP-1 Midi Device',
        state: 'connected',
        ...portState('out'),
        send(data: Uint8Array) {
          const bytes = [...data];
          if (bytes.length > 96) throw new Error('a SysEx over 96 bytes stops the OP-1’s MIDI input');
          if (queue.length >= 4) {
            stub.drops++;
            return;
          }
          queue.push(bytes);
          stub.deepest = Math.max(stub.deepest, queue.length);
          if (queue.length === 1) setTimeout(serve, 1);
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
    { build: options.build ?? 'e10', rows: options.rows, stats: options.stats ?? null, memory: options.memory ?? null },
  );
}
