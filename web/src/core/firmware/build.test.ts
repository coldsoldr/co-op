import { createHash } from 'node:crypto';
import { existsSync, readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { describe, expect, it } from 'vitest';
import { buildCoopPackage } from './build';
import { readDelta } from './delta';
import { lzmaFromSource } from './lzma';
import { APP_LDR, BOOT_LDR, checkLdr, packageProblems, readTar, unwrapCrc, wrapCrc, writeTar, type TarEntry } from './package';
import { COOP_RELEASE } from './release';

const require = createRequire(import.meta.url);
const lzma = lzmaFromSource(readFileSync(require.resolve('lzma/src/lzma_worker.js'), 'utf8'));
const sha = (b: Uint8Array): string => createHash('sha256').update(b).digest('hex');
const text = (s: string): Uint8Array => new TextEncoder().encode(s);

/** One FIRST block holding `payload`, its header XOR-ing to zero: a minimal valid LDR stream. */
function ldr(payload: Uint8Array): Uint8Array {
  const out = new Uint8Array(16 + payload.length);
  const view = new DataView(out.buffer);
  view.setUint32(0, (0xad << 24) | 0x4000 | 0x1, true);
  view.setUint32(4, 0xffa00000, true);
  view.setUint32(8, payload.length, true);
  view.setUint32(12, payload.length, true);
  let xor = 0;
  for (let i = 0; i < 16; i++) xor ^= out[i] as number;
  out[2] = (out[2] as number) ^ xor;
  out.set(payload, 16);
  return out;
}

/** A tar entry with a GNU header, as TE's packages have. */
function entry(name: string, data: Uint8Array | null): TarEntry {
  const header = new Uint8Array(512);
  const put = (at: number, s: string): void => header.set(text(s), at);
  put(0, name);
  put(100, data ? '0100666\0' : '0040666\0');
  put(108, '0000000\0');
  put(116, '0000000\0');
  put(136, '14334044173\0');
  header[156] = data ? 0x30 : 0x35;
  put(257, 'ustar  \0');
  const tar = writeTar([{ name, type: data ? '0' : '5', header, data: new Uint8Array(0) }], new Map([[name, data ?? new Uint8Array(0)]]), new Set());
  return readTar(tar)[0] as TarEntry;
}

/** The delta format of firmware/tools/make_web_delta.py for `ops`. */
function deltaOf(stock: Uint8Array, coop: Uint8Array, ops: ({ from: number; length: number } | Uint8Array)[]): Uint8Array {
  const parts: number[] = [...text('COOPDLT1'), ...Buffer.from(sha(stock), 'hex'), ...Buffer.from(sha(coop), 'hex')];
  const u32 = (n: number): number[] => [n & 0xff, (n >>> 8) & 0xff, (n >>> 16) & 0xff, n >>> 24];
  parts.push(...u32(coop.length), ...u32(ops.length));
  for (const op of ops) parts.push(...(op instanceof Uint8Array ? [1, ...u32(op.length), ...op] : [0, ...u32(op.from), ...u32(op.length)]));
  return Uint8Array.from(parts);
}

async function fixture() {
  const stockLdr = ldr(text('stock code '.repeat(40)));
  const coopLdr = ldr(text(`${'stock code '.repeat(20)}COOP CODE ${'stock code '.repeat(20)}`));
  // coop's LDR header (its payload is longer), then the first half copied, coop's bytes, the rest copied.
  const fixed = deltaOf(stockLdr, coopLdr, [coopLdr.subarray(0, 16), { from: 16, length: 220 }, text('COOP CODE '), { from: 16 + 220, length: 220 }]);
  const entries = [
    entry('content', null),
    entry('content/audio.raw', text('a sound')),
    entry(APP_LDR, stockLdr),
    entry(BOOT_LDR, text('the bootloader')),
  ];
  const stockFile = wrapCrc(await lzma.compress(writeTar(entries, new Map(), new Set())));
  const release = {
    ...COOP_RELEASE,
    deltaSha256: sha(fixed),
    stockLdrSha256: sha(stockLdr),
    coopLdrSha256: sha(coopLdr),
  } as unknown as typeof COOP_RELEASE;
  return { stockFile, delta: fixed, release, coopLdr };
}

describe('the coop package build', () => {
  it('rebuilds OP1_vdk.ldr, keeps the stock content, drops te-boot.ldr and passes its own checks', async () => {
    const { stockFile, delta, release, coopLdr } = await fixture();
    const steps: string[] = [];
    const built = await buildCoopPackage(stockFile, { lzma, delta, release, onProgress: (p) => steps.push(p.step) });
    expect([...new Set(steps)]).toEqual(['reading', 'checking', 'patching', 'packing', 'verifying']);
    const tar = await lzma.decompress(unwrapCrc(built.bytes));
    const names = readTar(tar).map((e) => e.name);
    expect(names).toEqual(['content', 'content/audio.raw', APP_LDR]);
    expect(readTar(tar).find((e) => e.name === APP_LDR)?.data).toEqual(coopLdr);
    expect(packageProblems(built.bytes, tar, built.fileName)).toEqual([]);
    expect(built.stockFiles).toBe(1);
    // TE's header layout: lc 3, lp 0, pb 2, 8 MiB dictionary, length unknown.
    expect(built.bytes[4]).toBe((2 * 5 + 0) * 9 + 3);
    expect(new DataView(built.bytes.buffer).getUint32(5, true)).toBe(8 * 1024 * 1024);
    expect([...built.bytes.subarray(9, 17)].every((b) => b === 0xff)).toBe(true);
  });

  it('refuses a file that is not R246 stock, a damaged file, and a wrong delta', async () => {
    const { stockFile, delta, release } = await fixture();
    await expect(buildCoopPackage(stockFile, { lzma, delta, release: { ...release, stockLdrSha256: 'ab'.repeat(32) } as typeof release })).rejects.toThrow(
      /not the stock R246 firmware/,
    );
    const damaged = stockFile.slice();
    damaged[40] = (damaged[40] as number) ^ 1;
    await expect(buildCoopPackage(damaged, { lzma, delta, release })).rejects.toThrow(/checksum is wrong/);
    await expect(buildCoopPackage(stockFile, { lzma, delta: delta.slice(0, -1), release })).rejects.toThrow(/delta is damaged/);
  });

  it('flags what verify_bootloader.py flags', async () => {
    const { stockFile } = await fixture();
    const tar = await lzma.decompress(unwrapCrc(stockFile));
    expect(packageProblems(stockFile, tar, 'op1_246.op1')).toEqual(['The package has te-boot.ldr; a coop package must never touch the bootloader.']);
    expect(packageProblems(stockFile, tar, 'op1_FRC.op1').join(' ')).toMatch(/FRC/);
    expect(() => checkLdr(text('not an ldr at all'))).toThrow(/signature/);
  });

  it('reads the shipped delta: it rebuilds the release it names', () => {
    const shipped = new Uint8Array(readFileSync(new URL('../../../public/firmware/coop-e10.delta', import.meta.url)));
    expect(sha(shipped)).toBe(COOP_RELEASE.deltaSha256);
    const delta = readDelta(shipped);
    expect(delta.stockSha256).toBe(COOP_RELEASE.stockLdrSha256);
    expect(delta.coopSha256).toBe(COOP_RELEASE.coopLdrSha256);
  });
});

// The real thing, when the owner points COOP_STOCK_OP1 at TE's op1_246.op1 (never in CI: TE's file is not in the repo).
const stockPath = process.env.COOP_STOCK_OP1;
describe.runIf(stockPath !== undefined && existsSync(stockPath))('the coop package build from the real op1_246.op1', () => {
  it('builds the tested OP1_vdk.ldr into a package the checks pass', { timeout: 900_000 }, async () => {
    const delta = new Uint8Array(readFileSync(new URL('../../../public/firmware/coop-e10.delta', import.meta.url)));
    const started = Date.now();
    const built = await buildCoopPackage(new Uint8Array(readFileSync(stockPath as string)), { lzma, delta });
    const tar = await lzma.decompress(unwrapCrc(built.bytes));
    const app = readTar(tar).find((e) => e.name === APP_LDR);
    expect(sha(app?.data ?? new Uint8Array())).toBe(COOP_RELEASE.coopLdrSha256);
    console.log(`built ${built.bytes.length} bytes in ${(Date.now() - started) / 1000} s, sha256 ${built.sha256}`);
    if (process.env.COOP_BUILD_OUT) (await import('node:fs')).writeFileSync(process.env.COOP_BUILD_OUT, built.bytes);
  });
});
