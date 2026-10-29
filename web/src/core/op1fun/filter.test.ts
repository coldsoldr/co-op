import { describe, expect, it } from 'vitest';
import { filterOp1FunPacks, filterOp1FunPatches } from './filter';
import type { Op1FunPack, Op1FunPatch } from './types';

const patch = (id: string, name: string, extra: Partial<Op1FunPatch> = {}): Op1FunPatch => ({
  id,
  name,
  description: '',
  license: '',
  public: true,
  patchType: '',
  fileUrl: '',
  previewUrl: '',
  selfUrl: '',
  userId: '',
  ...extra,
});

const pack = (id: string, name: string, description = ''): Op1FunPack => ({
  id,
  name,
  description,
  public: true,
  patchIds: [],
  downloadUrl: '',
  userId: '',
});

describe('op1.fun filters', () => {
  const patches = [
    patch('1', 'Big Kick', { patchType: 'drum', license: 'CC BY' }),
    patch('2', 'Soft Pad', { patchType: 'synth', description: 'warm and wide' }),
  ];

  it('returns everything for an empty filter', () => {
    expect(filterOp1FunPatches(patches, '  ')).toEqual(patches);
  });

  it('matches every word over name, description, type and licence, ignoring case', () => {
    expect(filterOp1FunPatches(patches, 'kick').map((p) => p.id)).toEqual(['1']);
    expect(filterOp1FunPatches(patches, 'DRUM cc').map((p) => p.id)).toEqual(['1']);
    expect(filterOp1FunPatches(patches, 'warm synth').map((p) => p.id)).toEqual(['2']);
    expect(filterOp1FunPatches(patches, 'warm drum')).toEqual([]);
  });

  it('filters packs by name and description', () => {
    const packs = [pack('1', 'Drums', 'lofi kits'), pack('2', 'Keys')];
    expect(filterOp1FunPacks(packs, 'lofi').map((p) => p.id)).toEqual(['1']);
    expect(filterOp1FunPacks(packs, 'keys').map((p) => p.id)).toEqual(['2']);
    expect(filterOp1FunPacks(packs, '')).toHaveLength(2);
  });
});
