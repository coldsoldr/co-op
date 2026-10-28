import type { DeviceProfile, DetectResult, Compat } from './types';
import type { DirectoryListing } from '../store/listing';
import type { Patch } from '../codec/patch';
import { isSynthMeta } from '../codec/op1-meta';

/** Synth engine names as they appear in the op-1 JSON `type` field on stock firmware. */
export const STOCK_SYNTH_ENGINES: readonly string[] = [
  'cluster', 'digital', 'dna', 'drwave', 'dsynth', 'fm', 'iter',
  'phase', 'pulse', 'sampler', 'string', 'voltage',
];

const SIGNATURE_FOLDERS = ['synth', 'drum', 'tape'];

async function detect(root: DirectoryListing): Promise<DetectResult> {
  const entries = await root.list('');
  const dirs = new Set(entries.filter((e) => e.kind === 'dir').map((e) => e.name.toLowerCase()));
  const found = SIGNATURE_FOLDERS.filter((name) => dirs.has(name));
  if (found.length === SIGNATURE_FOLDERS.length) {
    return { confidence: 0.9, reason: 'found synth, drum and tape folders' };
  }
  if (found.length === 2) {
    const missing = SIGNATURE_FOLDERS.filter((n) => !found.includes(n)).join(', ');
    return { confidence: 0.5, reason: `found ${found.join(' and ')} folders but not ${missing}` };
  }
  return { confidence: 0, reason: 'no OP-1 folders found' };
}

function compatibility(patch: Patch): Compat {
  if (patch.kind === 'unknown' || !patch.meta) {
    return { level: 'unsupported', reason: 'file has no op-1 metadata' };
  }
  if (patch.kind === 'synth' && isSynthMeta(patch.meta) && !STOCK_SYNTH_ENGINES.includes(patch.meta.type)) {
    return { level: 'warn', reason: `engine "${patch.meta.type}" is not part of stock firmware` };
  }
  return { level: 'ok' };
}

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
  detect,
  compatibility,
};
