import { describe, it, expect } from 'vitest';
import {
  MAX_FILE_BYTES,
  MAX_PACK_BYTES,
  PACK_LICENSES,
  parseManifest,
  patchFileNameProblem,
  validatePackFiles,
  type PackManifest,
  type PackIssue,
} from './packs';

const FIRMWARE = ['op1/stock', 'op1/hundekraft'] as const;

const valid = (): Record<string, unknown> => ({
  format: 1,
  name: 'Test Tones',
  slug: 'test-tones',
  author: 'co-op',
  description: 'Plain sine patches for checking an install works.',
  license: 'CC0-1.0',
  firmware: ['op1/stock'],
  tags: ['utility'],
  patches: [{ file: 'synth/cluster.aif', notes: 'optional' }, { file: 'drum/boombap.aif' }],
});

const parse = (overrides: Record<string, unknown>, folder = 'test-tones') =>
  parseManifest({ ...valid(), ...overrides }, folder, FIRMWARE);

const messages = (issues: PackIssue[]): string => issues.map((i) => i.message).join('\n');

/** Asserts the manifest is rejected with a message matching `pattern`. */
function rejects(overrides: Record<string, unknown>, pattern: RegExp, folder?: string): PackIssue[] {
  const { manifest, issues } = parse(overrides, folder);
  expect(manifest).toBeNull();
  expect(messages(issues)).toMatch(pattern);
  for (const issue of issues) expect(issue.pack).toBe(folder ?? 'test-tones');
  return issues;
}

