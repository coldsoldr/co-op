import { describe, it, expect } from 'vitest';
import { profiles, getProfile, detectProfiles } from './registry';
import { MemoryListing } from '../store/memory-listing';

describe('profile registry', () => {
  it('lists the stock OP-1 profile', () => {
    expect(profiles.map((p) => p.id)).toEqual(['op1/stock']);
  });

  it('looks a profile up by id', () => {
    expect(getProfile('op1/stock')?.label).toBe('OP-1 (stock firmware)');
    expect(getProfile('nope')).toBeUndefined();
  });

  it('ranks profiles by detection confidence', async () => {
    const root = new MemoryListing({ drum: {}, synth: {}, tape: {} });
    const ranked = await detectProfiles(root);
    expect(ranked[0]?.profile.id).toBe('op1/stock');
    expect(ranked[0]?.result.confidence).toBeGreaterThan(0);
  });

  it('returns an empty ranking when nothing matches', async () => {
    expect(await detectProfiles(new MemoryListing({ Music: {} }))).toEqual([]);
  });
});
