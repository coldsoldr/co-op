import { describe, it, expect } from 'vitest';
import { createHash } from 'node:crypto';
import type { PackIndex, PackIndexEntry } from '@co-op/shared/packs';
import {
  CatalogueError,
  compatibilityWarning,
  fetchIndex,
  fetchPatchBytes,
  filterPacks,
  installPatches,
} from './catalogue';
import { MemoryStore } from '../store/memory-store';
import { StagedStore } from '../library/changeset';
import { StoreError } from '../store/store';
import { fixtureBytes } from '../../../test/fixture-bytes';
import { expectSameBytes } from '../../../test/same-bytes';
import { nonCanonical } from '../../../test/non-canonical';

const cluster = fixtureBytes('synth-cluster.aif');
const boombap = fixtureBytes('drum-boombap.aif');
const sha = (bytes: Uint8Array): string => createHash('sha256').update(bytes).digest('hex');

const pack = (overrides: Partial<PackIndexEntry> = {}): PackIndexEntry => ({
  slug: 'test-tones',
  name: 'Test Tones',
  author: 'co-op',
  description: 'Plain sine patches for checking an install works.',
  license: 'CC0-1.0',
  firmware: ['op1/stock'],
  tags: ['utility'],
  sizeBytes: cluster.length + boombap.length,
  patches: [
    { file: 'synth/cluster.aif', kind: 'synth', name: 'default', engine: 'cluster', sizeBytes: cluster.length, sha256: sha(cluster) },
    { file: 'drum/boombap.aif', kind: 'drum', name: 'boombap1', engine: 'drum', sizeBytes: boombap.length, sha256: sha(boombap) },
  ],
  ...overrides,
});

const index = (packs: PackIndexEntry[] = [pack()]): PackIndex => ({ format: 1, generatedAt: '2026-10-28T00:00:00.000Z', packs });

/** A fetch that answers from a table of URL -> response body, and records every URL asked for. */
function fakeFetch(routes: Record<string, Uint8Array | string | { status: number }>) {
  const asked: string[] = [];
  const impl = async (input: string | URL | Request): Promise<Response> => {
    const url = String(input);
    asked.push(url);
    const route = routes[url];
    if (route === undefined) return new Response('not found', { status: 404 });
    if (typeof route === 'object' && 'status' in route) return new Response('error', { status: route.status });
    return new Response(route as BodyInit);
  };
  return { impl: impl as typeof fetch, asked };
}

describe('fetchIndex', () => {
  it('fetches packs/index.json below the base URL', async () => {
    const { impl, asked } = fakeFetch({ '/co-op/packs/index.json': JSON.stringify(index()) });
    expect(await fetchIndex('/co-op/', impl)).toEqual(index());
    expect(asked).toEqual(['/co-op/packs/index.json']);
  });

  it('adds a missing trailing slash to the base URL', async () => {
    const { impl } = fakeFetch({ 'https://example.org/co-op/packs/index.json': JSON.stringify(index()) });
    expect((await fetchIndex('https://example.org/co-op', impl)).packs).toHaveLength(1);
  });

  it('rejects an HTTP error', async () => {
    const { impl } = fakeFetch({ '/co-op/packs/index.json': { status: 500 } });
    await expect(fetchIndex('/co-op/', impl)).rejects.toThrow(CatalogueError);
    await expect(fetchIndex('/co-op/', impl)).rejects.toThrow(/HTTP 500/);
  });

  it('rejects a network failure', async () => {
    const impl = (async () => {
      throw new TypeError('Failed to fetch');
    }) as typeof fetch;
    await expect(fetchIndex('/co-op/', impl)).rejects.toThrow(/could not load the pack index: Failed to fetch/);
  });

  it('rejects a body that is not JSON', async () => {
    const { impl } = fakeFetch({ '/co-op/packs/index.json': '<html>' });
    await expect(fetchIndex('/co-op/', impl)).rejects.toThrow(/not valid JSON/);
  });

  it('rejects an unknown format or a missing pack list', async () => {
    for (const body of [{ ...index(), format: 2 }, { format: 1 }, [], null]) {
      const { impl } = fakeFetch({ '/co-op/packs/index.json': JSON.stringify(body) });
      await expect(fetchIndex('/co-op/', impl)).rejects.toThrow(/unsupported pack index/);
    }
  });
});