describe('parseManifest', () => {
  it('accepts a valid manifest', () => {
    const { manifest, issues } = parse({});
    expect(issues).toEqual([]);
    expect(manifest).toEqual(valid());
  });

  it('treats a missing tags list as empty', () => {
    const json = valid();
    delete json.tags;
    const { manifest, issues } = parseManifest(json, 'test-tones', FIRMWARE);
    expect(issues).toEqual([]);
    expect(manifest?.tags).toEqual([]);
  });

  it('rejects something that is not an object', () => {
    for (const json of [null, [], 'pack', 3]) {
      const { manifest, issues } = parseManifest(json, 'x', FIRMWARE);
      expect(manifest).toBeNull();
      expect(messages(issues)).toMatch(/must be a JSON object/);
      expect(issues[0]).toMatchObject({ pack: 'x', file: 'pack.json' });
    }
  });

  it('requires format 1', () => {
    rejects({ format: 2 }, /format must be 1 \(found 2\)/);
    rejects({ format: '1' }, /format must be 1 \(found "1"\)/);
    rejects({ format: undefined }, /format must be 1 \(it is missing\)/);
  });

  it('rejects unknown fields', () => {
    rejects({ licence: 'CC0-1.0' }, /unknown field "licence"/);
  });

  describe('slug', () => {
    it('must equal the folder name', () => {
      rejects({}, /slug "test-tones" must equal the folder name "other"/, 'other');
    });

    it('must be lower-case words joined by single hyphens', () => {
      for (const slug of ['Test-Tones', 'test_tones', '-test', 'test-', 'test--tones', '', 'té']) {
        rejects({ slug }, /slug/, slug);
      }
    });

    it('is at most 40 characters', () => {
      const ok = 'a'.repeat(40);
      expect(parse({ slug: ok }, ok).issues).toEqual([]);
      const long = 'a'.repeat(41);
      rejects({ slug: long }, /slug .*40/, long);
    });
  });

  describe('text fields', () => {
    const cases: [string, number][] = [
      ['name', 60],
      ['author', 60],
      ['description', 500],
    ];
    for (const [field, max] of cases) {
      it(`${field} is 1..${max} characters`, () => {
        expect(parse({ [field]: 'x'.repeat(max) }).issues).toEqual([]);
        rejects({ [field]: 'x'.repeat(max + 1) }, new RegExp(`${field} must be 1 to ${max} characters`));
        rejects({ [field]: '' }, new RegExp(`${field} must be 1 to ${max} characters`));
        rejects({ [field]: '   ' }, new RegExp(`${field} must be 1 to ${max} characters`));
        rejects({ [field]: 7 }, new RegExp(`${field} must be a string`));
      });
    }
  });

  describe('tags', () => {
    it('allows up to 8 tags', () => {
      const tags = ['a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'];
      expect(parse({ tags }).issues).toEqual([]);
      rejects({ tags: [...tags, 'i'] }, /at most 8 tags/);
    });

    it('checks each tag', () => {
      expect(parse({ tags: ['lo-fi', 'x'.repeat(20)] }).issues).toEqual([]);
      for (const tag of ['Lo-fi', 'lo fi', 'x'.repeat(21), '', 3]) rejects({ tags: [tag] }, /tag/);
    });

    it('rejects duplicate tags', () => {
      rejects({ tags: ['pad', 'pad'] }, /duplicate tag "pad"/);
    });

    it('must be a list', () => {
      rejects({ tags: 'pad' }, /tags must be a list/);
    });
  });

  it('accepts every listed licence and nothing else', () => {
    expect(PACK_LICENSES).toEqual(['CC0-1.0', 'CC-BY-4.0', 'CC-BY-SA-4.0', 'CC-BY-NC-4.0', 'CC-BY-NC-SA-4.0']);
    for (const license of PACK_LICENSES) expect(parse({ license }).issues).toEqual([]);
    rejects({ license: 'MIT' }, /license "MIT" must be one of CC0-1.0, CC-BY-4.0/);
    rejects({ license: 'cc0-1.0' }, /license/);
  });

  describe('firmware', () => {
    it('must be a non-empty list', () => {
      rejects({ firmware: [] }, /firmware must list at least one/);
      rejects({ firmware: 'op1/stock' }, /firmware must be a list/);
    });

    it('accepts every known profile id', () => {
      expect(parse({ firmware: ['op1/stock', 'op1/hundekraft'] }).issues).toEqual([]);
    });

    it('rejects an unknown profile id', () => {
      rejects({ firmware: ['op1/stock', 'op1/mystery'] }, /unknown firmware "op1\/mystery"; known: op1\/stock, op1\/hundekraft/);
    });

    it('rejects duplicates', () => {
      rejects({ firmware: ['op1/stock', 'op1/stock'] }, /duplicate firmware "op1\/stock"/);
    });
  });

  describe('patches', () => {
    it('must be a non-empty list of at most 100', () => {
      rejects({ patches: [] }, /patches must list at least one/);
      rejects({ patches: {} }, /patches must be a list/);
      const many = Array.from({ length: 101 }, (_, i) => ({ file: `synth/p${i}.aif` }));
      rejects({ patches: many }, /at most 100 patches/);
      expect(parse({ patches: many.slice(0, 100) }).issues).toEqual([]);
    });

    it('accepts synth/ and drum/ files ending in .aif or .aiff in any case', () => {
      const patches = ['synth/a.aif', 'synth/b.AIFF', 'drum/c.Aif', 'drum/d e (1).aiff'].map((file) => ({ file }));
      expect(parse({ patches }).issues).toEqual([]);
    });

    it('says folder names are lower-case', () => {
      for (const file of ['Synth/a.aif', 'DRUM/a.aif']) {
        const issues = rejects({ patches: [{ file }] }, /folder names are lower-case: use synth\/ or drum\//);
        expect(issues[0]?.file).toBe(file);
      }
    });

    it('caps a file name at 64 characters', () => {
      const ok = `synth/${'a'.repeat(60)}.aif`;
      expect(parse({ patches: [{ file: ok }] }).issues).toEqual([]);
      rejects({ patches: [{ file: `synth/${'a'.repeat(61)}.aif` }] }, /longer than 64 characters/);
    });

    it('rejects files outside synth/ or drum/, or nested deeper', () => {
      for (const file of ['a.aif', 'tape/a.aif', 'synth/sub/a.aif', 'synth/', '/synth/a.aif', 'synth//a.aif']) {
        const issues = rejects({ patches: [{ file }] }, /must be synth\/<name> or drum\/<name>/);
        expect(issues[0]?.file).toBe(file);
      }
    });

    it('rejects names that are not FAT-safe or lack the extension', () => {
      for (const file of ['synth/a?.aif', 'synth/.a.aif', 'synth/a.aif.', 'synth/a.aif ', 'synth/a.wav', 'synth/.aif', 'drum/a:b.aif']) {
        const issues = rejects({ patches: [{ file }] }, /file name/);
        expect(issues[0]?.file).toBe(file);
      }
    });

    it('rejects paths that collide ignoring case', () => {
      const issues = rejects(
        { patches: [{ file: 'synth/Pad.aif' }, { file: 'synth/pad.aif' }] },
        /listed more than once \(names are compared ignoring case\)/,
      );
      expect(issues[0]?.file).toBe('synth/pad.aif');
    });

    it('checks each entry shape', () => {
      rejects({ patches: ['synth/a.aif'] }, /each patch must be an object/);
      rejects({ patches: [{ file: 3 }] }, /file must be a string/);
      rejects({ patches: [{ file: 'synth/a.aif', notes: 3 }] }, /notes must be a string/);
      rejects({ patches: [{ file: 'synth/a.aif', notes: 'x'.repeat(201) }] }, /notes must be at most 200/);
      rejects({ patches: [{ file: 'synth/a.aif', note: 'x' }] }, /unknown field "note"/);
    });
  });

  it('reports every problem at once', () => {
    const { issues } = parse({ name: '', license: 'MIT', firmware: [] });
    expect(issues).toHaveLength(3);
  });
});

describe('patchFileNameProblem', () => {
  it('returns null for a good name and a reason otherwise', () => {
    expect(patchFileNameProblem('kick.aif')).toBeNull();
    expect(patchFileNameProblem('kick.wav')).toMatch(/\.aif or \.aiff/);
    expect(patchFileNameProblem('')).toMatch(/empty/);
  });
});

describe('validatePackFiles', () => {
  const manifest = parseManifest(valid(), 'test-tones', FIRMWARE).manifest as PackManifest;
  const kinds: Record<string, 'synth' | 'drum' | 'unknown'> = {
    'synth/cluster.aif': 'synth',
    'drum/boombap.aif': 'drum',
  };
  const parsePatch = async (path: string) => ({ kind: kinds[path] ?? 'unknown', name: path, engine: 'x' });
  const files = (extra: { path: string; size: number }[] = []) => [
    { path: 'pack.json', size: 300 },
    { path: 'synth/cluster.aif', size: 1000 },
    { path: 'drum/boombap.aif', size: 1000 },
    ...extra,
  ];

  it('passes a complete pack, with or without a README', async () => {
    expect(await validatePackFiles(manifest, files(), parsePatch)).toEqual([]);
    expect(await validatePackFiles(manifest, files([{ path: 'README.md', size: 10 }]), parsePatch)).toEqual([]);
  });

  it('reports a listed file that is missing', async () => {
    const issues = await validatePackFiles(manifest, files().slice(0, 2), parsePatch);
    expect(issues).toEqual([{ pack: 'test-tones', file: 'drum/boombap.aif', message: 'listed in pack.json but missing' }]);
  });

  it('reports a listed file whose spelling differs only in case', async () => {
    const on = files().map((f) => (f.path === 'drum/boombap.aif' ? { ...f, path: 'drum/BoomBap.aif' } : f));
    const issues = await validatePackFiles(manifest, on, parsePatch);
    expect(messages(issues)).toMatch(/drum\/BoomBap\.aif.*differs only in case/);
    expect(issues).toHaveLength(1);
  });

  it('reports unlisted files', async () => {
    const issues = await validatePackFiles(
      manifest,
      files([{ path: 'synth/extra.aif', size: 1 }, { path: 'notes.txt', size: 1 }, { path: 'drum/x/y.aif', size: 1 }]),
      parsePatch,
    );
    expect(issues.map((i) => i.file)).toEqual(['synth/extra.aif', 'notes.txt', 'drum/x/y.aif']);
    expect(messages(issues)).toMatch(/not listed in pack\.json/);
  });

  it('reports a file larger than 2 MB without parsing it', async () => {
    let parsed = 0;
    const counting = async (path: string) => {
      parsed++;
      return parsePatch(path);
    };
    const big = files().map((f) => (f.path === 'synth/cluster.aif' ? { ...f, size: MAX_FILE_BYTES + 1 } : f));
    const issues = await validatePackFiles(manifest, big, counting);
    expect(issues).toEqual([{ pack: 'test-tones', file: 'synth/cluster.aif', message: 'is 2097153 bytes, over the 2 MB (2097152 bytes) limit' }]);
    expect(parsed).toBe(1);
    const exact = files().map((f) => (f.path === 'synth/cluster.aif' ? { ...f, size: MAX_FILE_BYTES } : f));
    expect(await validatePackFiles(manifest, exact, parsePatch)).toEqual([]);
  });

  it('reports a pack larger than 20 MB', async () => {
    const patches = Array.from({ length: 11 }, (_, i) => ({ file: `synth/p${i}.aif` }));
    const big = parseManifest({ ...valid(), patches }, 'test-tones', FIRMWARE).manifest as PackManifest;
    const onDisk = [{ path: 'pack.json', size: 100 }, ...patches.map((p) => ({ path: p.file, size: MAX_FILE_BYTES }))];
    const issues = await validatePackFiles(big, onDisk, async () => ({ kind: 'synth', name: 'p', engine: 'cluster' }));
    expect(issues).toEqual([{ pack: 'test-tones', message: 'patches total 23068672 bytes, over the 20 MB (20971520 bytes) limit' }]);
    expect(MAX_PACK_BYTES).toBe(20 * 1024 * 1024);
  });

  it('reports a case variant of a listed file as unlisted when the listed file exists', async () => {
    const issues = await validatePackFiles(manifest, files([{ path: 'synth/Cluster.aif', size: 1000 }]), parsePatch);
    expect(issues).toEqual([
      { pack: 'test-tones', file: 'synth/Cluster.aif', message: 'not listed in pack.json; list it under "patches" or remove it' },
    ]);
  });

  it('caps pack.json and README.md at 64 KB', async () => {
    const big = files().map((f) => (f.path === 'pack.json' ? { ...f, size: 64 * 1024 + 1 } : f));
    const issues = await validatePackFiles(manifest, [...big, { path: 'README.md', size: 64 * 1024 + 1 }], parsePatch);
    expect(issues).toEqual([
      { pack: 'test-tones', file: 'pack.json', message: 'is 65537 bytes, over the 64 KB (65536 bytes) limit' },
      { pack: 'test-tones', file: 'README.md', message: 'is 65537 bytes, over the 64 KB (65536 bytes) limit' },
    ]);
    const exact = files([{ path: 'README.md', size: 64 * 1024 }]);
    expect(await validatePackFiles(manifest, exact, parsePatch)).toEqual([]);
  });

  it('reports a file that does not parse', async () => {
    const failing = async (path: string) => {
      if (path === 'synth/cluster.aif') throw new Error('not an AIFF file');
      return parsePatch(path);
    };
    const issues = await validatePackFiles(manifest, files(), failing);
    expect(issues).toEqual([{ pack: 'test-tones', file: 'synth/cluster.aif', message: 'not a readable patch: not an AIFF file' }]);
  });

  it('reports a kind that does not match its folder', async () => {
    const swapped = async (path: string) => ({ kind: path.startsWith('synth/') ? ('drum' as const) : ('unknown' as const), name: '', engine: '' });
    const issues = await validatePackFiles(manifest, files(), swapped);
    expect(issues).toEqual([
      { pack: 'test-tones', file: 'synth/cluster.aif', message: 'is a drum patch but sits in synth/' },
      { pack: 'test-tones', file: 'drum/boombap.aif', message: 'has no op-1 synth or drum metadata but sits in drum/' },
    ]);
  });
});
