import { describe, expect, it } from 'vitest';
import { detect } from './useDevice';
import { MemoryListing } from '../core/store/memory-listing';

const STOCK_DRIVE = { synth: {}, drum: {}, tape: {} };
const OPUS_DRIVE = { ...STOCK_DRIVE, mods: { 'x.opmod': 'file' as const } };

describe('detect', () => {
  it('picks the folder detection when nothing is remembered', async () => {
    const found = await detect(new MemoryListing(STOCK_DRIVE), null);
    expect(found.profile.id).toBe('op1/stock');
    expect(found.detection).toMatch(/^detected: /);
    expect(found.confidence).toBe(0.9);
  });

  it('uses the remembered profile over a weaker folder detection', async () => {
    const found = await detect(new MemoryListing(STOCK_DRIVE), 'op1/hundekraft');
    expect(found.profile.id).toBe('op1/hundekraft');
    expect(found.detection).toBe('remembered from last time');
    // The remembered profile's own folder confidence, so the content sniff still runs.
    expect(found.confidence).toBe(0.85);
  });

  it('uses the remembered profile when it is also the one detected', async () => {
    const found = await detect(new MemoryListing(OPUS_DRIVE), 'op1/opus');
    expect(found.profile.id).toBe('op1/opus');
    expect(found.detection).toBe('remembered from last time');
    expect(found.confidence).toBe(0.97);
  });

  it('lets a proving folder detection override the remembered profile', async () => {
    const found = await detect(new MemoryListing(OPUS_DRIVE), 'op1/hundekraft');
    expect(found.profile.id).toBe('op1/opus');
    expect(found.detection).toBe(
      'detected: found mods folder with opus plugins (you chose OP-1 (hundekraft) last time)',
    );
    expect(found.confidence).toBe(0.97);
  });

  it('uses the remembered profile on a drive it does not recognise', async () => {
    const found = await detect(new MemoryListing({ Music: {} }), 'op1/hundekraft');
    expect(found.profile.id).toBe('op1/hundekraft');
    expect(found.detection).toBe('remembered from last time');
    expect(found.confidence).toBe(0);
  });

  it('ignores a remembered id that is not in the registry', async () => {
    const found = await detect(new MemoryListing(STOCK_DRIVE), 'op1/gone');
    expect(found.profile.id).toBe('op1/stock');
    expect(found.detection).toMatch(/^detected: /);
  });
});
