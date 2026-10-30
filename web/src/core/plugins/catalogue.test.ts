import { describe, it, expect } from 'vitest';
import { createHash } from 'node:crypto';
import type { PluginIndex, PluginIndexEntry } from '@co-op/shared/plugins';
import { CatalogueError } from '../catalogue/catalogue';
import { fetchManual, fetchPluginBytes, fetchPluginIndex, filterPlugins, groupByAuthor, pluginFileUrl, pluginFolderUrl } from './catalogue';

const knl = new Uint8Array([0x4b, 0x4e, 0x4c, 0x00, 1, 2, 3, 4]);
const sha = (bytes: Uint8Array): string => createHash('sha256').update(bytes).digest('hex');

const plugin = (overrides: Partial<PluginIndexEntry> = {}): PluginIndexEntry => ({
  id: 'rings',
  name: 'rings',
  firmware: 'op1/hundekraft',
  kind: 'synth',
  author: 'coldsoldr',
  version: '0.1',
  description: 'Mutable Instruments Rings as a synth engine.',
  license: 'GPL-3.0-or-later',
  file: 'rings.knl',
  screenshot: 'rings/screenshot.png',
  manual: 'rings/manual.md',
  source: 'https://github.com/coldsoldr/te-op1-cfw',
  requires: 'hundekraft 3.0.3',
  credits: 'Port of Mutable Instruments Rings',
  tags: ['synth', 'mutable-instruments'],
  sizeBytes: knl.length,
  sha256: sha(knl),
  savedName: 'rings',
  ...overrides,
});

const index = (plugins: PluginIndexEntry[] = [plugin()]): PluginIndex => ({
  format: 1,
  generatedAt: '2026-10-29T00:00:00.000Z',
  plugins,
});

type Route = Uint8Array | string | { status: number };

/** As in the pack catalogue tests: answers from a table of URL -> body (a list answers in turn). */
function fakeFetch(routes: Record<string, Route | Route[]>) {
  const asked: string[] = [];
  const modes: (RequestCache | undefined)[] = [];
  const served = new Map<string, number>();
  const impl = async (input: string | URL | Request, init?: RequestInit): Promise<Response> => {
    const url = String(input);
    asked.push(url);
    modes.push(init?.cache);
    const entry = routes[url];
    const n = served.get(url) ?? 0;
    served.set(url, n + 1);
    const route = Array.isArray(entry) ? entry[Math.min(n, entry.length - 1)] : entry;
    if (route === undefined) return new Response('not found', { status: 404 });
    if (typeof route === 'object' && 'status' in route) return new Response('error', { status: route.status });
    return new Response(route as BodyInit);
  };
  return { impl: impl as typeof fetch, asked, modes };
}

const INDEX_URL = '/co-op/plugins/index.json';

