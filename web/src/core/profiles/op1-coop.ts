import type { DetectResult, DeviceProfile, PluginContext, ProfileSupports } from './types';
import { isOp1rSelector, type SoundTraits } from '../codec/sound-traits';
import { pluginFindings, traitCompatibility, traitsCompatibility, type Finding, type TraitRule } from './compat';
import { op1Stock, folderDetect, SIGNATURE_FOLDERS, STOCK_SUPPORTS } from './op1-stock';

/**
 * coop, the custom firmware in this repository (firmware/): stock R246 as the OS, its own audio core and a plugin
 * runtime. In disk mode a coop drive is a stock drive. Its built-in names are stock's; everything else comes from
 * `.coop` plugin files, which the plugin library and the device's inventory name (firmware/docs/engines.md,
 * firmware/docs/plugins.md, firmware/docs/sysex.md).
 */
export const COOP_SUPPORTS: ProfileSupports = STOCK_SUPPORTS;

/** Folder confidences below stock's and hundekraft's: on folders alone a coop drive is indistinguishable. */
export const COOP_FULL_MATCH = 0.8;
export const COOP_PARTIAL_MATCH = 0.4;
/** Sniff confidence when loaded patches carry the `coop` member. */
export const COOP_SNIFF = 0.95;

/**
 * coop keeps a sound whose plugin is missing, as hundekraft does: an engine with a selector plays as the stock iter
 * engine and an effect with a selector as the stock filter (firmware/docs/engines.md, plugins.md P2). The `coop`
 * member is coop's own and fine here. opus sounds need opus.
 */
export const coopRule: TraitRule = (traits, findings) => {
  const all = findings.map((finding): Finding => {
    if (finding.subject === 'engine' && traits.kind === 'synth' && isOp1rSelector(traits.engineSelector)) {
      return { level: 'warn', reason: `engine "${finding.name}": plugin may not be installed; plays as stock Iter` };
    }
    if (finding.subject === 'fx' && isOp1rSelector(traits.fxSelector)) {
      return { level: 'warn', reason: `effect "${finding.name}": plugin may not be installed; plays as the stock filter` };
    }
    return finding;
  });
  if (traits.hasOpus) all.push({ level: 'warn', reason: 'needs opus firmware' });
  return all;
};

export const coopPluginRule =
  (context: PluginContext): TraitRule =>
  (traits, findings) =>
    coopRule(traits, pluginFindings(traits, findings, context));

function sniff(traits: readonly SoundTraits[]): DetectResult {
  const matches = traits.filter((t) => t.hasCoop).length;
  if (matches === 0) return { confidence: 0, reason: 'no patches saved on coop' };
  return { confidence: COOP_SNIFF, reason: `${matches} of ${traits.length} patches were saved on coop` };
}

export const op1Coop: DeviceProfile = {
  id: 'op1/coop',
  device: 'op1',
  firmware: 'coop',
  label: 'OP-1 (coop)',
  layout: op1Stock.layout,
  patchKinds: op1Stock.patchKinds,
  detect: folderDetect(SIGNATURE_FOLDERS, COOP_FULL_MATCH, COOP_PARTIAL_MATCH),
  sniff,
  supports: COOP_SUPPORTS,
  compatibility: traitCompatibility(COOP_SUPPORTS, { rule: coopRule }),
  pluginCompatibility: (context) => traitsCompatibility(COOP_SUPPORTS, { rule: coopPluginRule(context) }),
};
