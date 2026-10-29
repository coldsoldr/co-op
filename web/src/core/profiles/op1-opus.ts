import type { DetectResult, DeviceProfile, ProfileSupports } from './types';
import type { DirectoryListing } from '../store/listing';
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
import { HUNDEKRAFT_SUPPORTS } from './op1-hundekraft';

/**
 * Effect names opus plugins save in `fx_type`: the starter effects, bare (as the docs show for
 * bitcrush) and with their plugin id, since no real opus sound has been seen yet.
 */
export const OPUS_FX_TYPES: readonly string[] = [
  'bitcrush', 'chorus', 'plate', 'filter',
  'opus.bitcrush', 'opus.chorus', 'opus.plate', 'opus.filter',
];

/** opus engines save as stock `iter` plus a mark, so the stock engine list covers them. */
export const OPUS_SUPPORTS: ProfileSupports = {
  synthEngines: STOCK_SYNTH_ENGINES,
  drumEngines: STOCK_DRUM_ENGINES,
  fx: [...STOCK_FX_TYPES, ...OPUS_FX_TYPES],
  lfo: STOCK_LFO_TYPES,
};

/** The top-level folder opus plugins live in on the disk-mode drive. */
export const MODS_FOLDER = 'mods';
export const OPUS_PLUGIN_EXTENSION = '.opmod';
/** A mods folder holding at least one .opmod file. */
export const OPUS_MODS_MATCH = 0.97;
/** A mods folder without any .opmod file. */
export const OPUS_EMPTY_MODS_MATCH = 0.6;
/** Without a mods folder an opus drive looks exactly like a stock one. */
export const OPUS_FOLDER_CAP = 0.5;
/**
 * Sniff confidence when loaded patches were saved on opus: above stock's folder match, so a
 * stock-looking drive of opus sounds gets the suggestion, and above hundekraft's sniff, because
 * opus also hosts hundekraft sounds through its bridge plugin.
 */
export const OPUS_SNIFF = 0.96;

const BRIDGE = 'needs the opus bridge plugin';

const stockFolders = folderDetect(SIGNATURE_FOLDERS);

/**
 * mods/ beside at least one stock signature folder (synth, drum or tape): OPUS_MODS_MATCH with a
 * .opmod file in it, OPUS_EMPTY_MODS_MATCH without. Otherwise the stock folder rule capped at
 * OPUS_FOLDER_CAP. Names only; plugin bytes are checked when the Library lists them.
 */
async function detect(root: DirectoryListing): Promise<DetectResult> {
  const dirs = (await root.list('')).filter((e) => e.kind === 'dir');
  const mods = dirs.find((e) => e.name.toLowerCase() === MODS_FOLDER);
  const opFolder = dirs.some((e) => SIGNATURE_FOLDERS.includes(e.name.toLowerCase()));
  if (mods && opFolder) {
    const plugins = (await root.list(mods.name)).filter(
      (e) => e.kind === 'file' && e.name.toLowerCase().endsWith(OPUS_PLUGIN_EXTENSION),
    );
    return plugins.length > 0
      ? { confidence: OPUS_MODS_MATCH, reason: 'found mods folder with opus plugins' }
      : { confidence: OPUS_EMPTY_MODS_MATCH, reason: 'found mods folder but no opus plugins in it' };
  }
  const stock = await stockFolders(root);
  return { confidence: Math.min(stock.confidence, OPUS_FOLDER_CAP), reason: stock.reason };
}

/** True when a finding names something hundekraft has, which the opus bridge plugin can host. */
function bridgeable(finding: Finding, traits: SoundTraits): boolean {
  if (finding.name === undefined) return false;
  if (finding.subject === 'engine') {
    const engines = traits.kind === 'drum' ? HUNDEKRAFT_SUPPORTS.drumEngines : HUNDEKRAFT_SUPPORTS.synthEngines;
    return engines.includes(finding.name);
  }
  if (finding.subject === 'fx') return HUNDEKRAFT_SUPPORTS.fx.includes(finding.name);
  return HUNDEKRAFT_SUPPORTS.lfo.includes(finding.name);
}

/**
 * opus keeps every sound and names the plugin a missing engine or effect needs, so plugin needs
 * are warnings: the opus member's plugin (or an engine mark naming none), effect slots holding
 * effects opus does not ship, and hundekraft names or op1r selectors,
 * which load through the opus bridge plugin. Names no known firmware uses stay unsupported.
 */
export const opusRule: TraitRule = (traits, findings) => {
  const all: Finding[] = [];
  let bridge = isOp1rSelector(traits.engineSelector) || isOp1rSelector(traits.fxSelector);
  for (const finding of findings) {
    if (bridgeable(finding, traits)) bridge = true;
    else all.push(finding);
  }
  if (traits.opusPlugin !== null) all.push({ level: 'warn', reason: `needs plugin ${traits.opusPlugin}` });
  else if (traits.opusMark !== null) all.push({ level: 'warn', reason: 'needs an opus plugin' });
  for (const effect of new Set(traits.opusEffects)) {
    if (!OPUS_SUPPORTS.fx.includes(effect)) all.push({ level: 'warn', reason: `needs the effect ${effect}` });
  }
  if (bridge) all.push({ level: 'warn', reason: BRIDGE });
  return all;
};

function sniff(traits: readonly SoundTraits[]): DetectResult {
  const matches = traits.filter((t) => t.hasOpus).length;
  if (matches === 0) return { confidence: 0, reason: 'no patches saved on opus' };
  return { confidence: OPUS_SNIFF, reason: `${matches} of ${traits.length} patches were saved on opus` };
}

export const op1Opus: DeviceProfile = {
  id: 'op1/opus',
  device: 'op1',
  firmware: 'opus',
  label: 'OP-1 (opus)',
  layout: {
    patchRoots: op1Stock.layout.patchRoots,
    otherRoots: [...op1Stock.layout.otherRoots, MODS_FOLDER],
    backupOtherRoots: [MODS_FOLDER],
  },
  patchKinds: op1Stock.patchKinds,
  detect,
  sniff,
  supports: OPUS_SUPPORTS,
  compatibility: traitCompatibility(OPUS_SUPPORTS, { rule: opusRule }),
};
