import type { DeviceProfile, DetectResult, ProfileSupports } from './types';
import type { DirectoryListing } from '../store/listing';
import { isOp1rSelector } from '../codec/sound-traits';
import { traitCompatibility, type TraitRule } from './compat';

/** Synth engine names as they appear in the op-1 JSON `type` field on stock firmware. */
export const STOCK_SYNTH_ENGINES: readonly string[] = [
  'cluster', 'digital', 'dna', 'drwave', 'dsynth', 'fm', 'iter',
  'phase', 'pulse', 'sampler', 'string', 'voltage',
];

/**
 * Drum engine names in the `type` field on stock firmware: the sample kit and the DBox drum
 * synth. Both appear in the firmware's engine name table (docs/research/2026-10-28-custom-firmware-notes.md).
 */
export const STOCK_DRUM_ENGINES: readonly string[] = ['drum', 'dbox'];

/**
 * Effect names in the `fx_type` field on stock firmware, confirmed in the firmware's effect name
 * table; delay, nitro and spring also appear in real device files.
 */
export const STOCK_FX_TYPES: readonly string[] = ['cwo', 'delay', 'grid', 'nitro', 'phone', 'punch', 'spring'];

/**
 * LFO names in the `lfo_type` field on stock firmware, confirmed in the firmware's LFO name table;
 * random, tremolo and value also appear in real device files. The same table holds 'quad', which
 * hundekraft adds, so it is not listed here.
 */
export const STOCK_LFO_TYPES: readonly string[] = ['bend', 'crank', 'element', 'midi', 'random', 'tremolo', 'value'];

/**
 * Confidence when every signature folder is present. Deliberately below 1:
 * the OP-1 Field presents the same three folders in disk mode, so folder
 * presence alone can never be certain which device this is.
 */
export const FULL_MATCH = 0.9;
/** Confidence when all but one signature folder is present. */
export const PARTIAL_MATCH = 0.5;

/** Stock OP-1 disk mode: these three top-level folders are always present. */
export const SIGNATURE_FOLDERS: readonly string[] = ['synth', 'drum', 'tape'];

/**
 * Builds a detector that looks only at top-level folder names, case-insensitively.
 * All folders present: `full`. One missing: `partial`. Fewer: 0. Profiles that share a disk
 * layout pass lower confidences so the more common firmware wins ties on folders alone.
 */
export function folderDetect(
  signatureFolders: readonly string[],
  full = FULL_MATCH,
  partial = PARTIAL_MATCH,
): DeviceProfile['detect'] {
  return async (root: DirectoryListing): Promise<DetectResult> => {
    const entries = await root.list('');
    const dirs = new Set(entries.filter((e) => e.kind === 'dir').map((e) => e.name.toLowerCase()));
    const found = signatureFolders.filter((name) => dirs.has(name));
    const missing = signatureFolders.filter((name) => !dirs.has(name));
    if (missing.length === 0) {
      return { confidence: full, reason: `found ${listNames(found)} folders` };
    }
    if (missing.length === 1 && found.length >= 2) {
      return {
        confidence: partial,
        reason: `found ${listNames(found)} folders but not ${missing.join(', ')}`,
      };
    }
    if (found.length === 1) {
      return { confidence: 0, reason: `found only ${found[0]}` };
    }
    return { confidence: 0, reason: 'no OP-1 folders found' };
  };
}

/** 'a', 'a and b', 'a, b and c'. */
function listNames(names: readonly string[]): string {
  if (names.length <= 1) return names.join('');
  return `${names.slice(0, -1).join(', ')} and ${names[names.length - 1]}`;
}

/** What stock R246 loads. */
export const STOCK_SUPPORTS: ProfileSupports = {
  synthEngines: STOCK_SYNTH_ENGINES,
  drumEngines: STOCK_DRUM_ENGINES,
  fx: STOCK_FX_TYPES,
  lfo: STOCK_LFO_TYPES,
};

/** Why a stock-unsafe patch matters: stock R246 is not forgiving about custom-firmware sounds. */
export const STOCK_BOOT_RISK = 'may stop a stock OP-1 at boot';

/**
 * Stock is strict: an unknown name or an opus member marks a sound saved on custom firmware, and
 * either is unsupported. An op1r selector word on its own is only a warning: 0x5300..0x53ff is also
 * a legal stock knob value, and hundekraft always pairs its selectors with its own names, so a
 * sound whose names are all stock is probably a stock sound whose fifth knob sits in that band.
 */
export const stockRule: TraitRule = (traits, findings) => {
  const all = [...findings];
  if (traits.hasOpus) all.push({ level: 'unsupported', reason: 'saved on opus firmware (has an opus member)' });
  if (all.length > 0) return all;
  if (isOp1rSelector(traits.engineSelector)) {
    all.push({ level: 'warn', reason: 'fifth knob value matches a hundekraft engine selector; probably fine on stock' });
  }
  if (isOp1rSelector(traits.fxSelector)) {
    all.push({ level: 'warn', reason: 'fifth effect value matches a hundekraft effect selector; probably fine on stock' });
  }
  return all;
};

export const op1Stock: DeviceProfile = {
  id: 'op1/stock',
  device: 'op1',
  firmware: 'stock',
  label: 'OP-1 (stock firmware)',
  layout: {
    patchRoots: [
      { path: 'synth', kind: 'synth' },
      { path: 'drum', kind: 'drum' },
    ],
    otherRoots: ['tape', 'album'],
  },
  patchKinds: [
    { kind: 'synth', extensions: ['.aif', '.aiff'] },
    { kind: 'drum', extensions: ['.aif', '.aiff'] },
  ],
  detect: folderDetect(SIGNATURE_FOLDERS),
  supports: STOCK_SUPPORTS,
  compatibility: traitCompatibility(STOCK_SUPPORTS, { rule: stockRule, unsupportedNote: STOCK_BOOT_RISK }),
};
