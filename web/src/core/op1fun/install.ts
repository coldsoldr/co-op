/**
 * Installing op1.fun patches onto a device store: each file is downloaded, checked with readPatch,
 * and written unchanged through the catalogue install path (writeAtomic under a free name) into
 * `<synth root>/op1fun` or `<drum root>/op1fun`. Files are installed one by one; a failure is
 * recorded and the rest carry on.
 */
import { isWindowsReservedName, type PackKind } from '@co-op/shared/packs';
import { installPatches } from '../catalogue/catalogue';
import { readPatch } from '../codec/patch';
import type { DeviceStore } from '../store/store';
import type { Op1FunPatch } from './types';

/** The folder name under each patch root. */
export const OP1FUN_FOLDER = 'op1fun';
/** Room for '.aif' and a '-NN' suffix from freeName within the 64 character file name limit. */
const MAX_BASE = 56;
const ILLEGAL = ':*?"<>|/\\';

export interface InstalledPatch {
  patch: Op1FunPatch;
  path: string;
}

export interface FailedPatch {
  patch: Op1FunPatch;
  reason: string;
}

export interface Op1FunInstallResult {
  installed: InstalledPatch[];
  failed: FailedPatch[];
}

const reason = (error: unknown): string => (error instanceof Error ? error.message : String(error));

/**
 * A file name the OP-1 drive can hold, from the patch name: characters FAT refuses become '-',
 * runs of spaces collapse, leading dots and trailing dots and spaces go, and the result is cut
 * to fit. Falls back to `op1fun-<id>`.
 */
export function patchFileName(patch: Pick<Op1FunPatch, 'id' | 'name'>): string {
  let base = '';
  for (const ch of patch.name) {
    const code = ch.codePointAt(0) ?? 0;
    base += code < 0x20 || code === 0x7f || ILLEGAL.includes(ch) ? '-' : ch;
  }
  base = base.replace(/\s+/g, ' ').trim().replace(/^[.\s]+/, '');
  base = [...base].slice(0, MAX_BASE).join('').replace(/[.\s]+$/, '');
  if (base === '') base = `op1fun-${patch.id}`.slice(0, MAX_BASE);
  if (isWindowsReservedName(base)) base = `${base}-patch`;
  return `${base}.aif`;
}

/** The patch kind from the file's own metadata, else from op1.fun's patch type. */
function kindOf(bytes: Uint8Array, patch: Op1FunPatch): PackKind {
  const parsed = readPatch(bytes);
  if (parsed.kind === 'synth' || parsed.kind === 'drum') return parsed.kind;
  const declared = patch.patchType.toLowerCase();
  if (declared === 'synth' || declared === 'drum') return declared;
  throw new Error('not an OP-1 synth or drum patch');
}

/**
 * Downloads, validates and installs each patch in turn, continuing past failures. `onProgress`
 * is called after each patch with the count done so far.
 */
export async function installOp1FunPatches(
  store: DeviceStore,
  profileId: string,
  patches: readonly Op1FunPatch[],
  fetchFile: (url: string) => Promise<Uint8Array>,
  onProgress?: (done: number, total: number) => void,
): Promise<Op1FunInstallResult> {
  const result: Op1FunInstallResult = { installed: [], failed: [] };
  for (const [index, patch] of patches.entries()) {
    try {
      if (!patch.fileUrl) throw new Error('op1.fun gave no file link');
      const bytes = await fetchFile(patch.fileUrl);
      const kind = kindOf(bytes, patch);
      const { installed } = await installPatches(store, profileId, OP1FUN_FOLDER, [
        { file: `${kind}/${patchFileName(patch)}`, bytes },
      ]);
      for (const path of installed) result.installed.push({ patch, path });
    } catch (error) {
      result.failed.push({ patch, reason: reason(error) });
    }
    onProgress?.(index + 1, patches.length);
  }
  return result;
}
