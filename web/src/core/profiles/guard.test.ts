import { describe, it, expect } from 'vitest';
import { findUnsupported } from './guard';
import { op1Stock } from './op1-stock';
import { op1Hundekraft } from './op1-hundekraft';
import { readPatch, withMeta } from '../codec/patch';
import type { Op1Meta } from '../codec/op1-meta';
import { fixtureBytes } from '../../../test/fixture-bytes';
import { SYNTH_META } from '../../../test/fixtures/meta';

const cluster = fixtureBytes('synth-cluster.aif');
const supersaw = withMeta(readPatch(cluster), { ...SYNTH_META, type: 'supersaw' } as Op1Meta).raw;

describe('findUnsupported', () => {
  it('lists the patches the profile marks unsupported, with their reasons', () => {
    const files = [
      { path: 'synth/a/cluster.aif', bytes: cluster },
      { path: 'synth/a/saw.aif', bytes: supersaw },
      { path: 'synth/a/plain.aif', bytes: fixtureBytes('no-metadata.aif') },
    ];
    expect(findUnsupported(files, op1Stock)).toEqual([
      {
        path: 'synth/a/saw.aif',
        reason: 'engine "supersaw" is not part of this firmware; may stop a stock OP-1 at boot',
      },
      { path: 'synth/a/plain.aif', reason: 'file has no op-1 metadata' },
    ]);
  });

  it('judges by the profile it is given', () => {
    expect(findUnsupported([{ path: 'saw.aif', bytes: supersaw }], op1Hundekraft)).toEqual([]);
  });

  it('skips files that are not patches at all', () => {
    expect(findUnsupported([{ path: 'notes.txt', bytes: new TextEncoder().encode('hello') }], op1Stock)).toEqual([]);
  });
});
