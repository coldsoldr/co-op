import { describe, it, expect } from 'vitest';
import { MemoryStore } from '../store/memory-store';
import { OP1_FLASH_BYTES, driveUsedBytes, freePercent } from './drive-space';

describe('driveUsedBytes', () => {
  it('adds up every file on the drive, hidden ones and all folders included', async () => {
    const store = new MemoryStore({
      'synth/user/a.aif': new Uint8Array(10),
      'tape/track_1.aif': new Uint8Array(100),
      'drum/.hidden': new Uint8Array(1),
    });
    expect(await driveUsedBytes(store)).toBe(111);
  });
});

describe('freePercent', () => {
  it('rounds to a whole percent of the flash', () => {
    expect(freePercent(0)).toBe(100);
    expect(freePercent(OP1_FLASH_BYTES / 4)).toBe(75);
  });

  it('stays in range when the files add up to more than the flash', () => {
    expect(freePercent(OP1_FLASH_BYTES * 2)).toBe(0);
  });
});
