/**
 * The packs CLI. Run from web/ with tsx:
 *
 *   tsx scripts/packs.ts validate         check every pack in ../packs; exit 1 on any issue
 *   tsx scripts/packs.ts build <outDir>   validate, then copy the packs and write <outDir>/packs/index.json
 *
 * PACKS_NOW (an ISO time) fixes the index's generatedAt, for reproducible output.
 * PACKS_ONLY (comma-separated folders) limits a build to those packs, so the e2e tests do not
 * change when a community pack is added.
 */
import { resolve } from 'node:path';
import { DEFAULT_PACKS_DIR, buildPacks, formatIssues, loadPacks, type PacksResult } from './packs-lib';

function now(): Date {
  const fixed = process.env.PACKS_NOW;
  if (fixed === undefined || fixed === '') return new Date();
  const date = new Date(fixed);
  if (Number.isNaN(date.getTime())) throw new Error(`PACKS_NOW is not a valid time: ${fixed}`);
  return date;
}

function only(): string[] | undefined {
  const list = process.env.PACKS_ONLY;
  return list === undefined || list === '' ? undefined : list.split(',');
}

function report(result: PacksResult, verb: string): number {
  if (result.issues.length > 0) {
    console.error(formatIssues(result.issues));
    return 1;
  }
  const count = result.index.packs.length;
  console.log(`${verb} ${count} pack${count === 1 ? '' : 's'}: ${result.index.packs.map((p) => p.slug).join(', ') || '(none)'}`);
  return 0;
}

async function main(args: string[]): Promise<number> {
  const [command, outDir] = args;
  if (command === 'validate' && outDir === undefined) {
    return report(await loadPacks(DEFAULT_PACKS_DIR, { now: now() }), 'validated');
  }
  if (command === 'build' && outDir !== undefined) {
    const selection = only();
    const options = { now: now(), ...(selection ? { only: selection } : {}) };
    return report(await buildPacks(DEFAULT_PACKS_DIR, resolve(outDir), options), `built into ${resolve(outDir, 'packs')}:`);
  }
  console.error('usage: tsx scripts/packs.ts validate | build <outDir>');
  return 2;
}

process.exitCode = await main(process.argv.slice(2));
