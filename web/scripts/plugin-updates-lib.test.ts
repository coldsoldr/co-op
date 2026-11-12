import { createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import { compareVersions, summary, updatePlugin, type Remote } from './plugin-updates-lib';

const plugin = (id: string): Uint8Array => new Uint8Array(readFileSync(new URL(`../../plugins/${id}/${id}.knl`, import.meta.url)));
/** A real kennel file: an fx saved as "ensemble". */
const ensemble = plugin('ensemble');
const OLD = 'a'.repeat(40);
const NEW = 'b'.repeat(40);
const PATH = 'build/ensemble.knl';
const rawAt = (sha: string): string => `https://raw.githubusercontent.com/someone/ensemble-fx/${sha}/${PATH}`;

/** A fake GitHub: API answers by path, downloads by URL; anything else is a 404. */
function remote(api: Record<string, unknown>, files: Record<string, Uint8Array> = {}): Remote & { asked: string[] } {
  const asked: string[] = [];
  return {
    asked,
    api: async (path) => {
      asked.push(path);
      return api[path] ?? null;
    },
    download: async (url) => files[url] ?? null,
  };
}

const fileListing = (): Record<string, unknown> => ({
  id: 'ensemble',
  kind: 'fx',
  firmware: 'op1/hundekraft',
  link: 'https://github.com/someone/ensemble-fx',
  fileUrl: rawAt(OLD),
  sha256: 'c'.repeat(64),
  sizeBytes: 10,
  version: '0.1',
});
const commitsPath = `repos/someone/ensemble-fx/commits?path=${encodeURIComponent(PATH)}&per_page=1`;

describe('plugin updates from the author’s repo', () => {
  it('follows a committed .knl to its newest commit: new pin, hash, size and the header’s version', async () => {
    const manifest = fileListing();
    const found = await updatePlugin(manifest, remote({ [commitsPath]: [{ sha: NEW }] }, { [rawAt(NEW)]: ensemble }));
    expect(found.problems).toEqual([]);
    expect(found.changes).toMatchObject([{ plugin: 'ensemble', kind: 'file', to: expect.stringContaining(NEW.slice(0, 7)) }]);
    expect(manifest).toMatchObject({
      fileUrl: rawAt(NEW),
      sha256: createHash('sha256').update(ensemble).digest('hex'),
      sizeBytes: ensemble.length,
    });
    expect(manifest.version).not.toBe('0.1');
  });

  it('leaves an unchanged file alone', async () => {
    const manifest = fileListing();
    const found = await updatePlugin(manifest, remote({ [commitsPath]: [{ sha: OLD }] }));
    expect(found).toEqual({ changes: [], problems: [] });
    expect(manifest.fileUrl).toBe(rawAt(OLD));
  });

  it('refuses a new file that is another plugin, or not a plugin, and says why', async () => {
    const other = fileListing();
    const wrong = await updatePlugin(other, remote({ [commitsPath]: [{ sha: NEW }] }, { [rawAt(NEW)]: plugin('bitcrusher') }));
    expect(wrong.changes).toEqual([]);
    expect(wrong.problems[0]?.message).toMatch(/not the fx "ensemble" listed\. Not applied/);
    expect(other.fileUrl).toBe(rawAt(OLD));

    const junk = await updatePlugin(fileListing(), remote({ [commitsPath]: [{ sha: NEW }] }, { [rawAt(NEW)]: new Uint8Array(200) }));
    expect(junk.problems[0]?.message).toMatch(/fails co-op's check/);

    const gone = await updatePlugin(fileListing(), remote({ [commitsPath]: [] }));
    expect(gone.problems[0]?.message).toMatch(/moved or removed/);
  });

  it('takes a newer release’s tag as the version of a build that only links out, and never goes back', async () => {
    const manifest = { id: 'castle', kind: 'synth', builds: [{ firmware: 'op1/opus', link: 'https://github.com/someone/castle/releases/latest', version: '1.0' }] };
    const newer = await updatePlugin(manifest, remote({ 'repos/someone/castle/releases/latest': { tag_name: 'v1.1', html_url: 'https://github.com/someone/castle/releases/tag/v1.1', body: 'Fixes.' } }));
    expect(newer.changes).toMatchObject([{ from: '1.0', to: '1.1', kind: 'release', notes: 'Fixes.' }]);
    expect(manifest.builds[0]?.version).toBe('1.1');

    const older = await updatePlugin(manifest, remote({ 'repos/someone/castle/releases/latest': { tag_name: '1.0.3', html_url: 'x' } }));
    expect(older.changes).toEqual([]);
    expect(manifest.builds[0]?.version).toBe('1.1');
  });

  it('leaves the workshop’s and opus’s listings, hosted plugins and versionless builds to their own process', async () => {
    const r = remote({});
    await updatePlugin({ id: 'moai', kind: 'synth', firmware: 'op1/hundekraft', link: 'https://hundekraft.github.io/op1-workshop/', source: 'https://github.com/fungiboletus/op-moai', version: '3.2' }, r);
    await updatePlugin({ id: 'opus.dice', kind: 'feature', firmware: 'op1/opus', link: 'https://github.com/LizardMan245/opus/releases', version: '2.0' }, r);
    await updatePlugin({ id: 'rings', kind: 'synth', firmware: 'op1/hundekraft', file: 'rings.knl', version: '0.1' }, r);
    await updatePlugin({ id: 'x', kind: 'synth', firmware: 'op1/opus', link: 'https://github.com/someone/x' }, r);
    expect(r.asked).toEqual([]);
  });

  it('compares versions by their numbers', () => {
    expect(compareVersions('v1.10', '1.9')).toBeGreaterThan(0);
    expect(compareVersions('1.5.2', '1.5.2')).toBe(0);
    expect(compareVersions('1.0.3', '1.4')).toBeLessThan(0);
  });

  it('writes a pull request body with each change and what needs a person', () => {
    const text = summary(
      [{ plugin: 'castle', firmware: 'op1/opus', repo: 'someone/castle', from: '1.0', to: '1.1', kind: 'release', compare: 'https://example.com/r', notes: 'Fixes.' }],
      [{ plugin: 'erdtree', firmware: 'op1/hundekraft', message: 'moved' }],
    );
    expect(text).toContain('| castle | op1/opus | 1.0 | 1.1 | [the release](https://example.com/r) |');
    expect(text).toContain('### Needs a person');
    expect(summary([], [])).toBe('No plugin updates today.\n');
  });
});
