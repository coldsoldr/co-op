/** Made-up contents for the OP-XY design preview. Nothing here comes from a device. */

export type XyTab = 'presets' | 'samples' | 'projects' | 'packs';
export type XyFolderTab = Exclude<XyTab, 'packs'>;
export type XyStatus = 'ok' | 'warn' | 'no';

export interface XyFolder {
  id: string;
  tab: XyFolderTab;
  group: string;
  name: string;
  /** A folder that cannot be removed. */
  fixed?: boolean;
}

export interface XyPreset {
  folder: string;
  name: string;
  type: 'drum' | 'multisampler' | 'sampler' | 'synth';
  engine: string;
  status: XyStatus;
  note: string;
  size: string;
}

export interface XySample {
  folder: string;
  name: string;
  format: string;
  pitch: string;
  seconds: number;
  status: XyStatus;
  note: string;
}

export interface XyProject {
  folder: string;
  name: string;
  versions: string;
  size: string;
}

export const XY_FOLDERS: readonly XyFolder[] = [
  { id: 'pad', tab: 'presets', group: 'category', name: 'pad', fixed: true },
  { id: 'lead', tab: 'presets', group: 'category', name: 'lead', fixed: true },
  { id: 'bass', tab: 'presets', group: 'category', name: 'bass', fixed: true },
  { id: 'keys', tab: 'presets', group: 'category', name: 'keys', fixed: true },
  { id: 'drum', tab: 'presets', group: 'category', name: 'drum', fixed: true },
  { id: 'cold', tab: 'presets', group: 'user', name: '[coldsoldr]' },
  { id: 'live', tab: 'presets', group: 'user', name: '[live set]' },
  { id: 's-user', tab: 'samples', group: 'samples', name: '[user]', fixed: true },
  { id: 's-field', tab: 'samples', group: 'samples', name: '[field]' },
  { id: 's-drums', tab: 'samples', group: 'samples', name: '[drums]' },
  { id: 'p-user', tab: 'projects', group: 'projects', name: '[user]', fixed: true },
  { id: 'p-templates', tab: 'projects', group: 'projects', name: '[templates]', fixed: true },
  { id: 'p-sketches', tab: 'projects', group: 'projects', name: '[sketches]' },
];

export const XY_PRESETS: readonly XyPreset[] = [
  { folder: 'pad', name: 'glass harp', type: 'synth', engine: 'axis', status: 'ok', note: 'ready', size: '6 KB' },
  { folder: 'pad', name: 'night choir', type: 'multisampler', engine: 'multisampler', status: 'ok', note: 'ready', size: '38 MB' },
  { folder: 'pad', name: 'grain drift', type: 'synth', engine: 'dissolve', status: 'ok', note: 'ready', size: '6 KB' },
  { folder: 'pad', name: 'felt strings', type: 'multisampler', engine: 'multisampler', status: 'no', note: '31 zones, max 24', size: '74 MB' },
  { folder: 'pad', name: 'long tape hiss', type: 'sampler', engine: 'sampler', status: 'warn', note: 'name over 14 chars', size: '4.1 MB' },
  { folder: 'lead', name: 'thin wire', type: 'synth', engine: 'hardsync', status: 'ok', note: 'ready', size: '6 KB' },
  { folder: 'lead', name: 'prism line', type: 'synth', engine: 'prism', status: 'ok', note: 'ready', size: '6 KB' },
  { folder: 'bass', name: 'sub cellar', type: 'synth', engine: 'simple', status: 'ok', note: 'ready', size: '6 KB' },
  { folder: 'keys', name: 'dusty ep', type: 'synth', engine: 'epiano', status: 'ok', note: 'ready', size: '6 KB' },
  { folder: 'keys', name: 'chapel', type: 'synth', engine: 'organ', status: 'ok', note: 'ready', size: '6 KB' },
  { folder: 'drum', name: 'dust kit', type: 'drum', engine: 'drum sampler', status: 'ok', note: 'ready', size: '12 MB' },
  { folder: 'drum', name: 'tin line', type: 'drum', engine: 'drumline', status: 'ok', note: 'ready', size: '6 KB' },
  { folder: 'cold', name: 'hollow wave', type: 'synth', engine: 'wavetable', status: 'ok', note: 'ready', size: '6 KB' },
  { folder: 'live', name: 'set opener', type: 'sampler', engine: 'sampler', status: 'ok', note: 'ready', size: '2.2 MB' },
];

export const XY_SAMPLES: readonly XySample[] = [
  { folder: 's-user', name: 'recording 014', format: 'wav · stereo', pitch: 'no note', seconds: 12, status: 'ok', note: 'ready' },
  { folder: 's-user', name: 'vox take 2', format: 'wav · mono', pitch: 'a3', seconds: 8, status: 'ok', note: 'ready' },
  { folder: 's-user', name: 'tram loop', format: 'aiff · stereo', pitch: 'no note', seconds: 23, status: 'no', note: 'over 20 s' },
  { folder: 's-user', name: 'kick_01', format: 'wav · mono', pitch: 'c2', seconds: 1, status: 'warn', note: 'rename, _ not allowed' },
  { folder: 's-field', name: 'harbour rain', format: 'wav · stereo', pitch: 'no note', seconds: 19, status: 'ok', note: 'ready' },
  { folder: 's-drums', name: 'snare #4', format: 'wav · mono', pitch: 'd2', seconds: 1, status: 'ok', note: 'ready' },
];

export const XY_PROJECTS: readonly XyProject[] = [
  { folder: 'p-user', name: 'cold harbour', versions: '14 versions', size: '3.4 MB' },
  { folder: 'p-user', name: 'set may', versions: '6 versions', size: '2.1 MB' },
  { folder: 'p-user', name: 'autosave', versions: '1 version', size: '1.8 MB' },
  { folder: 'p-templates', name: 'four track', versions: '2 versions', size: '0.4 MB' },
  { folder: 'p-sketches', name: 'idea 09', versions: '3 versions', size: '0.9 MB' },
];
