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

/** Enumerates patch files without reading them: <root>/<folder>/<file> for each patch root. */
export async function scanDrive(root: DirectoryListing, profile: DeviceProfile): Promise<ScannedPatch[]> {
  const found: ScannedPatch[] = [];
  for (const patchRoot of profile.layout.patchRoots) {
    const extensions = profile.patchKinds.find((k) => k.kind === patchRoot.kind)?.extensions ?? [];
    const folders = (await root.list(patchRoot.path)).filter((e) => e.kind === 'dir');
    for (const folder of folders) {
      const folderPath = `${patchRoot.path}/${folder.name}`;
      for (const entry of await root.list(folderPath)) {
        if (entry.kind !== 'file' || !extensions.includes(extensionOf(entry.name))) continue;
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
