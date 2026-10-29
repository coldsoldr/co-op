import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { createHash } from 'node:crypto';
import { mkdtemp, mkdir, readFile, readdir, rm, symlink, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import type { IncomingMessage, ServerResponse } from 'node:http';
import { buildPacks, formatIssues, loadPacks, packsMiddleware } from './packs-lib';
import { fixtureBytes } from '../test/fixture-bytes';
import { readPatch, withMeta } from '../src/core/codec/patch';
import type { Op1Meta } from '../src/core/codec/op1-meta';

const cluster = fixtureBytes('synth-cluster.aif');
const boombap = fixtureBytes('drum-boombap.aif');
const noMeta = fixtureBytes('no-metadata.aif');

const sha = (bytes: Uint8Array): string => createHash('sha256').update(bytes).digest('hex');
const NOW = new Date('2026-10-28T12:00:00.000Z');
const FIRMWARE = ['op1/stock'];

const manifest = (slug: string, patches: string[], extra: Record<string, unknown> = {}) => ({
  format: 1,
  name: `Pack ${slug}`,
  slug,
  author: 'co-op',
  description: 'A pack for tests.',
  license: 'CC0-1.0',
  firmware: ['op1/stock'],
  tags: ['test'],
  patches: patches.map((file) => ({ file })),
  ...extra,
});

let root: string;
let packsDir: string;

async function put(path: string, content: Uint8Array | string | object): Promise<void> {
  const full = join(packsDir, path);
  await mkdir(join(full, '..'), { recursive: true });
  const data = content instanceof Uint8Array || typeof content === 'string' ? content : JSON.stringify(content, null, 2);
  await writeFile(full, data);
}

async function goodPack(slug = 'good'): Promise<void> {
  await put(`${slug}/pack.json`, manifest(slug, ['synth/cluster.aif', 'drum/boombap.aif']));
  await put(`${slug}/synth/cluster.aif`, cluster);
  await put(`${slug}/drum/boombap.aif`, boombap);
  await put(`${slug}/README.md`, '# good\n');
}

beforeEach(async () => {
  root = await mkdtemp(join(tmpdir(), 'co-op-packs-'));
  packsDir = join(root, 'packs');
  await mkdir(packsDir);
});

afterEach(async () => {
  await rm(root, { recursive: true, force: true });
});

describe('loadPacks', () => {
  it('indexes a valid pack', async () => {
    await goodPack();
    const { issues, index } = await loadPacks(packsDir, { now: NOW, firmware: FIRMWARE });
    expect(issues).toEqual([]);
    expect(index).toEqual({
      format: 1,
      generatedAt: '2026-10-28T12:00:00.000Z',
      packs: [
        {
          slug: 'good',
          name: 'Pack good',
          author: 'co-op',
          description: 'A pack for tests.',
          license: 'CC0-1.0',
          firmware: ['op1/stock'],
          tags: ['test'],
          sizeBytes: cluster.length + boombap.length,
          hasReadme: true,
          patches: [
            { file: 'synth/cluster.aif', kind: 'synth', name: 'default', engine: 'cluster', sizeBytes: cluster.length, sha256: sha(cluster) },
            { file: 'drum/boombap.aif', kind: 'drum', name: 'boombap1', engine: 'drum', sizeBytes: boombap.length, sha256: sha(boombap) },
          ],
        },
      ],
    });
  });

  it('sorts packs by slug and ignores loose files in the packs folder', async () => {
    await goodPack('zeta');
    await goodPack('alpha');
    await put('README.md', 'about packs');
    const { issues, index } = await loadPacks(packsDir, { now: NOW, firmware: FIRMWARE });
    expect(issues).toEqual([]);
    expect(index.packs.map((p) => p.slug)).toEqual(['alpha', 'zeta']);
  });

  it('defaults to the registered profiles as known firmware', async () => {
    await goodPack();
    expect((await loadPacks(packsDir, { now: NOW })).issues).toEqual([]);
  });

  it('indexes a drum kit under its own drum engine', async () => {
    const kit = (type: string) => {
      const patch = readPatch(boombap);
      return withMeta(patch, { ...patch.meta, type, name: type } as Op1Meta).raw;
    };
    await put('kits/pack.json', manifest('kits', ['drum/dbox.aif', 'drum/gin.aif', 'drum/kit.aif'], { firmware: ['op1/hundekraft'] }));
    await put('kits/drum/dbox.aif', kit('dbox'));
    await put('kits/drum/gin.aif', kit('gin'));
    await put('kits/drum/kit.aif', boombap);
    const { issues, index } = await loadPacks(packsDir, { now: NOW });
    expect(issues).toEqual([]);
    expect(index.packs[0]?.patches.map((p) => [p.kind, p.engine])).toEqual([
      ['drum', 'dbox'],
      ['drum', 'gin'],
      ['drum', 'drum'],
    ]);
  });

  it('accepts op1/hundekraft as a registered firmware', async () => {
    await put('hk/pack.json', manifest('hk', ['synth/cluster.aif'], { firmware: ['op1/stock', 'op1/hundekraft'] }));
    await put('hk/synth/cluster.aif', cluster);
    const { issues, index } = await loadPacks(packsDir, { now: NOW });
    expect(issues).toEqual([]);
    expect(index.packs[0]?.firmware).toEqual(['op1/stock', 'op1/hundekraft']);
  });

  it('returns an empty index when there is no packs folder', async () => {
    const { issues, index } = await loadPacks(join(root, 'missing'), { now: NOW });
    expect(issues).toEqual([]);
    expect(index.packs).toEqual([]);
  });

  it('reports every broken pack and keeps the valid ones', async () => {
    await goodPack();
    await mkdir(join(packsDir, 'no-manifest', 'synth'), { recursive: true });
    await put('bad-json/pack.json', '{ "format": 1,');
    await put('wrong-slug/pack.json', manifest('other', ['synth/cluster.aif']));
    await put('wrong-slug/synth/cluster.aif', cluster);
    await put('extra/pack.json', manifest('extra', ['synth/cluster.aif']));
    await put('extra/synth/cluster.aif', cluster);
    await put('extra/synth/stray.aif', cluster);
    await put('not-audio/pack.json', manifest('not-audio', ['synth/text.aif']));
    await put('not-audio/synth/text.aif', 'hello');
    await put('wrong-kind/pack.json', manifest('wrong-kind', ['synth/kit.aif', 'drum/plain.aif']));
    await put('wrong-kind/synth/kit.aif', boombap);
    await put('wrong-kind/drum/plain.aif', noMeta);

    const { issues, index } = await loadPacks(packsDir, { now: NOW, firmware: FIRMWARE });
    expect(index.packs.map((p) => p.slug)).toEqual(['good']);
    const by = (pack: string) => issues.filter((i) => i.pack === pack).map((i) => `${i.file ?? ''}: ${i.message}`);
    expect(by('no-manifest')).toEqual(['pack.json: missing']);
    expect(by('bad-json')[0]).toMatch(/^pack\.json: not valid JSON/);
    expect(by('wrong-slug')).toEqual(['pack.json: slug "other" must equal the folder name "wrong-slug"']);
    expect(by('extra')).toEqual(['synth/stray.aif: not listed in pack.json; list it under "patches" or remove it']);
    expect(by('not-audio')).toEqual(['synth/text.aif: not a readable patch: not an AIFF file']);
    expect(by('wrong-kind')).toEqual([
      'synth/kit.aif: is a drum patch but sits in synth/',
      'drum/plain.aif: has no op-1 synth or drum metadata but sits in drum/',
    ]);
  });
});

describe('loadPacks extras', () => {
  it('carries per-patch notes into the index', async () => {
    await goodPack();
    const json = manifest('good', ['synth/cluster.aif', 'drum/boombap.aif']);
    json.patches[0] = { file: 'synth/cluster.aif', notes: 'soft pad' } as { file: string };
    await put('good/pack.json', json);
    const { index } = await loadPacks(packsDir, { now: NOW, firmware: FIRMWARE });
    expect(index.packs[0]?.patches.map((p) => p.notes)).toEqual(['soft pad', undefined]);
    expect(Object.keys(index.packs[0]?.patches[1] ?? {})).not.toContain('notes');
  });

  it('records whether a pack has a README', async () => {
    await goodPack();
    await rm(join(packsDir, 'good', 'README.md'));
    expect((await loadPacks(packsDir, { now: NOW, firmware: FIRMWARE })).index.packs[0]?.hasReadme).toBe(false);
  });

  it('ignores .DS_Store, Thumbs.db and desktop.ini and never publishes them', async () => {
    await goodPack();
    await put('good/.DS_Store', 'x');
    await put('good/synth/Thumbs.db', 'x');
    await put('good/drum/desktop.ini', 'x');
    expect((await loadPacks(packsDir, { now: NOW, firmware: FIRMWARE })).issues).toEqual([]);
    const out = join(root, 'dist');
    await buildPacks(packsDir, out, { now: NOW, firmware: FIRMWARE });
    expect((await readdir(join(out, 'packs/good'))).sort()).toEqual(['README.md', 'drum', 'pack.json', 'synth']);
    expect(await readdir(join(out, 'packs/good/synth'))).toEqual(['cluster.aif']);
    expect(await readdir(join(out, 'packs/good/drum'))).toEqual(['boombap.aif']);
  });
});

describe('loadPacks file checks', () => {
  it('accepts a pack.json that starts with a byte order mark', async () => {
    await goodPack();
    await put('good/pack.json', `\uFEFF${JSON.stringify(manifest('good', ['synth/cluster.aif', 'drum/boombap.aif']))}`);
    expect((await loadPacks(packsDir, { now: NOW, firmware: FIRMWARE })).issues).toEqual([]);
  });

  it('refuses a pack.json over 64 KB without parsing it', async () => {
    await put('big/pack.json', `${JSON.stringify(manifest('big', ['synth/a.aif']))}${' '.repeat(64 * 1024)}`);
    const { issues } = await loadPacks(packsDir, { now: NOW, firmware: FIRMWARE });
    expect(issues).toEqual([{ pack: 'big', file: 'pack.json', message: expect.stringMatching(/over the 64 KB \(65536 bytes\) limit$/) }]);
  });

  it('reports a symbolic link instead of following it', async (context) => {
    await goodPack();
    await put('outside.aif', cluster);
    try {
      await symlink(join(packsDir, 'outside.aif'), join(packsDir, 'good', 'synth', 'link.aif'), 'file');
    } catch (error) {
      if (['EPERM', 'EACCES'].includes((error as NodeJS.ErrnoException).code ?? '')) {
        context.skip('this system does not permit creating symbolic links');
      }
      throw error;
    }
    const { issues, index } = await loadPacks(packsDir, { now: NOW, firmware: FIRMWARE });
    expect(index.packs).toEqual([]);
    expect(issues).toEqual([{ pack: 'good', file: 'synth/link.aif', message: 'is a link; add the real file instead' }]);
  });
});

describe('formatIssues', () => {
  it('groups issues by pack', () => {
    const text = formatIssues([
      { pack: 'a', file: 'pack.json', message: 'one' },
      { pack: 'b', message: 'two' },
      { pack: 'a', file: 'synth/x.aif', message: 'three' },
    ]);
    expect(text).toBe(['a', '  pack.json: one', '  synth/x.aif: three', 'b', '  two', '', '3 issues in 2 packs'].join('\n'));
  });

  it('uses the singular for one issue', () => {
    expect(formatIssues([{ pack: 'a', message: 'x' }])).toMatch(/1 issue in 1 pack$/);
  });
});

describe('buildPacks', () => {
  it('copies every pack and writes index.json', async () => {
    await goodPack();
    const out = join(root, 'dist');
    await mkdir(join(out, 'packs', 'stale'), { recursive: true });
    const { issues } = await buildPacks(packsDir, out, { now: NOW, firmware: FIRMWARE });
    expect(issues).toEqual([]);
    expect((await readdir(join(out, 'packs'))).sort()).toEqual(['good', 'index.json']);
    expect(new Uint8Array(await readFile(join(out, 'packs/good/synth/cluster.aif')))).toEqual(cluster);
    expect(new Uint8Array(await readFile(join(out, 'packs/good/drum/boombap.aif')))).toEqual(boombap);
    expect(await readFile(join(out, 'packs/good/README.md'), 'utf8')).toBe('# good\n');
    const index = JSON.parse(await readFile(join(out, 'packs/index.json'), 'utf8'));
    expect(index).toEqual((await loadPacks(packsDir, { now: NOW, firmware: FIRMWARE })).index);
  });

  it('writes nothing when a pack is invalid', async () => {
    await goodPack();
    await put('broken/pack.json', manifest('broken', ['synth/missing.aif']));
    const out = join(root, 'dist');
    const { issues } = await buildPacks(packsDir, out, { now: NOW, firmware: FIRMWARE });
    expect(issues).toEqual([{ pack: 'broken', file: 'synth/missing.aif', message: 'listed in pack.json but missing' }]);
    await expect(readdir(join(out, 'packs'))).rejects.toThrow();
  });
});

describe('packsMiddleware', () => {
  interface Sent {
    status: number;
    headers: Record<string, string>;
    body: Buffer;
    next: boolean;
  }

  async function request(url: string): Promise<Sent> {
    const handle = packsMiddleware(packsDir, { base: '/co-op/', firmware: FIRMWARE, now: () => NOW });
    const sent: Sent = { status: 200, headers: {}, body: Buffer.alloc(0), next: false };
    await new Promise<void>((resolve) => {
      const res = {
        set statusCode(code: number) {
          sent.status = code;
        },
        setHeader(name: string, value: string) {
          sent.headers[name.toLowerCase()] = value;
        },
        end(body?: string | Uint8Array) {
          if (body !== undefined) sent.body = Buffer.from(body);
          resolve();
        },
      } as unknown as ServerResponse;
      handle({ url } as IncomingMessage, res, () => {
        sent.next = true;
        resolve();
      });
    });
    return sent;
  }

  it('serves the index built on the fly', async () => {
    await goodPack();
    const sent = await request('/co-op/packs/index.json');
    expect(sent.headers['content-type']).toBe('application/json');
    expect(JSON.parse(sent.body.toString('utf8'))).toEqual((await loadPacks(packsDir, { now: NOW, firmware: FIRMWARE })).index);
  });

  it('serves pack files byte for byte', async () => {
    await goodPack();
    const sent = await request('/co-op/packs/good/synth/cluster.aif');
    expect(sent.status).toBe(200);
    expect(new Uint8Array(sent.body)).toEqual(cluster);
  });

  it('answers 404 for a missing file or a path that leaves the packs folder', async () => {
    await goodPack();
    expect((await request('/co-op/packs/good/synth/nope.aif')).status).toBe(404);
    expect((await request('/co-op/packs/../package.json')).status).toBe(404);
    expect((await request('/co-op/packs/%2e%2e/%2e%2e/package.json')).status).toBe(404);
    expect((await request('/co-op/packs/good/synth')).status).toBe(404);
  });

  it('answers 404 for a drive-letter segment, raw or encoded', async () => {
    await goodPack();
    for (const path of ['C:', 'C%3A', 'c%3a', 'good/C:', 'C%3A%5CWindows%5Cwin.ini', 'C:/Windows/win.ini']) {
      expect((await request(`/co-op/packs/${path}`)).status).toBe(404);
    }
  });

  it('serves only pack.json, README.md and listed patches of valid packs', async () => {
    await goodPack();
    expect((await request('/co-op/packs/good/pack.json')).status).toBe(200);
    expect((await request('/co-op/packs/good/README.md')).status).toBe(200);
    // A file in a pack that fails validation is not served, even though it exists.
    await put('broken/pack.json', manifest('broken', ['synth/cluster.aif']));
    await put('broken/synth/cluster.aif', cluster);
    await put('broken/synth/stray.aif', cluster);
    expect((await request('/co-op/packs/broken/synth/cluster.aif')).status).toBe(404);
    expect((await request('/co-op/packs/broken/pack.json')).status).toBe(404);
    // A case variant of a listed path is not the listed path.
    expect((await request('/co-op/packs/good/synth/Cluster.aif')).status).toBe(404);
  });

  it('passes other requests on', async () => {
    expect((await request('/co-op/index.html')).next).toBe(true);
    expect((await request('/packs/index.json')).next).toBe(true);
  });
});
