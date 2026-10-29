import type { DeviceProfile, DetectResult } from './types';
import type { DirectoryListing } from '../store/listing';
import type { SoundTraits } from '../codec/sound-traits';
import { op1Stock } from './op1-stock';
import { op1Hundekraft } from './op1-hundekraft';

/**
 * Every supported profile. Order matters: detection ties keep this order, and the first is the
 * default when nothing is recognised. Later firmware profiles are appended here.
 */
export const profiles: readonly DeviceProfile[] = [op1Stock, op1Hundekraft];

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

/**
 * Content detection after the library loads: runs every candidate's sniff over the loaded
 * patches' traits and returns the best one when it is not the active profile and is more
 * confident than the active profile's folder detection was. Null otherwise, and always null
 * without traits. A sniff that throws counts as confidence 0. The result is only a suggestion:
 * patch contents prove where sounds came from, not which firmware is installed now.
 */
export function suggestProfile(
  traits: readonly SoundTraits[],
  active: DeviceProfile,
  activeConfidence: number,
  candidates: readonly DeviceProfile[] = profiles,
): RankedProfile | null {
  if (traits.length === 0) return null;
  let best: RankedProfile | null = null;
  for (const profile of candidates) {
    if (!profile.sniff) continue;
    let result: DetectResult;
    try {
      result = profile.sniff(traits);
    } catch {
      continue;
    }
    if (result.confidence > 0 && (!best || result.confidence > best.result.confidence)) best = { profile, result };
  }
  if (!best || best.profile.id === active.id || best.result.confidence <= activeConfidence) return null;
  return best;
}
