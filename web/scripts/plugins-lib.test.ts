import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { createHash } from 'node:crypto';
import { mkdtemp, mkdir, readFile, readdir, rm, symlink, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import type { IncomingMessage, ServerResponse } from 'node:http';
import { buildPlugins, formatIssues, loadPlugins, pluginsMiddleware } from './plugins-lib';
import { knlBytes, opmodBytes, pngBytes } from '../test/plugin-bytes';

const sha = (bytes: Uint8Array): string => createHash('sha256').update(bytes).digest('hex');
const NOW = new Date('2026-10-29T12:00:00.000Z');

const manifest = (id: string, extra: Record<string, unknown> = {}) => ({
  format: 1,
  id,
  name: `Plugin ${id}`,
  firmware: 'op1/hundekraft',
  kind: 'fx',
  author: 'coldsoldr',
  version: '1.0',
  description: 'A plugin for tests.',
  license: 'GPL-3.0-or-later',
  file: `${id}.knl`,
  screenshot: 'screenshot.png',
  manual: 'manual.md',
  tags: ['test'],
  ...extra,
});

let root: string;
let pluginsDir: string;

async function put(path: string, content: Uint8Array | string | object): Promise<void> {
  const full = join(pluginsDir, path);
  await mkdir(join(full, '..'), { recursive: true });
  const data = content instanceof Uint8Array || typeof content === 'string' ? content : JSON.stringify(content, null, 2);
  await writeFile(full, data);
}

const drive = knlBytes({ name: 'drive' });
const shot = pngBytes();

async function goodPlugin(id = 'drive', bytes = id === 'drive' ? drive : knlBytes({ name: id })): Promise<void> {
  await put(`${id}/plugin.json`, manifest(id));
  await put(`${id}/${id}.knl`, bytes);
  await put(`${id}/manual.md`, `# ${id}\n`);
  await put(`${id}/screenshot.png`, shot);
}

beforeEach(async () => {
  root = await mkdtemp(join(tmpdir(), 'co-op-plugins-'));
  pluginsDir = join(root, 'plugins');
  await mkdir(pluginsDir);
});

afterEach(async () => {
  await rm(root, { recursive: true, force: true });
});

describe('loadPlugins', () => {
  it('indexes a valid plugin', async () => {
    await goodPlugin();
    const { issues, warnings, index } = await loadPlugins(pluginsDir, { now: NOW });
    expect(issues).toEqual([]);
    expect(warnings).toEqual([]);
    expect(index).toEqual({
      format: 1,
      generatedAt: '2026-10-29T12:00:00.000Z',
      plugins: [
        {
          id: 'drive',
          name: 'Plugin drive',
          firmware: 'op1/hundekraft',
          kind: 'fx',
          author: 'coldsoldr',
          version: '1.0',
          description: 'A plugin for tests.',
          license: 'GPL-3.0-or-later',
          file: 'drive.knl',
          screenshot: 'drive/screenshot.png',
          manual: 'drive/manual.md',
          tags: ['test'],
          sizeBytes: drive.length,
          sha256: sha(drive),
          savedName: 'drive',
        },
      ],
    });
  });

  it('indexes an opus plugin with its savedName from plugin.json', async () => {
    const opmod = opmodBytes();
    await put('opus.chorus/plugin.json', manifest('opus.chorus', { firmware: 'op1/opus', file: '10_chorus.opmod', savedName: 'opus.chorus' }));
    await put('opus.chorus/10_chorus.opmod', opmod);
    await put('opus.chorus/manual.md', 'chorus');
    await put('opus.chorus/screenshot.png', shot);
    const { issues, index } = await loadPlugins(pluginsDir, { now: NOW });
    expect(issues).toEqual([]);
    expect(index.plugins[0]).toMatchObject({ id: 'opus.chorus', file: '10_chorus.opmod', savedName: 'opus.chorus', sha256: sha(opmod) });
  });

  it('carries the extension table into the index and warns when it disagrees', async () => {
    await goodPlugin('drive', knlBytes({ name: 'drive', author: 'someone', version: '1.0' }));
    const { issues, warnings, index } = await loadPlugins(pluginsDir, { now: NOW });
    expect(issues).toEqual([]);
    expect(warnings).toEqual([{ plugin: 'drive', file: 'drive.knl', message: 'the file says author "someone" but plugin.json says "coldsoldr"' }]);
    expect(index.plugins[0]).toMatchObject({ headerAuthor: 'someone', headerVersion: '1.0' });
  });

  it('sorts plugins by id and ignores loose files in the plugins folder', async () => {
    await goodPlugin('wavefolder');
    await goodPlugin('bitcrusher');
    await put('README.md', 'about plugins');
    const { issues, index } = await loadPlugins(pluginsDir, { now: NOW });
    expect(issues).toEqual([]);
    expect(index.plugins.map((p) => p.id)).toEqual(['bitcrusher', 'wavefolder']);
  });

  it('returns an empty index when there is no plugins folder', async () => {
    const { issues, index } = await loadPlugins(join(root, 'missing'), { now: NOW });
    expect(issues).toEqual([]);
    expect(index).toEqual({ format: 1, generatedAt: '2026-10-29T12:00:00.000Z', plugins: [] });
  });

  it('reports every broken plugin and keeps the valid ones', async () => {
    await goodPlugin();
    await mkdir(join(pluginsDir, 'no_manifest'), { recursive: true });
    await put('bad_json/plugin.json', '{ "format": 1,');
    await goodPlugin('wrong_id');
    await put('wrong_id/plugin.json', manifest('other', { file: 'wrong_id.knl' }));
    await goodPlugin('extra');
    await put('extra/notes.txt', 'hi');
    await goodPlugin('not_knl', new Uint8Array(200));
    await goodPlugin('wrong_kind');
    await put('wrong_kind/plugin.json', manifest('wrong_kind', { kind: 'synth' }));

    const { issues, index } = await loadPlugins(pluginsDir, { now: NOW });
    expect(index.plugins.map((p) => p.id)).toEqual(['drive']);
    const by = (plugin: string) => issues.filter((i) => i.plugin === plugin).map((i) => `${i.file ?? ''}: ${i.message}`);
    expect(by('no_manifest')).toEqual(['plugin.json: missing']);
    expect(by('bad_json')[0]).toMatch(/^plugin\.json: not valid JSON/);
    expect(by('wrong_id')).toEqual(['plugin.json: id "other" must equal the folder name "wrong_id"', 'plugin.json: file must be "other.knl" or "other.op1p"']);
    expect(by('extra')).toEqual(['notes.txt: not part of a plugin folder; it holds plugin.json, extra.knl, manual.md and screenshot.png only']);
    expect(by('not_knl')).toEqual(['not_knl.knl: no KNPL header (not a kennel plugin)']);
    expect(by('wrong_kind')).toEqual(["wrong_kind.knl: the file's kind is fx but plugin.json says synth"]);
  });

  it('ignores .DS_Store, Thumbs.db and desktop.ini and never publishes them', async () => {
    await goodPlugin();
    await put('drive/.DS_Store', 'x');
    await put('drive/Thumbs.db', 'x');
    await put('drive/desktop.ini', 'x');
    expect((await loadPlugins(pluginsDir, { now: NOW })).issues).toEqual([]);
    const out = join(root, 'dist');
    await buildPlugins(pluginsDir, out, { now: NOW });
    expect((await readdir(join(out, 'plugins/drive'))).sort()).toEqual(['drive.knl', 'manual.md', 'plugin.json', 'screenshot.png']);
  });
});

describe('loadPlugins file checks', () => {
  it('accepts a plugin.json that starts with a byte order mark', async () => {
    await goodPlugin();
    await put('drive/plugin.json', `${String.fromCharCode(0xfeff)}${JSON.stringify(manifest('drive'))}`);
    expect((await loadPlugins(pluginsDir, { now: NOW })).issues).toEqual([]);
  });

  it('refuses a plugin.json over 64 KB without parsing it', async () => {
    await put('big/plugin.json', `${JSON.stringify(manifest('big'))}${' '.repeat(64 * 1024)}`);
    const { issues } = await loadPlugins(pluginsDir, { now: NOW });
    expect(issues).toEqual([{ plugin: 'big', file: 'plugin.json', message: expect.stringMatching(/over the 64 KB \(65536 bytes\) limit$/) }]);
  });

  it('reports a symbolic link instead of following it', async (context) => {
    await goodPlugin();
    await put('outside.png', shot);
    await rm(join(pluginsDir, 'drive', 'screenshot.png'));
    try {
      await symlink(join(pluginsDir, 'outside.png'), join(pluginsDir, 'drive', 'screenshot.png'), 'file');
    } catch (error) {
      if (['EPERM', 'EACCES'].includes((error as NodeJS.ErrnoException).code ?? '')) {
        context.skip('this system does not permit creating symbolic links');
      }
      throw error;
    }
    const { issues, index } = await loadPlugins(pluginsDir, { now: NOW });
    expect(index.plugins).toEqual([]);
    expect(issues).toEqual([{ plugin: 'drive', file: 'screenshot.png', message: 'is a link; add the real file instead' }]);
  });
});

describe('formatIssues', () => {
  it('groups issues by plugin', () => {
    const text = formatIssues([
      { plugin: 'a', file: 'plugin.json', message: 'one' },
      { plugin: 'b', message: 'two' },
      { plugin: 'a', file: 'a.knl', message: 'three' },
    ]);
    expect(text).toBe(['a', '  plugin.json: one', '  a.knl: three', 'b', '  two', '', '3 issues in 2 plugins'].join('\n'));
  });

  it('uses the singular for one issue and can name warnings', () => {
    expect(formatIssues([{ plugin: 'a', message: 'x' }])).toMatch(/1 issue in 1 plugin$/);
    expect(formatIssues([{ plugin: 'a', message: 'x' }], 'warning')).toMatch(/1 warning in 1 plugin$/);
  });
});

describe('buildPlugins', () => {
  it('copies every plugin and writes index.json', async () => {
    await goodPlugin();
    const out = join(root, 'dist');
    await mkdir(join(out, 'plugins', 'stale'), { recursive: true });
    const { issues } = await buildPlugins(pluginsDir, out, { now: NOW });
    expect(issues).toEqual([]);
    expect((await readdir(join(out, 'plugins'))).sort()).toEqual(['drive', 'index.json']);
    expect(new Uint8Array(await readFile(join(out, 'plugins/drive/drive.knl')))).toEqual(drive);
    expect(new Uint8Array(await readFile(join(out, 'plugins/drive/screenshot.png')))).toEqual(shot);
    expect(await readFile(join(out, 'plugins/drive/manual.md'), 'utf8')).toBe('# drive\n');
    const index = JSON.parse(await readFile(join(out, 'plugins/index.json'), 'utf8'));
    expect(index).toEqual((await loadPlugins(pluginsDir, { now: NOW })).index);
  });

  it('writes an empty index when there is no plugins folder', async () => {
    const out = join(root, 'dist');
    const { issues } = await buildPlugins(join(root, 'missing'), out, { now: NOW });
    expect(issues).toEqual([]);
    expect(await readdir(join(out, 'plugins'))).toEqual(['index.json']);
    expect(JSON.parse(await readFile(join(out, 'plugins/index.json'), 'utf8'))).toEqual({ format: 1, generatedAt: NOW.toISOString(), plugins: [] });
  });

  it('writes nothing when a plugin is invalid', async () => {
    await goodPlugin();
    await goodPlugin('broken');
    await rm(join(pluginsDir, 'broken', 'manual.md'));
    const out = join(root, 'dist');
    const { issues } = await buildPlugins(pluginsDir, out, { now: NOW });
    expect(issues).toEqual([{ plugin: 'broken', file: 'manual.md', message: 'missing' }]);
    await expect(readdir(join(out, 'plugins'))).rejects.toThrow();
  });
});

describe('pluginsMiddleware', () => {
  interface Sent {
    status: number;
    headers: Record<string, string>;
    body: Buffer;
    next: boolean;
  }

  async function request(url: string): Promise<Sent> {
    const handle = pluginsMiddleware(pluginsDir, { base: '/co-op/', now: () => NOW });
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
    await goodPlugin();
    const sent = await request('/co-op/plugins/index.json');
    expect(sent.headers['content-type']).toBe('application/json');
    expect(JSON.parse(sent.body.toString('utf8'))).toEqual((await loadPlugins(pluginsDir, { now: NOW })).index);
  });

  it('serves an empty index when there is no plugins folder', async () => {
    await rm(pluginsDir, { recursive: true });
    const sent = await request('/co-op/plugins/index.json');
    expect(JSON.parse(sent.body.toString('utf8')).plugins).toEqual([]);
  });

  it('serves plugin files byte for byte with their types', async () => {
    await goodPlugin();
    const knl = await request('/co-op/plugins/drive/drive.knl');
    expect(knl.status).toBe(200);
    expect(knl.headers['content-type']).toBe('application/octet-stream');
    expect(new Uint8Array(knl.body)).toEqual(drive);
    expect((await request('/co-op/plugins/drive/screenshot.png')).headers['content-type']).toBe('image/png');
    expect((await request('/co-op/plugins/drive/manual.md')).headers['content-type']).toBe('text/markdown; charset=utf-8');
    expect((await request('/co-op/plugins/drive/plugin.json')).headers['content-type']).toBe('application/json');
  });

  it('answers 404 for a missing file or a path that leaves the plugins folder', async () => {
    await goodPlugin();
    expect((await request('/co-op/plugins/drive/nope.knl')).status).toBe(404);
    expect((await request('/co-op/plugins/../package.json')).status).toBe(404);
    expect((await request('/co-op/plugins/%2e%2e/%2e%2e/package.json')).status).toBe(404);
    expect((await request('/co-op/plugins/drive')).status).toBe(404);
    for (const path of ['C:', 'C%3A', 'drive/C:', 'C%3A%5CWindows%5Cwin.ini']) {
      expect((await request(`/co-op/plugins/${path}`)).status).toBe(404);
    }
  });

  it('serves only the four files of valid plugins', async () => {
    await goodPlugin();
    await goodPlugin('broken');
    await put('broken/notes.txt', 'x');
    expect((await request('/co-op/plugins/broken/broken.knl')).status).toBe(404);
    expect((await request('/co-op/plugins/broken/plugin.json')).status).toBe(404);
    expect((await request('/co-op/plugins/drive/Drive.knl')).status).toBe(404);
  });

  it('passes other requests on', async () => {
    expect((await request('/co-op/index.html')).next).toBe(true);
    expect((await request('/co-op/packs/index.json')).next).toBe(true);
  });
});
