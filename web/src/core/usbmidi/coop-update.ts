/**
 * "Update all" for a coop OP-1: every plugin file on it that the catalogue has a newer build of is replaced, one
 * after another, over the same PUT as a single install (coop.ts). Nothing here touches the page: the caller gives
 * the download, the questions and the progress line, so the whole run is tested against the fake device.
 */
import type { HostedPluginEntry, PluginIndex } from '@co-op/shared/plugins';
import { formatKb, largestRows, planMemory, shortNote, type DeviceCoopFile, type DeviceMemoryReport, type KnownCoopFile } from '../plugins/memory';
import { listUpdates, type BlockedUpdate, type PluginUpdate } from '../plugins/updates';
import { messageOf } from '../util';
import { coopPutPlugin, readCoopInventory } from './coop';
import type { Inventory } from './inventory';
import type { Session } from './session';

const titleOf = (file: string): string => file.replace(/\.coop$/, '').replaceAll('_', ' ');
const plugins = (n: number): string => `${n} ${n === 1 ? 'plugin' : 'plugins'}`;

/** The inventory's coop files as the memory sum takes them (memory.ts). */
export const deviceFilesOf = (inventory: Inventory | null): DeviceCoopFile[] =>
  (inventory?.records ?? [])
    .filter((r) => r.source === 'coop')
    .map((r) => ({
      file: r.path.replace(/^plugins\//, ''),
      size: r.size,
      kind: r.kind === 'feature' ? null : r.kind,
      loaded: r.state === 'loaded',
      enabled: r.enabled !== false,
      reason: r.reason,
      ...(r.need === undefined ? {} : { need: r.need, scratch: r.scratch ?? 0 }),
      ...(r.heap === undefined ? {} : { heap: r.heap }),
    }));

/** The plugin memory by the OP-1's own count, from an e12 or e13 inventory's header; null before e12. */
export function deviceReportOf(inventory: Inventory | null): DeviceMemoryReport | null {
  const m = inventory?.memory;
  if (!m) return null;
  return {
    budget: m.budget,
    used: m.used,
    exact: true,
    needs: {},
    fxbuf: m.fxbuf,
    fxbufs: m.fxbufs,
    ...(m.clip === null ? {} : { clip: m.clip }),
    ...(m.pools ? { pools: m.pools } : {}),
    ...(m.place ? { place: m.place } : {}),
    ...(m.placeSrc ? { placeSrc: m.placeSrc } : {}),
  };
}

/** One line of the list the user agrees to: "fickle: 0.2 → 0.3". */
export const updateLine = (u: PluginUpdate): string => `${titleOf(u.file)}: ${u.from ?? 'unknown version'} → ${u.to}`;
const blockedLine = (blocked: readonly BlockedUpdate[]): string =>
  `Not updated: ${blocked.map((b) => `${titleOf(b.file)} ${b.to} needs ${b.needs}`).join(', ')}.`;

/** What is asked once before anything is sent: every plugin that will be replaced, with its versions. */
export function updateQuestion(updates: readonly PluginUpdate[], blocked: readonly BlockedUpdate[]): string {
  return [
    `Update ${plugins(updates.length)} on the OP-1?`,
    updates.map(updateLine).join('\n'),
    `${updates.length === 1 ? 'It replaces the file' : 'Each replaces the file'} on the OP-1.`,
    ...(blocked.length > 0 ? [blockedLine(blocked)] : []),
  ].join('\n\n');
}

/**
 * What is asked when the updated set would not fit the OP-1's plugin memory (a newer build can be larger); null
 * when it fits, the core has no budget, or the catalogue cannot size the files. As the question before a single
 * install: the files are harmless there, the OP-1 just leaves a plugin out at boot.
 */
export function updateMemoryQuestion(inventory: Inventory, known: readonly KnownCoopFile[], updates: readonly PluginUpdate[]): string | null {
  const add = updates.flatMap((u) => known.filter((k) => k.file === u.file && k.sizeBytes === u.plugin.sizeBytes).slice(0, 1));
  const plan = planMemory(deviceFilesOf(inventory), known, { build: inventory.build, report: deviceReportOf(inventory), add });
  if (!plan || plan.level !== 'over') return null;
  const largest = largestRows(plan).map((r) => `${titleOf(r.file)} (${formatKb(r.frees)})`);
  return [
    `With the updates, the plugins do not fit in the OP-1’s plugin memory: those switched on need ${plan.approximate ? 'about ' : ''}${formatKb(plan.used)}; the OP-1 has ${formatKb(plan.budget)}.`,
    plan.refused.length > 0
      ? `At the next power cycle the OP-1 would leave out ${plan.refused.map(titleOf).join(', ')}: it loads the plugins in name order.`
      : 'At the next power cycle the OP-1 would leave out at least one plugin.',
    ...(shortNote(plan) ? [shortNote(plan) as string] : []),
    ...(largest.length > 0 ? [`The largest: ${largest.join(', ')}. Remove or switch off one to make room.`] : []),
    'Update anyway? The files do no harm; a plugin left out loads once there is room.',
  ].join('\n\n');
}

export interface UpdateOptions {
  index: PluginIndex | null;
  /** The catalogue's coop files with what they take of the plugin memory (catalogue.ts coopFilesOf). */
  known: readonly KnownCoopFile[];
  /** Only this file ("fickle.coop"): the row's own Update. Without it, every update there is. */
  only?: string;
  /**
   * Files already sent since the OP-1 last booted, with the version sent: its inventory names the version the boot
   * read until the next power cycle, so these are not sent again.
   */
  already?: ReadonlyMap<string, string>;
  /** Downloads a build and checks it against the index (catalogue.ts fetchPluginBytes). */
  fetchBytes: (plugin: HostedPluginEntry) => Promise<Uint8Array>;
  /** Asks the user; false stops before anything is sent. */
  confirm: (question: string) => boolean | Promise<boolean>;
  /** The line to show while it runs: "Updating 3 of 8: fickle, 40 %". */
  onProgress?: (text: string) => void;
}

export type UpdateOutcome =
  /** Nothing on the OP-1 has a newer build it can run. */
  | { kind: 'none'; blocked: BlockedUpdate[]; inventory: Inventory }
  /** The user said no; nothing was sent. */
  | { kind: 'cancelled'; inventory: Inventory }
  | {
      kind: 'ran';
      /** Sent whole and confirmed by the OP-1 (size and CRC), in order. */
      done: PluginUpdate[];
      /** The one that stopped the run, with why; `sending` when its file had begun to go over. Null when all went. */
      failed: { update: PluginUpdate; error: string; sending: boolean } | null;
      /** Not tried, because the run stopped before them. */
      skipped: PluginUpdate[];
      blocked: BlockedUpdate[];
      /** As read after the run; null when the OP-1 no longer answered. */
      inventory: Inventory | null;
    };

/**
 * Reads a fresh inventory, lists the updates, asks once with the list (and once more if the updated set would not
 * fit the plugin memory), then downloads and sends each file in turn. The first failure stops the run: what went
 * before stays updated, the rest is not tried.
 */
export async function updateCoopPlugins(session: Session, options: UpdateOptions): Promise<UpdateOutcome> {
  const inventory = await readCoopInventory(session);
  const list = listUpdates(inventory, options.index);
  const wanted = (u: PluginUpdate): boolean => (options.only === undefined || u.file === options.only) && options.already?.get(u.file) !== u.to;
  const updates = list.updates.filter(wanted);
  const blocked = options.only === undefined ? list.blocked : list.blocked.filter((b) => b.file === options.only);
  if (updates.length === 0) return { kind: 'none', blocked, inventory };
  if (!(await options.confirm(updateQuestion(updates, blocked)))) return { kind: 'cancelled', inventory };
  const memory = updateMemoryQuestion(inventory, options.known, updates);
  if (memory && !(await options.confirm(memory))) return { kind: 'cancelled', inventory };

  const done: PluginUpdate[] = [];
  let failed: { update: PluginUpdate; error: string; sending: boolean } | null = null;
  for (const [i, update] of updates.entries()) {
    const doing = `Updating ${i + 1} of ${updates.length}: ${titleOf(update.file)}`;
    options.onProgress?.(doing);
    let sending = false;
    try {
      const bytes = await options.fetchBytes(update.plugin);
      sending = true;
      await coopPutPlugin(session, update.file, bytes, {
        onProgress: (n, total) => options.onProgress?.(`${doing}, ${Math.round((100 * n) / total)} %`),
      });
      done.push(update);
    } catch (e) {
      failed = { update, error: messageOf(e), sending };
      break;
    }
  }
  const skipped = updates.slice(done.length + (failed ? 1 : 0));
  const after = await readCoopInventory(session).catch(() => null);
  return { kind: 'ran', done, failed, skipped, blocked, inventory: after };
}

/** The run in words: what was updated, and after a failure which file failed, why, and which were not tried. */
export function updateSummary(outcome: UpdateOutcome): string {
  if (outcome.kind === 'cancelled') return 'Nothing was sent.';
  const left = outcome.blocked.length > 0 ? ` ${blockedLine(outcome.blocked)}` : '';
  if (outcome.kind === 'none') return `All plugins are up to date.${left}`;
  const names = (list: readonly PluginUpdate[]): string => list.map((u) => titleOf(u.file)).join(', ');
  const { done, failed, skipped } = outcome;
  if (!failed) return `${plugins(done.length)} updated. Power-cycle the OP-1 to load ${done.length === 1 ? 'it' : 'them'}.${left}`;
  const total = done.length + 1 + skipped.length;
  return [
    done.length > 0 ? `Updated ${done.length} of ${total}: ${names(done)}.` : `Updated none of ${total}.`,
    `${titleOf(failed.update.file)} failed: ${failed.error.replace(/\.$/, '')}.`,
    ...(failed.sending ? ['Its file on the OP-1 may be incomplete and will not load until it is sent again.'] : []),
    ...(skipped.length > 0 ? [`Not sent: ${names(skipped)}.`] : []),
    ...(done.length > 0 ? ['Power-cycle the OP-1 to load the updated ones.'] : []),
  ].join(' ') + left;
}
