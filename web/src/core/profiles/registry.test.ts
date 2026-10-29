import { describe, it, expect } from 'vitest';
import { profiles, getProfile, detectProfiles, runDetectors } from './registry';
import type { DeviceProfile, DetectResult } from './types';
import { MemoryListing } from '../store/memory-listing';

/** A minimal profile whose detector returns a fixed result or throws. */
function fakeProfile(id: string, detect: () => Promise<DetectResult>): DeviceProfile {
  return {
    id,
    device: 'op1',
    firmware: id,
    label: id,
    layout: { patchRoots: [], otherRoots: [] },
    patchKinds: [],
    detect,
    supports: { synthEngines: [], drumEngines: [], fx: [], lfo: [] },
    compatibility: () => ({ level: 'ok' }),
  };
}

const fixed = (id: string, confidence: number) =>
  fakeProfile(id, async () => ({ confidence, reason: `fixed ${confidence}` }));

describe('profile registry', () => {
  it('lists stock first, then hundekraft', () => {
    expect(profiles.map((p) => p.id)).toEqual(['op1/stock', 'op1/hundekraft']);
  });

  it('looks a profile up by id', () => {
    expect(getProfile('op1/stock')?.label).toBe('OP-1 (stock firmware)');
    expect(getProfile('nope')).toBeUndefined();
  });

  it('ranks profiles by detection confidence', async () => {
    const root = new MemoryListing({ drum: {}, synth: {}, tape: {} });
    const ranked = await detectProfiles(root);
    expect(ranked.map((r) => r.profile.id)).toEqual(['op1/stock', 'op1/hundekraft']);
    expect(ranked[0]?.result.confidence).toBeGreaterThan(ranked[1]?.result.confidence ?? 1);
  });

  it('returns an empty ranking when nothing matches', async () => {
    expect(await detectProfiles(new MemoryListing({ Music: {} }))).toEqual([]);
  });

  it('ranks injected candidates highest confidence first', async () => {
    const ranked = await detectProfiles(new MemoryListing({}), [fixed('low', 0.3), fixed('high', 0.8)]);
    expect(ranked.map((r) => r.profile.id)).toEqual(['high', 'low']);
  });

  it('keeps candidate order on equal confidence', async () => {
    const ranked = await detectProfiles(new MemoryListing({}), [
      fixed('first', 0.5),
      fixed('second', 0.5),
      fixed('third', 0.5),
    ]);
    expect(ranked.map((r) => r.profile.id)).toEqual(['first', 'second', 'third']);
  });

  it('isolates a throwing detector so the others still rank', async () => {
    const broken = fakeProfile('broken', async () => {
      throw new Error('boom');
    });
    const ranked = await detectProfiles(new MemoryListing({}), [broken, fixed('fine', 0.5)]);
    expect(ranked.map((r) => r.profile.id)).toEqual(['fine']);
  });

  it('reports a failed detector as confidence 0 with the error message', async () => {
    const broken = fakeProfile('broken', async () => {
      throw new Error('boom');
    });
    const results = await runDetectors(new MemoryListing({}), [broken, fixed('fine', 0.5)]);
    expect(results.map((r) => r.result)).toEqual([
      { confidence: 0, reason: 'detector failed: boom' },
      { confidence: 0.5, reason: 'fixed 0.5' },
    ]);
  });

  it('isolates a detector that throws synchronously', async () => {
    const broken = fakeProfile('broken', () => {
      throw new Error('sync boom');
    });
    const results = await runDetectors(new MemoryListing({}), [broken, fixed('fine', 0.5)]);
    expect(results[0]?.result.confidence).toBe(0);
    expect(results[0]?.result.reason).toMatch(/detector failed/);
    expect(results[1]?.result).toEqual({ confidence: 0.5, reason: 'fixed 0.5' });
    const ranked = await detectProfiles(new MemoryListing({}), [broken, fixed('fine', 0.5)]);
    expect(ranked.map((r) => r.profile.id)).toEqual(['fine']);
  });
});