describe('filterPacks', () => {
  const drums = pack({
    slug: 'drum-kits',
    name: 'Dusty Kits',
    author: 'someone',
    description: 'Lo-fi drums.',
    license: 'CC-BY-4.0',
    firmware: ['op1/stock', 'op1/hundekraft'],
    tags: ['lofi', 'drums'],
    patches: [{ file: 'drum/a.aif', kind: 'drum', name: 'Snappy', engine: 'drum', sizeBytes: 1, sha256: 'x' }],
  });
  const all = index([pack(), drums]);
  const slugs = (filter: Parameters<typeof filterPacks>[1]) => filterPacks(all, filter).map((p) => p.slug);

  it('returns every pack for an empty filter', () => {
    expect(slugs({})).toEqual(['test-tones', 'drum-kits']);
    expect(slugs({ text: '   ' })).toEqual(['test-tones', 'drum-kits']);
  });

  it('matches every word of the text, ignoring case, in name, author, description, tags and patch names', () => {
    expect(slugs({ text: 'dusty' })).toEqual(['drum-kits']);
    expect(slugs({ text: 'SOMEONE' })).toEqual(['drum-kits']);
    expect(slugs({ text: 'sine' })).toEqual(['test-tones']);
    expect(slugs({ text: 'utility' })).toEqual(['test-tones']);
    expect(slugs({ text: 'snappy' })).toEqual(['drum-kits']);
    expect(slugs({ text: 'lo-fi drums' })).toEqual(['drum-kits']);
    expect(slugs({ text: 'dusty sine' })).toEqual([]);
  });

  it('filters by kind, firmware, licence and tag', () => {
    expect(slugs({ kind: 'synth' })).toEqual(['test-tones']);
    expect(slugs({ kind: 'drum' })).toEqual(['test-tones', 'drum-kits']);
    expect(slugs({ firmware: 'op1/hundekraft' })).toEqual(['drum-kits']);
    expect(slugs({ license: 'CC0-1.0' })).toEqual(['test-tones']);
    expect(slugs({ tag: 'drums' })).toEqual(['drum-kits']);
    expect(slugs({ kind: 'synth', license: 'CC-BY-4.0' })).toEqual([]);
  });
});

describe('fetchPatchBytes', () => {
  it('fetches the file and checks its hash', async () => {
    const { impl, asked } = fakeFetch({ '/co-op/packs/test-tones/synth/cluster.aif': cluster });
    expectSameBytes(await fetchPatchBytes('/co-op/', 'test-tones', 'synth/cluster.aif', sha(cluster), impl), cluster);
    expect(asked).toEqual(['/co-op/packs/test-tones/synth/cluster.aif']);
  });

  it('encodes each path segment', async () => {
    const { impl, asked } = fakeFetch({ '/co-op/packs/p/synth/a%20b%23.aif': cluster });
    await fetchPatchBytes('/co-op/', 'p', 'synth/a b#.aif', sha(cluster), impl);
    expect(asked).toEqual(['/co-op/packs/p/synth/a%20b%23.aif']);
  });

  it('accepts an upper-case hash', async () => {
    const { impl } = fakeFetch({ '/co-op/packs/test-tones/synth/cluster.aif': cluster });
    await expect(fetchPatchBytes('/co-op/', 'test-tones', 'synth/cluster.aif', sha(cluster).toUpperCase(), impl)).resolves.toHaveLength(
      cluster.length,
    );
  });

  it('throws CatalogueError when the bytes do not match the hash', async () => {
    const { impl } = fakeFetch({ '/co-op/packs/test-tones/synth/cluster.aif': boombap });
    const fetching = fetchPatchBytes('/co-op/', 'test-tones', 'synth/cluster.aif', sha(cluster), impl);
    await expect(fetching).rejects.toThrow(CatalogueError);
    await expect(fetchPatchBytes('/co-op/', 'test-tones', 'synth/cluster.aif', sha(cluster), impl)).rejects.toThrow(
      /test-tones\/synth\/cluster\.aif does not match the pack index/,
    );
  });

  it('throws CatalogueError on an HTTP error', async () => {
    const { impl } = fakeFetch({});
    await expect(fetchPatchBytes('/co-op/', 'test-tones', 'synth/cluster.aif', sha(cluster), impl)).rejects.toThrow(
      /could not download test-tones\/synth\/cluster\.aif: HTTP 404/,
    );
  });
});

