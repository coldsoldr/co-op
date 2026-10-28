import type { DeviceProfile, DetectResult } from './types';
import type { DirectoryListing } from '../store/listing';
import { op1Stock } from './op1-stock';

/** Every supported profile. Later firmware profiles are appended here. */
export const profiles: readonly DeviceProfile[] = [op1Stock];

export function getProfile(id: string): DeviceProfile | undefined {
  return profiles.find((p) => p.id === id);
}

export interface RankedProfile {
  profile: DeviceProfile;
  result: DetectResult;
}

/** Runs every profile's detector and returns the matches, highest confidence first. */
export async function detectProfiles(root: DirectoryListing): Promise<RankedProfile[]> {
  const ranked = await Promise.all(
    profiles.map(async (profile) => ({ profile, result: await profile.detect(root) })),
  );
  return ranked
    .filter((r) => r.result.confidence > 0)
    .sort((a, b) => b.result.confidence - a.result.confidence);
}