describe('fetchPluginIndex', () => {
  it('fetches plugins/index.json below the base URL, revalidating it', async () => {
    const { impl, asked, modes } = fakeFetch({ [INDEX_URL]: JSON.stringify(index()) });
    expect(await fetchPluginIndex('/co-op/', impl)).toEqual(index());
    expect(asked).toEqual([INDEX_URL]);
    expect(modes).toEqual(['no-cache']);
  });

  it('adds the slash a base URL lacks', async () => {
    const { impl, asked } = fakeFetch({ [INDEX_URL]: JSON.stringify(index()) });
    await fetchPluginIndex('/co-op', impl);
    expect(asked).toEqual([INDEX_URL]);
  });

  it('accepts an index without the optional fields', async () => {
    const { source: _s, requires: _r, credits: _c, savedName: _n, ...bare } = plugin();
    const { impl } = fakeFetch({ [INDEX_URL]: JSON.stringify(index([bare])) });
    expect((await fetchPluginIndex('/co-op/', impl)).plugins).toEqual([bare]);
  });

  it('reports HTTP errors, bad JSON and an unknown format', async () => {
    await expect(fetchPluginIndex('/co-op/', fakeFetch({ [INDEX_URL]: { status: 500 } }).impl)).rejects.toThrow(
      /could not load the plugin index: HTTP 500/,
    );
    await expect(fetchPluginIndex('/co-op/', fakeFetch({ [INDEX_URL]: '{' }).impl)).rejects.toThrow(/not valid JSON/);
    await expect(
      fetchPluginIndex('/co-op/', fakeFetch({ [INDEX_URL]: JSON.stringify({ ...index(), format: 2 }) }).impl),
    ).rejects.toThrow(/unsupported plugin index/);
  });

  it('refuses malformed entries', async () => {
    const bad: unknown[] = [
      { ...plugin(), name: 3 },
      { ...plugin(), kind: 'drum' },
      { ...plugin(), firmware: 'op1/stock' },
      { ...plugin(), sha256: 'abc' },
      { ...plugin(), sizeBytes: '8' },
      { ...plugin(), tags: [1] },
      { ...plugin(), source: 42 },
      // Paths outside the plugin's own folder, or a file that is not a bare name.
      { ...plugin(), screenshot: 'grids/screenshot.png' },
      { ...plugin(), screenshot: 'rings/../x.png' },
      { ...plugin(), manual: 'rings/sub/manual.md' },
      { ...plugin(), file: '../rings.knl' },
      { ...plugin(), id: '../x' },
      // A source link must be https.
      { ...plugin(), source: 'javascript:alert(1)' },
    ];
    for (const entry of bad) {
      const { impl } = fakeFetch({ [INDEX_URL]: JSON.stringify({ ...index(), plugins: [entry] }) });
      await expect(fetchPluginIndex('/co-op/', impl), JSON.stringify(entry)).rejects.toThrow(/plugin index entry 1 is malformed/);
    }
  });

  it('throws CatalogueError', async () => {
    await expect(fetchPluginIndex('/co-op/', fakeFetch({}).impl)).rejects.toBeInstanceOf(CatalogueError);
  });
});

describe('filterPlugins', () => {
  const rings = plugin();
  const echo = plugin({
    id: 'tape_echo',
    name: 'tape echo',
    kind: 'fx',
    description: 'A tape delay.',
    tags: ['effect', 'delay', 'tape'],
    credits: 'After the Space Echo',
  });
  const opus = plugin({
    id: 'chorus.x',
    name: 'opus chorus',
    firmware: 'op1/opus',
    kind: 'fx',
    tags: ['effect'],
    author: 'someone',
    description: 'A chorus.',
    credits: 'Own work',
  });
  const all = index([rings, echo, opus]);
  const ids = (filter: Parameters<typeof filterPlugins>[1]): string[] => filterPlugins(all, filter).map((p) => p.id);

  it('returns everything for an empty filter, in index order', () => {
    expect(ids({})).toEqual(['rings', 'tape_echo', 'chorus.x']);
  });

  it('filters by kind, firmware and tag', () => {
    expect(ids({ kind: 'fx' })).toEqual(['tape_echo', 'chorus.x']);
    expect(ids({ firmware: 'op1/opus' })).toEqual(['chorus.x']);
    expect(ids({ tag: 'delay' })).toEqual(['tape_echo']);
    expect(ids({ kind: 'fx', firmware: 'op1/hundekraft' })).toEqual(['tape_echo']);
  });

  it('matches every word of the text, ignoring case, in name, id, author, description, tags and credits', () => {
    expect(ids({ text: 'TAPE' })).toEqual(['tape_echo']);
    expect(ids({ text: 'space echo' })).toEqual(['tape_echo']);
    expect(ids({ text: 'someone' })).toEqual(['chorus.x']);
    expect(ids({ text: 'mutable rings' })).toEqual(['rings']);
    expect(ids({ text: 'tape_echo' })).toEqual(['tape_echo']);
    expect(ids({ text: 'rings delay' })).toEqual([]);
    expect(ids({ text: '   ' })).toEqual(['rings', 'tape_echo', 'chorus.x']);
  });
});

