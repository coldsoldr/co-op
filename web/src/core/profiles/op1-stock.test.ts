import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { op1Stock, STOCK_SYNTH_ENGINES } from './op1-stock';
import { MemoryListing } from '../store/memory-listing';
import { readPatch, withMeta } from '../codec/patch';
import { SYNTH_META } from '../../../test/fixtures/meta';

const fixture = (name: string) =>
  readPatch(new Uint8Array(readFileSync(fileURLToPath(new URL(`../../../test/fixtures/${name}`, import.meta.url)))));

describe('op1/stock detection', () => {
  it('is confident when synth, drum and tape folders exist', async () => {
    const root = new MemoryListing({ album: {}, drum: {}, synth: {}, tape: {} });
    const result = await op1Stock.detect(root);
    expect(result.confidence).toBeGreaterThanOrEqual(0.9);
    expect(result.reason).toMatch(/synth, drum and tape/);
  });

  it('ignores folder name case', async () => {
    const root = new MemoryListing({ DRUM: {}, SYNTH: {}, TAPE: {} });
    expect((await op1Stock.detect(root)).confidence).toBeGreaterThanOrEqual(0.9);
  });

  it('is unsure with only two of the folders', async () => {
    const root = new MemoryListing({ drum: {}, synth: {} });
    expect((await op1Stock.detect(root)).confidence).toBe(0.5);
  });

  it('rejects an unrelated folder', async () => {
    const root = new MemoryListing({ Documents: {}, Music: {} });
    expect((await op1Stock.detect(root)).confidence).toBe(0);
  });
});

describe('op1/stock compatibility', () => {
  it('accepts stock synth and drum patches', () => {
    expect(op1Stock.compatibility(fixture('synth-cluster.aif'))).toEqual({ level: 'ok' });
    expect(op1Stock.compatibility(fixture('drum-boombap.aif'))).toEqual({ level: 'ok' });
  });

  it('warns on an engine stock firmware does not have', () => {
    const patch = withMeta(fixture('synth-cluster.aif'), { ...SYNTH_META, type: 'wavetable' });
    const compat = op1Stock.compatibility(patch);
    expect(compat.level).toBe('warn');
    expect(compat.reason).toMatch(/wavetable/);
  });

  it('marks files without op-1 metadata unsupported', () => {
    expect(op1Stock.compatibility(fixture('no-metadata.aif')).level).toBe('unsupported');
  });

  it('knows every stock engine', () => {
    expect(STOCK_SYNTH_ENGINES).toContain('cluster');
    expect(STOCK_SYNTH_ENGINES).toContain('sampler');
    expect(STOCK_SYNTH_ENGINES).toContain('iter');
  });
});

describe('op1/stock layout', () => {
  it('scans synth and drum as patch roots', () => {
    expect(op1Stock.layout.patchRoots).toEqual([
      { path: 'synth', kind: 'synth' },
      { path: 'drum', kind: 'drum' },
    ]);
  });
});
