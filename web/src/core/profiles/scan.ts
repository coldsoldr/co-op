import type { DeviceProfile } from './types';
import type { DirectoryListing } from '../store/listing';
import type { PatchKind } from '../codec/op1-meta';

export interface ScannedPatch {
  /** Path relative to the drive root, for example 'synth/user/1.aif'. */
  path: string;
  /** The category folder under the patch root, for example 'user'. */
  folder: string;
  fileName: string;
  kind: PatchKind;
}

function extensionOf(fileName: string): string {
  const dot = fileName.lastIndexOf('.');
  return dot < 0 ? '' : fileName.slice(dot).toLowerCase();
}

/** Dotfiles and macOS AppleDouble sidecars ('._name.aif') are never patches. */
const isHidden = (name: string): boolean => name.startsWith('.');

/**
 * Enumerates patch files without reading them: <root>/<folder>/<file> for each
 * patch root in the profile's layout.
 *
 * Output order is patch roots in profile order, then the listing's order
 * (folders, then files within each folder). Files directly under a patch root
 * and anything deeper than one folder are deliberately ignored: the OP-1 only
 * reads one level of category folders. Entries whose name starts with '.' are
 * skipped at both levels.
 *
 * Throws when a patch root names a kind the profile has no `patchKinds` spec
 * for; that is a profile misconfiguration, not a drive condition.
 */
export async function scanDrive(root: DirectoryListing, profile: DeviceProfile): Promise<ScannedPatch[]> {
  const found: ScannedPatch[] = [];
  for (const patchRoot of profile.layout.patchRoots) {
    const spec = profile.patchKinds.find((k) => k.kind === patchRoot.kind);
    if (!spec) {
      throw new Error(
        `profile "${profile.id}" has patch root "${patchRoot.path}" of kind "${patchRoot.kind}" but no patchKinds spec for it`,
      );
    }
    const folders = (await root.list(patchRoot.path)).filter((e) => e.kind === 'dir' && !isHidden(e.name));
    for (const folder of folders) {
      const folderPath = `${patchRoot.path}/${folder.name}`;
      for (const entry of await root.list(folderPath)) {
        if (entry.kind !== 'file' || isHidden(entry.name)) continue;
        if (!spec.extensions.includes(extensionOf(entry.name))) continue;
        found.push({
          path: `${folderPath}/${entry.name}`,
          folder: folder.name,
          fileName: entry.name,
          kind: patchRoot.kind,
        });
      }
    }
  }
  return found;
}
