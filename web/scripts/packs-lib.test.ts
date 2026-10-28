import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { createHash } from 'node:crypto';
import { mkdtemp, mkdir, readFile, readdir, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import type { IncomingMessage, ServerResponse } from 'node:http';
import { buildPacks, formatIssues, loadPacks, packsMiddleware } from './packs-lib';
import { fixtureBytes } from '../test/fixture-bytes';

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

  it('passes other requests on', async () => {
    expect((await request('/co-op/index.html')).next).toBe(true);
    expect((await request('/packs/index.json')).next).toBe(true);
  });
});