describe('installPatches', () => {
  const files = [
    { file: 'synth/cluster.aif', bytes: cluster },
    { file: 'drum/boombap.aif', bytes: boombap },
  ];

  it('writes raw bytes into <kind>/<slug> by default', async () => {
    const store = new MemoryStore({ 'synth/user/x.aif': cluster });
    const { installed } = await installPatches(store, 'op1/stock', 'test-tones', files);
    expect(installed).toEqual(['synth/test-tones/cluster.aif', 'drum/test-tones/boombap.aif']);
    const snapshot = store.snapshot();
    expect(Object.keys(snapshot)).toEqual(['drum/test-tones/boombap.aif', 'synth/test-tones/cluster.aif', 'synth/user/x.aif']);
    expectSameBytes(snapshot['synth/test-tones/cluster.aif'] as Uint8Array, cluster);
    expectSameBytes(snapshot['drum/test-tones/boombap.aif'] as Uint8Array, boombap);
  });

  it('writes the bytes untouched even when they are not canonical', async () => {
    const odd = nonCanonical(cluster);
    const store = new MemoryStore({});
    await installPatches(store, 'op1/stock', 'p', [{ file: 'synth/odd.aif', bytes: odd }]);
    expectSameBytes(store.snapshot()['synth/p/odd.aif'] as Uint8Array, odd);
  });

  it('takes a free name, ignoring case, when one is used', async () => {
    const store = new MemoryStore({ 'synth/test-tones/Cluster.AIF': boombap, 'synth/test-tones/cluster-2.aif': boombap });
    const { installed } = await installPatches(store, 'op1/stock', 'test-tones', [files[0] as (typeof files)[0]]);
    expect(installed).toEqual(['synth/test-tones/cluster-3.aif']);
    expectSameBytes(store.snapshot()['synth/test-tones/Cluster.AIF'] as Uint8Array, boombap);
  });

  it('reuses a target folder that differs only in case', async () => {
    const store = new MemoryStore({ 'Synth/Test-Tones/a.aif': boombap });
    const { installed } = await installPatches(store, 'op1/stock', 'test-tones', [files[0] as (typeof files)[0]]);
    expect(installed).toEqual(['Synth/Test-Tones/cluster.aif']);
  });

  it('gives two files with one name in the same install different names', async () => {
    const store = new MemoryStore({});
    const { installed } = await installPatches(store, 'op1/stock', 'p', [
      { file: 'synth/a.aif', bytes: cluster },
      { file: 'synth/A.aif', bytes: boombap },
    ]);
    expect(installed).toEqual(['synth/p/a.aif', 'synth/p/A-2.aif']);
  });

  it('uses the target folder the caller chooses', async () => {
    const store = new MemoryStore({});
    const { installed } = await installPatches(store, 'op1/stock', 'test-tones', files, (kind) =>
      kind === 'synth' ? 'synth/tones' : 'drum/user',
    );
    expect(installed).toEqual(['synth/tones/cluster.aif', 'drum/user/boombap.aif']);
  });

  it('stages installs on a read-only source through a StagedStore', async () => {
    const base = new MemoryStore({ 'synth/user/x.aif': cluster }, { write: false });
    const staged = new StagedStore(base);
    await installPatches(staged, 'op1/stock', 'test-tones', files);
    expect(staged.changes().map((c) => `${c.op} ${c.path}`)).toEqual([
      'write drum/test-tones/boombap.aif',
      'write synth/test-tones/cluster.aif',
    ]);
    expect(Object.keys(base.snapshot())).toEqual(['synth/user/x.aif']);
  });

  it('refuses a read-only store', async () => {
    const store = new MemoryStore({}, { write: false });
    await expect(installPatches(store, 'op1/stock', 'test-tones', files)).rejects.toThrow(StoreError);
  });

  it('refuses an unknown profile, a file outside synth/ or drum/, and a bad file name before writing', async () => {
    const store = new MemoryStore({});
    await expect(installPatches(store, 'op1/mystery', 'p', files)).rejects.toThrow(/unknown device profile "op1\/mystery"/);
    await expect(installPatches(store, 'op1/stock', 'p', [...files, { file: 'tape/a.aif', bytes: cluster }])).rejects.toThrow(
      /tape\/a\.aif is not in synth\/ or drum\//,
    );
    await expect(installPatches(store, 'op1/stock', 'p', [...files, { file: 'synth/a?.aif', bytes: cluster }])).rejects.toThrow(
      /cannot hold/,
    );
    await expect(installPatches(store, 'op1/stock', 'p', [...files, { file: 'synth/x/a.aif', bytes: cluster }])).rejects.toThrow(
      CatalogueError,
    );
    expect(store.snapshot()).toEqual({});
  });
});

describe('compatibilityWarning', () => {
  it('is null when the profile is listed', () => {
    expect(compatibilityWarning(pack(), 'op1/stock')).toBeNull();
  });

  it('names the pack, its firmware and the device when it is not', () => {
    const warning = compatibilityWarning(pack({ firmware: ['op1/hundekraft'] }), 'op1/stock');
    expect(warning).toBe(
      'Test Tones is made for op1/hundekraft, not OP-1 (stock firmware). Its patches may not load or may sound different.',
    );
  });

  it('uses profile labels where they are known', () => {
    expect(compatibilityWarning(pack(), 'op1/other')).toBe(
      'Test Tones is made for OP-1 (stock firmware), not op1/other. Its patches may not load or may sound different.',
    );
  });
});
