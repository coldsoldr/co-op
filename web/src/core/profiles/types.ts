import type { Patch } from '../codec/patch';
import type { PatchKind } from '../codec/op1-meta';
import type { DirectoryListing } from '../store/listing';

export type DeviceId = 'op1' | 'op1-field' | 'opxy';

export interface PatchRoot {
  /** Folder relative to the drive root, for example 'synth'. Its subfolders hold patches. */
  path: string;
  kind: PatchKind;
}

export interface DriveLayout {
  patchRoots: PatchRoot[];
  /** Folders that exist on the drive but hold no patches, kept for detection and backups. */
  otherRoots: string[];
}

export interface PatchKindSpec {
  kind: PatchKind;
  /** Lower-case extensions including the dot. */
  extensions: string[];
}

export interface DetectResult {
  /** 0 means definitely not this profile, 1 means certain. */
  confidence: number;
  reason: string;
}

export interface Compat {
  level: 'ok' | 'warn' | 'unsupported';
  reason?: string;
}

export interface DeviceProfile {
  /** '<device>/<firmware>', for example 'op1/stock'. */
  id: string;
  device: DeviceId;
  firmware: string;
  label: string;
  layout: DriveLayout;
  patchKinds: PatchKindSpec[];
  detect(root: DirectoryListing): Promise<DetectResult>;
  compatibility(patch: Patch): Compat;
}
