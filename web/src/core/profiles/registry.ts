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

/**
 * Runs every candidate's detector and returns one result per candidate, in
 * candidate order, unfiltered. A detector that throws (synchronously or by
 * rejecting) is reported as confidence 0 with the error message so one broken
 * profile cannot take the others down with it.
 */
export async function runDetectors(
  root: DirectoryListing,
  candidates: readonly DeviceProfile[] = profiles,
): Promise<RankedProfile[]> {
  const settled = await Promise.allSettled(candidates.map(async (profile) => profile.detect(root)));
  return settled.map((outcome, i) => {
    const profile = candidates[i] as DeviceProfile;
    if (outcome.status === 'fulfilled') return { profile, result: outcome.value };
    const message = outcome.reason instanceof Error ? outcome.reason.message : String(outcome.reason);
    return { profile, result: { confidence: 0, reason: `detector failed: ${message}` } };
  });
}

/**
 * Runs every candidate's detector and returns the matches, highest confidence
 * first. Ties keep candidate (registry) order: Array.prototype.sort is stable.
 */
export async function detectProfiles(
  root: DirectoryListing,
  candidates: readonly DeviceProfile[] = profiles,
): Promise<RankedProfile[]> {
  return (await runDetectors(root, candidates))
    .filter((r) => r.result.confidence > 0)
    .sort((a, b) => b.result.confidence - a.result.confidence);
}
