/**
 * Which plugins on an OP-1 have a newer build in the catalogue. Pure: an inventory (what the OP-1 lists, with each
 * file's version) and the plugin index in, a list out. Written for coop, whose inventory names files; the matching
 * and the version rule hold for any firmware whose inventory gives a path and a version.
 */
import { isInstallable, pluginFor, type HostedPluginEntry, type PluginIndex } from '@co-op/shared/plugins';
import type { Inventory } from '../usbmidi/inventory';

/**
 * A version as its numeric segments: "0.10" is [0, 10]. A leading "v" and surrounding spaces are let go. Null for
 * anything else (empty, "beta", "1.2b"): such a version cannot be compared.
 */
export function parseVersion(version: string): number[] | null {
  const text = version.trim().replace(/^v/i, '');
  return /^\d+(\.\d+)*$/.test(text) ? text.split('.').map(Number) : null;
}

/**
 * Compares two versions segment by segment, as numbers: 0.10 is after 0.9, 1.0 equals 1. Negative when `a` is the
 * older, 0 when they are the same, positive when `a` is the newer; null when either cannot be read.
 */
export function compareVersions(a: string, b: string): number | null {
  const x = parseVersion(a);
  const y = parseVersion(b);
  if (!x || !y) return null;
  for (let i = 0; i < Math.max(x.length, y.length); i++) {
    const d = (x[i] ?? 0) - (y[i] ?? 0);
    if (d !== 0) return d;
  }
  return 0;
}

/**
 * The coop core a build asks for, from its `requires` ("coop e10", "coop e4b"): the number after the e. Null when
 * it names none.
 */
export function coopCoreOf(text: string | null | undefined): number | null {
  const m = /\be(\d+)/.exec(text ?? '');
  return m ? Number(m[1]) : null;
}

/**
 * True when a core with this build tag ("e12") runs a build that requires `requires`. A requirement or a tag that
 * names no core number does not stand in the way: the OP-1 judges the file itself at boot.
 */
export function coreSatisfies(requires: string | undefined, build: string | null | undefined): boolean {
  const need = coopCoreOf(requires);
  const have = /^e(\d+)/.exec(build ?? '');
  return need === null || !have || Number(have[1]) >= need;
}

export interface PluginUpdate {
  /** The catalogue's entry for the firmware: what to fetch and send. */
  plugin: HostedPluginEntry;
  /** The file's name on the OP-1 (and in the catalogue). */
  file: string;
  /** The version on the OP-1; null when the inventory gives none it can read (the update is offered all the same). */
  from: string | null;
  /** The catalogue's version. */
  to: string;
}

export interface BlockedUpdate extends PluginUpdate {
  /** What the newer build asks for that this OP-1 does not run, such as "coop e12". */
  needs: string;
}

export interface UpdateList {
  /** In the inventory's order. */
  updates: PluginUpdate[];
  /** Newer builds this OP-1's core cannot run: listed, never sent. */
  blocked: BlockedUpdate[];
}

/**
 * The plugin files on the OP-1 the catalogue has a newer build of, for `firmware` (a profile id).
 *
 * A file on the OP-1 is matched to the catalogue build of the same file name; files the catalogue does not hold are
 * left alone. It is an update when the catalogue's version is the newer by compareVersions; the same or an older
 * one never is. A version on the OP-1 that cannot be read (empty: a file the boot has not seen, or one without a
 * version) is offered the catalogue's, as `from: null`. A file switched off is updated like any other (it stays
 * off). A build whose `requires` names a later coop core than the OP-1 runs goes to `blocked`.
 */
export function listUpdates(inventory: Inventory | null, index: PluginIndex | null, firmware = 'op1/coop'): UpdateList {
  const updates: PluginUpdate[] = [];
  const blocked: BlockedUpdate[] = [];
  if (!inventory || !index) return { updates, blocked };
  const builds = new Map<string, HostedPluginEntry>();
  for (const entry of index.plugins) {
    const build = pluginFor(entry, firmware);
    if (build && isInstallable(build) && !builds.has(build.file)) builds.set(build.file, build);
  }
  const seen = new Set<string>();
  for (const record of inventory.records) {
    const file = record.path.replace(/^plugins\//, '');
    const plugin = builds.get(file);
    if (!plugin || record.path === '' || seen.has(file)) continue;
    seen.add(file);
    // A catalogue version that cannot be read is never offered: there is no telling it is the newer.
    if (!parseVersion(plugin.version)) continue;
    const known = parseVersion(record.version) !== null;
    if (known && (compareVersions(plugin.version, record.version) as number) <= 0) continue;
    const update: PluginUpdate = { plugin, file, from: known ? record.version : null, to: plugin.version };
    if (firmware === 'op1/coop' && !coreSatisfies(plugin.requires, inventory.build)) {
      blocked.push({ ...update, needs: `coop e${coopCoreOf(plugin.requires)}` });
    } else updates.push(update);
  }
  return { updates, blocked };
}
