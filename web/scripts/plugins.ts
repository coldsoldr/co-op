/**
 * The plugins CLI. Run from web/ with tsx:
 *
 *   tsx scripts/plugins.ts validate         check every plugin in ../plugins; exit 1 on any issue
 *   tsx scripts/plugins.ts build <outDir>   validate, then copy the plugins and write <outDir>/plugins/index.json
 *
 * Warnings are printed but do not fail. Without a ../plugins folder the index is empty.
 * PLUGINS_NOW (an ISO time) fixes the index's generatedAt, for reproducible output.
 */
import { resolve } from 'node:path';
import { DEFAULT_PLUGINS_DIR, buildPlugins, formatIssues, loadPlugins, type PluginsResult } from './plugins-lib';

function now(): Date {
  const fixed = process.env.PLUGINS_NOW;
  if (fixed === undefined || fixed === '') return new Date();
  const date = new Date(fixed);
  if (Number.isNaN(date.getTime())) throw new Error(`PLUGINS_NOW is not a valid time: ${fixed}`);
  return date;
}

function report(result: PluginsResult, verb: string): number {
  if (result.warnings.length > 0) console.warn(formatIssues(result.warnings, 'warning'));
  if (result.issues.length > 0) {
    console.error(formatIssues(result.issues));
    return 1;
  }
  const count = result.index.plugins.length;
  console.log(`${verb} ${count} plugin${count === 1 ? '' : 's'}: ${result.index.plugins.map((p) => p.id).join(', ') || '(none)'}`);
  return 0;
}

async function main(args: string[]): Promise<number> {
  const [command, outDir] = args;
  if (command === 'validate' && outDir === undefined) {
    return report(await loadPlugins(DEFAULT_PLUGINS_DIR, { now: now() }), 'validated');
  }
  if (command === 'build' && outDir !== undefined) {
    return report(await buildPlugins(DEFAULT_PLUGINS_DIR, resolve(outDir), { now: now() }), `built into ${resolve(outDir, 'plugins')}:`);
  }
  console.error('usage: tsx scripts/plugins.ts validate | build <outDir>');
  return 2;
}

process.exitCode = await main(process.argv.slice(2));