describe('groupByAuthor', () => {
  it('sorts authors ignoring case and keeps each author\'s plugins in order', () => {
    const groups = groupByAuthor([
      plugin({ id: 'b', author: 'zed' }),
      plugin({ id: 'a', author: 'Amy' }),
      plugin({ id: 'c', author: 'zed' }),
      plugin({ id: 'd', author: 'bob' }),
    ]);
    expect(groups.map((g) => [g.author, g.plugins.map((p) => p.id)])).toEqual([
      ['Amy', ['a']],
      ['bob', ['d']],
      ['zed', ['b', 'c']],
    ]);
  });
});

describe('plugin URLs', () => {
  it('builds folder and file URLs below the base, encoding each segment', () => {
    expect(pluginFolderUrl('/co-op/', 'rings')).toBe('/co-op/plugins/rings/');
    expect(pluginFileUrl('/co-op', 'rings/screenshot.png')).toBe('/co-op/plugins/rings/screenshot.png');
    expect(pluginFileUrl('/co-op/', 'a b/c#d.png')).toBe('/co-op/plugins/a%20b/c%23d.png');
  });
});

describe('fetchPluginBytes', () => {
  const url = '/co-op/plugins/rings/rings.knl';

  it('downloads the plugin file and checks its hash and size', async () => {
    const { impl, asked } = fakeFetch({ [url]: knl });
    expect(await fetchPluginBytes('/co-op/', plugin(), impl)).toEqual(knl);
    expect(asked).toEqual([url]);
  });

  it('retries a mismatch once bypassing the cache, then gives up', async () => {
    const stale = new Uint8Array([9, 9, 9]);
    const retried = fakeFetch({ [url]: [stale, knl] });
    expect(await fetchPluginBytes('/co-op/', plugin(), retried.impl)).toEqual(knl);
    expect(retried.modes).toEqual([undefined, 'no-cache']);

    const broken = fakeFetch({ [url]: stale });
    await expect(fetchPluginBytes('/co-op/', plugin(), broken.impl)).rejects.toThrow(
      /rings\/rings.knl does not match the plugin index/,
    );
    expect(broken.asked).toHaveLength(2);
  });

  it('refuses bytes of the right hash but the wrong size in the index', async () => {
    const { impl } = fakeFetch({ [url]: knl });
    await expect(fetchPluginBytes('/co-op/', plugin({ sizeBytes: knl.length + 1 }), impl)).rejects.toThrow(
      /does not match the plugin index/,
    );
  });

  it('refuses an entry whose file is not a bare name or whose hash is malformed, before fetching', async () => {
    const { impl, asked } = fakeFetch({});
    await expect(fetchPluginBytes('/co-op/', plugin({ file: '../x.knl' }), impl)).rejects.toThrow(/not a plugin file name/);
    await expect(fetchPluginBytes('/co-op/', plugin({ sha256: '' }), impl)).rejects.toThrow(/SHA-256/);
    expect(asked).toEqual([]);
  });

  it('reports a failed download', async () => {
    await expect(fetchPluginBytes('/co-op/', plugin(), fakeFetch({ [url]: { status: 404 } }).impl)).rejects.toThrow(
      /could not download rings\/rings.knl: HTTP 404/,
    );
  });
});

describe('fetchManual', () => {
  const url = '/co-op/plugins/rings/manual.md';

  it('fetches the manual as text', async () => {
    const { impl, asked } = fakeFetch({ [url]: '# rings\n' });
    expect(await fetchManual('/co-op/', plugin(), impl)).toBe('# rings\n');
    expect(asked).toEqual([url]);
  });

  it('refuses a manual over the size limit', async () => {
    const { impl } = fakeFetch({ [url]: 'x'.repeat(64 * 1024 + 1) });
    await expect(fetchManual('/co-op/', plugin(), impl)).rejects.toThrow(/too large/);
  });

  it('refuses a manual path outside the plugin folder', async () => {
    const { impl, asked } = fakeFetch({});
    await expect(fetchManual('/co-op/', plugin({ manual: 'grids/manual.md' }), impl)).rejects.toThrow(/not in the plugin folder/);
    expect(asked).toEqual([]);
  });
});
