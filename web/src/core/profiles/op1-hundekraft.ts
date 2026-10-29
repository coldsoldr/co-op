import type { DetectResult, DeviceProfile, ProfileSupports } from './types';
import { isOp1rSelector, type SoundTraits } from '../codec/sound-traits';
import { traitCompatibility, type Finding, type TraitRule } from './compat';
import {
  op1Stock,
  folderDetect,
  SIGNATURE_FOLDERS,
  STOCK_SYNTH_ENGINES,
  STOCK_DRUM_ENGINES,
  STOCK_FX_TYPES,
  STOCK_LFO_TYPES,
} from './op1-stock';

/** Synth engines hundekraft 3.0 adds, as saved in `type` (docs/research/2026-10-28-custom-firmware-notes.md). */
export const HUNDEKRAFT_SYNTH_ENGINES: readonly string[] = [
  'supersaw', 'fmbell', 'granular', 'wavetable', 'taffy', 'pew', 'elg', 'mixtur',
  'arc', 'aether', 'eva', 'mbira', 'oxbow', 'sid', 'amp', 'iter',
];

/** Effects hundekraft 3.0 adds, as saved in `fx_type`; "in a room" keeps its spaces. */
export const HUNDEKRAFT_FX_TYPES: readonly string[] = [
  'tape', 'clouds', 'reverb', 'washer', 'snowglobe', 'palme', 'in a room',
  'stutter', 'hail', 'crush', 'warble', 'plate', 'father',
];

/**
 * LFOs hundekraft adds. 'quad' sits in the firmware's LFO name table next to the stock names,
 * but no saved sound using it has been seen yet, so the saved spelling is unconfirmed.
 */
export const HUNDEKRAFT_LFO_TYPES: readonly string[] = ['quad'];

/** Gin kits, which the firmware treats as DBox. */
export const HUNDEKRAFT_DRUM_ENGINES: readonly string[] = ['gin'];

const union = (a: readonly string[], b: readonly string[]): readonly string[] => [...new Set([...a, ...b])];

export const HUNDEKRAFT_SUPPORTS: ProfileSupports = {
  synthEngines: union(STOCK_SYNTH_ENGINES, HUNDEKRAFT_SYNTH_ENGINES),
  drumEngines: union(STOCK_DRUM_ENGINES, HUNDEKRAFT_DRUM_ENGINES),
  fx: union(STOCK_FX_TYPES, HUNDEKRAFT_FX_TYPES),
  lfo: union(STOCK_LFO_TYPES, HUNDEKRAFT_LFO_TYPES),
};

/**
 * Folder confidences just below stock's: a hundekraft drive looks exactly like a stock one in disk
 * mode, so on folders alone stock, the more common firmware, wins. `sniff` tells them apart.
 */
export const HUNDEKRAFT_FULL_MATCH = 0.85;
export const HUNDEKRAFT_PARTIAL_MATCH = 0.45;
/** Sniff confidence when loaded patches carry hundekraft-only traits. */
export const HUNDEKRAFT_SNIFF = 0.95;

/** A name hundekraft supports and stock does not. */
const customOnly = (name: string | null, stock: readonly string[], hundekraft: readonly string[]): boolean =>
  name !== null && !stock.includes(name) && hundekraft.includes(name);

/**
 * True when a sound's names (engine, effect, LFO) or Gin members can only have come from
 * hundekraft. Selector words alone do not count: they are legal stock knob values too.
 */
export function hasHundekraftTraits(traits: SoundTraits): boolean {
  const engine =
    traits.kind === 'drum'
      ? customOnly(traits.engine, STOCK_DRUM_ENGINES, HUNDEKRAFT_SUPPORTS.drumEngines)
      : customOnly(traits.engine, STOCK_SYNTH_ENGINES, HUNDEKRAFT_SUPPORTS.synthEngines);
  return (
    engine ||
    customOnly(traits.fx, STOCK_FX_TYPES, HUNDEKRAFT_SUPPORTS.fx) ||
    customOnly(traits.lfo, STOCK_LFO_TYPES, HUNDEKRAFT_SUPPORTS.lfo) ||
    traits.hasGin
  );
}

/**
 * hundekraft keeps a sound whose plugin is missing: an engine with a selector plays as stock Iter
 * and an effect with a selector as the stock filter (hundekraft manual). Unknown names without a
 * selector stay unsupported. opus sounds load only through the opus bridge or on opus itself.
 */
export const hundekraftRule: TraitRule = (traits, findings) => {
  const all = findings.map((finding): Finding => {
    if (finding.subject === 'engine' && traits.kind === 'synth' && isOp1rSelector(traits.engineSelector)) {
      return { level: 'warn', reason: `engine "${finding.name}": plugin may not be installed; plays as stock Iter` };
    }
    if (finding.subject === 'fx' && isOp1rSelector(traits.fxSelector)) {
      return { level: 'warn', reason: `effect "${finding.name}": plugin may not be installed; plays as the stock filter` };
    }
    return finding;
  });
  if (traits.hasOpus) all.push({ level: 'warn', reason: 'needs the opus bridge or opus firmware' });
  return all;
};

function sniff(traits: readonly SoundTraits[]): DetectResult {
  const matches = traits.filter(hasHundekraftTraits).length;
  if (matches === 0) return { confidence: 0, reason: 'no patches with hundekraft engines, effects or kits' };
  return {
    confidence: HUNDEKRAFT_SNIFF,
    reason: `${matches} of ${traits.length} patches use hundekraft engines, effects or kits`,
  };
}

export const op1Hundekraft: DeviceProfile = {
  id: 'op1/hundekraft',
  device: 'op1',
  firmware: 'hundekraft',
  label: 'OP-1 (hundekraft)',
  layout: op1Stock.layout,
  patchKinds: op1Stock.patchKinds,
  detect: folderDetect(SIGNATURE_FOLDERS, HUNDEKRAFT_FULL_MATCH, HUNDEKRAFT_PARTIAL_MATCH),
  sniff,
  supports: HUNDEKRAFT_SUPPORTS,
  compatibility: traitCompatibility(HUNDEKRAFT_SUPPORTS, { rule: hundekraftRule }),
};
