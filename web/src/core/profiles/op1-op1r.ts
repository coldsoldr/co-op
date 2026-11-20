import type { DeviceProfile, ProfileSupports } from './types';
import { traitCompatibility, traitsCompatibility } from './compat';
import { coopPluginRule, coopRule } from './op1-coop';
import { op1Stock, folderDetect, SIGNATURE_FOLDERS, STOCK_SUPPORTS } from './op1-stock';

/**
 * op1r, pcppcp's custom firmware for the original OP-1 (https://github.com/pcppcp/op1r): R246 patched, with synth
 * engines, effects, sequencers, drum and sampler types and tools as `.op1p` plugin files in `plugins/`, loaded at
 * boot. In disk mode an op1r drive is a stock drive.
 *
 * Its own engines and effects are all plugins now, so they come from the plugin library (the op1/op1r builds), not
 * from here. Its two LFOs are built in: quad and pond. Their saved spellings are unconfirmed (no op1r sound has been
 * seen), as with hundekraft's quad.
 */
export const OP1R_LFO_TYPES: readonly string[] = ['quad', 'pond'];

export const OP1R_SUPPORTS: ProfileSupports = {
  ...STOCK_SUPPORTS,
  lfo: [...new Set([...STOCK_SUPPORTS.lfo, ...OP1R_LFO_TYPES])],
};

/** Folder confidences below coop's: on folders alone an op1r drive is indistinguishable from a stock one. */
export const OP1R_FULL_MATCH = 0.75;
export const OP1R_PARTIAL_MATCH = 0.35;

/**
 * op1r keeps a sound whose plugin is missing as hundekraft does (op1r README, "Choosing synths and FX"): a plugin
 * engine plays as the stock iter and a plugin effect as the stock filter until the plugin is back, found again by
 * name. So coop's rule applies (the same as hundekraft's): a selector word is a warning, not an error, and opus
 * sounds need opus.
 */
export const op1Op1r: DeviceProfile = {
  id: 'op1/op1r',
  device: 'op1',
  firmware: 'op1r',
  label: 'OP-1 (op1r)',
  layout: op1Stock.layout,
  patchKinds: op1Stock.patchKinds,
  detect: folderDetect(SIGNATURE_FOLDERS, OP1R_FULL_MATCH, OP1R_PARTIAL_MATCH),
  supports: OP1R_SUPPORTS,
  compatibility: traitCompatibility(OP1R_SUPPORTS, { rule: coopRule }),
  pluginCompatibility: (context) => traitsCompatibility(OP1R_SUPPORTS, { rule: coopPluginRule(context) }),
};
