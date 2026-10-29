import type { PluginIndex, PluginIndexEntry } from '@co-op/shared/plugins';
import type { SoundTraits } from '../codec/sound-traits';
import { getProfile } from '../profiles/registry';
import type { PluginContext } from '../profiles/types';
import { savedNameOf, type Inventory } from '../usbmidi/inventory';

/**
 * The names a sound needs a plugin for, with the names the firmware has built in left out: the
 * engine of a synth sound, the effect, and the opus member's plugin and effects.
 */
function pluginNames(traits: SoundTraits, firmwareId: string): Set<string> {
  const supports = getProfile(firmwareId)?.supports;
  const names = new Set<string>();
  const add = (name: string | null, builtIn: readonly string[] = []): void => {
    if (name && !builtIn.includes(name)) names.add(name);
  };
  if (traits.kind === 'synth') add(traits.engine, supports?.synthEngines);
  add(traits.fx, supports?.fx);
  add(traits.opusPlugin);
  for (const effect of traits.opusEffects) add(effect, supports?.fx);
  return names;
}

/**
 * The library plugins a sound needs on `firmwareId` (a profile id): those for that firmware whose
 * saved name is the sound's engine (synth sounds), effect, or opus plugin or effects. Names the
 * firmware has built in need nothing. In index order.
 */
export function requiredPlugins(traits: SoundTraits, index: PluginIndex, firmwareId: string): PluginIndexEntry[] {
  const names = pluginNames(traits, firmwareId);
  return index.plugins.filter((p) => p.firmware === firmwareId && p.savedName !== undefined && names.has(p.savedName));
}

/** The saved names of the plugins an inventory reports loaded; empty without an inventory. */
export function installedNames(inventory: Inventory | null): Set<string> {
  return new Set((inventory?.records ?? []).filter((r) => r.state === 'loaded').map((r) => savedNameOf(r.name)));
}

/**
 * What a profile needs to know about plugins (see withPlugins): the library plugins for
 * `firmwareId` by saved name, and what the inventory reports loaded. Null without an index.
 */
export function pluginContext(index: PluginIndex | null, firmwareId: string, inventory: Inventory | null): PluginContext | null {
  if (!index) return null;
  const library = new Map<string, string>();
  for (const p of index.plugins) if (p.firmware === firmwareId && p.savedName !== undefined) library.set(p.savedName, p.id);
  return { library, loaded: installedNames(inventory) };
}
