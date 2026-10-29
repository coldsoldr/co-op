import type { Patch } from '../codec/patch';
import type { PatchKind } from '../codec/op1-meta';
import type { SoundTraits } from '../codec/sound-traits';
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

/**
 * Which file extensions a patch kind uses on this profile's drive.
 *
 * Parser/validator hooks are deliberately absent: the codec classifies every
 * kind we currently support, hundekraft 'gin' and stock 'dbox' drum kits
 * included. They will be added when a profile needs a kind the codec does not
 * classify.
 */
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

/** The names, as saved in sounds, that a firmware can load. */
export interface ProfileSupports {
  /** `type` values of synth patches. */
  synthEngines: readonly string[];
  /** `type` values of drum patches. */
  drumEngines: readonly string[];
  /** `fx_type` values. */
  fx: readonly string[];
  /** `lfo_type` values. */
  lfo: readonly string[];
}

export interface DeviceProfile {
  /** '<device>/<firmware>', for example 'op1/stock'. */
  id: string;
  device: DeviceId;
  firmware: string;
  label: string;
  layout: DriveLayout;
  patchKinds: PatchKindSpec[];
  /**
   * Folder-level detection only: it sees names, not file contents. That is
   * enough to tell an OP-1 drive from an unrelated folder but not to rank
   * firmwares against each other. hundekraft, for example, keeps its plugins
   * on the internal volume, so its disk mode looks identical to stock; ranking
   * at that level is the job of `sniff`.
   */
  detect(root: DirectoryListing): Promise<DetectResult>;
  /**
   * Content detection over the traits of loaded patches, run after the library loads. Patch
   * contents prove where sounds came from, not which firmware is installed now, so callers may
   * suggest a switch but never switch on this alone. Absent means the profile cannot tell.
   */
  sniff?(traits: readonly SoundTraits[]): DetectResult;
  supports: ProfileSupports;
  compatibility(patch: Patch): Compat;
}
