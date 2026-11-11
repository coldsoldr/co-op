/**
 * Builds a coop package from the owner's own stock op1_246.op1: unpack it, check its OP1_vdk.ldr is R246's,
 * rebuild the coop OP1_vdk.ldr from the delta and check it is the tested one byte for byte, pack it with the
 * stock content and without te-boot.ldr, then unpack the result again and check it the way
 * firmware/tools/verify_bootloader.py and pkg_check.py do. Nothing leaves the browser.
 */
import { bytesEqual, sha256Hex } from '../util';
import { readDelta } from './delta';
import type { Lzma } from './lzma';
import { APP_LDR, BOOT_LDR, PackageError, packageProblems, readTar, unwrapCrc, wrapCrc, writeTar } from './package';
import { COOP_RELEASE } from './release';

export type BuildStep = 'reading' | 'checking' | 'patching' | 'packing' | 'verifying';

export interface BuildProgress {
  step: BuildStep;
  /** 0 to 1 within the step, when known. */
  fraction?: number;
}

export interface BuildResult {
  bytes: Uint8Array;
  fileName: string;
  sha256: string;
  /** Files carried over from the stock package unchanged. */
  stockFiles: number;
}

export interface BuildOptions {
  lzma: Lzma;
  delta: Uint8Array;
  onProgress?: (progress: BuildProgress) => void;
  release?: typeof COOP_RELEASE;
}

export async function buildCoopPackage(stockFile: Uint8Array, options: BuildOptions): Promise<BuildResult> {
  const { lzma, onProgress } = options;
  const release = options.release ?? COOP_RELEASE;
  const step = (s: BuildStep, fraction?: number): void => onProgress?.(fraction === undefined ? { step: s } : { step: s, fraction });

  step('reading');
  const tar = await lzma.decompress(unwrapCrc(stockFile)).catch(() => {
    throw new PackageError('It does not unpack: this is not an OP-1 firmware file, or it is damaged.');
  });
  const entries = readTar(tar);

  step('checking');
  const stockLdr = entries.find((e) => e.name === APP_LDR);
  if (!stockLdr) throw new PackageError('It has no OP1_vdk.ldr, so it is not an OP-1 firmware package.');
  if ((await sha256Hex(stockLdr.data)) !== release.stockLdrSha256) {
    throw new PackageError(
      'This is not the stock R246 firmware. coop is built over TE\'s own op1_246.op1, not over an earlier release or another custom firmware (hundekraft, opus, an older coop).',
    );
  }
  if ((await sha256Hex(options.delta)) !== release.deltaSha256) throw new PackageError('The coop delta is damaged; reload the page and try again.');
  const delta = readDelta(options.delta);
  if (delta.stockSha256 !== release.stockLdrSha256 || delta.coopSha256 !== release.coopLdrSha256) {
    throw new PackageError('The coop delta does not belong to this release; reload the page and try again.');
  }

  step('patching');
  const coopLdr = delta.apply(stockLdr.data);
  if ((await sha256Hex(coopLdr)) !== release.coopLdrSha256) throw new PackageError('The patched OP1_vdk.ldr is not the tested one; nothing was built.');

  step('packing', 0);
  const coopTar = writeTar(entries, new Map([[APP_LDR, coopLdr]]), new Set([BOOT_LDR]));
  const bytes = wrapCrc(await lzma.compress(coopTar, (f) => step('packing', f)));

  step('verifying');
  const roundTrip = await lzma.decompress(unwrapCrc(bytes));
  if (!bytesEqual(roundTrip, coopTar)) throw new PackageError('The packed firmware does not unpack to what was packed; nothing to flash.');
  const problems = packageProblems(bytes, roundTrip, release.fileName);
  if (problems.length > 0) throw new PackageError(`The package failed its safety check: ${problems.join(' ')}`);
  // pkg_check: every file but OP1_vdk.ldr is the stock one, and only te-boot.ldr is gone.
  const built = new Map(readTar(roundTrip).map((e) => [e.name, e]));
  let stockFiles = 0;
  for (const entry of entries) {
    if (entry.name === BOOT_LDR) continue;
    const mine = built.get(entry.name);
    if (!mine) throw new PackageError(`The package lost ${entry.name}.`);
    if (entry.name === APP_LDR) continue;
    if (!bytesEqual(mine.data, entry.data)) throw new PackageError(`${entry.name} changed in packing.`);
    if (entry.type === '0') stockFiles += 1;
  }
  if (built.size !== entries.length - (entries.some((e) => e.name === BOOT_LDR) ? 1 : 0)) {
    throw new PackageError('The package has files the stock one does not.');
  }

  return { bytes, fileName: release.fileName, sha256: await sha256Hex(bytes), stockFiles };
}
