import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import {
  clip,
  draftListings,
  findPluginFiles,
  firstParagraph,
  firstScreen,
  headingName,
  imageUrl,
  licenseOf,
  opusKind,
  parseOpusManifest,
  sectionFor,
  tableRowFor,
} from './plugin-request-lib';
import type { Remote } from './plugin-updates-lib';

/** A real kennel file: an fx saved as "ensemble". */
const ensemble = new Uint8Array(readFileSync(new URL('../../plugins/ensemble/ensemble.knl', import.meta.url)));
const enc = (s: string): Uint8Array => new TextEncoder().encode(s);

/** A fake GitHub repo: its metadata, files by path, and an optional latest release. */
function repo(
  name: string,
  files: Record<string, string | Uint8Array>,
  extra: { license?: string; release?: string; description?: string } = {},
): Remote {
  const raw = (p: string) => `https://raw.githubusercontent.com/${name}/main/${p.split('/').map(encodeURIComponent).join('/')}`;
  const byUrl = new Map(Object.entries(files).map(([p, v]) => [raw(p), typeof v === 'string' ? enc(v) : v]));
  return {
    api: async (path) => {
      if (path === `repos/${name}`) {
        return { full_name: name, default_branch: 'main', description: extra.description ?? null, license: extra.license ? { spdx_id: extra.license } : null, owner: { login: name.split('/')[0] } };
      }
      if (path === `repos/${name}/git/trees/main?recursive=1`) return { tree: Object.keys(files).map((p) => ({ path: p, type: 'blob' })) };
      if (path === `repos/${name}/releases/latest`) return extra.release ? { tag_name: extra.release } : null;
      if (path.startsWith(`repos/${name}/commits?path=`)) return [{ sha: 'f'.repeat(40) }];
      return null;
    },
    download: async (url) => byUrl.get(url) ?? null,
  };
}

describe('reading a repo', () => {
  it('finds built plugin files, the shallowest of each, not examples or SDK copies', () => {
    expect(
      findPluginFiles(['a/x.opmod', 'build/deep/x.opmod', 'plugin-kit/examples/y.opmod', 'sdk/z.knl', 'k.knl', 'readme.md']).map((f) => f.path),
    ).toEqual(['a/x.opmod', 'k.knl']);
  });

  it('reads an opus manifest and its kind', () => {
    const m = parseOpusManifest('opmod 1\nid        esc.bubbler\nname      Bubbler\nauthor    ESC_CODE\ncore 2.31\nadds effect bubbler\n');
    expect(m).toEqual({ id: 'esc.bubbler', name: 'Bubbler', author: 'ESC_CODE', core: '2.31', adds: 'effect bubbler' });
    expect(opusKind('effect bubbler')).toBe('fx');
    expect(opusKind('synth sequencer fluke')).toBe('feature');
    expect(opusKind('synth engine x')).toBe('synth');
    expect(opusKind('convert pulses to midi clock')).toBe('feature');
  });

  it('takes prose, not badges, tables, link rows or controls', () => {
    const md = '# X 1.0\n\n![ci](https://img.shields.io/x.svg)\n\n[Download](a) · [Manual](b)\n\n| a | b |\n|---|---|\n\nX is a delay that smears your notes into a cloud. It has four knobs.\n';
    expect(firstParagraph(md)).toBe('X is a delay that smears your notes into a cloud. It has four knobs.');
    expect(headingName('# FLUKE — Generative Sequencer\n')).toBe('FLUKE');
    expect(headingName('# Midi-Reverb 1.0.0\n')).toBe('Midi-Reverb');
  });

  it('keeps decimals whole and stops at three sentences or the limit', () => {
    expect(clip('Needs Opus core 2.31 or later. Built with 2.33. Third. Fourth.')).toBe('Needs Opus core 2.31 or later. Built with 2.33. Third.');
    expect(clip('One. Two.', 5)).toBe('One.');
  });

  it('reads a shared README by section and by its table row', () => {
    const md = '# Trio\n\n| Device | Does |\n|---|---|\n| Bubbler | Granular delay with grains and pitch |\n\n## Bubbler 1.5\n\nBubbler text that is long enough.\n\n## Delayed\n\nOther.\n';
    expect(sectionFor(md, ['Bubbler'])?.trim()).toBe('Bubbler text that is long enough.');
    expect(tableRowFor(md, ['bubbler'])).toBe('Granular delay with grains and pitch.');
  });

  it('picks the screen image for the plugin and resolves it in the repo', () => {
    const md = '![badge](https://img.shields.io/a.svg)\n![Delayed](Shots/Delayed.png)\n<img src="./Shots/Bubbler%20one.png" alt="b">';
    expect(firstScreen(md)).toBe('Shots/Delayed.png');
    expect(firstScreen(md, ['bubbler'])).toBe('./Shots/Bubbler%20one.png');
    expect(imageUrl('./Shots/Bubbler%20one.png', 'o/r', 'main', 'Trio/README.md')).toBe('https://raw.githubusercontent.com/o/r/main/Trio/Shots/Bubbler%20one.png');
    expect(imageUrl('https://github.com/o/r/blob/main/a.png', 'o/r', 'main', 'README.md')).toBe('https://raw.githubusercontent.com/o/r/main/a.png');
    expect(imageUrl('javascript:alert(1)', 'o/r', 'main', 'README.md')).toBeNull();
  });

  it('names licences by co-op’s ids, with "or later" only when it is about the GPL', () => {
    expect(licenseOf('GPL-3.0', null, 'Needs Opus core 2.31 or later.')).toBe('GPL-3.0-only');
    expect(licenseOf('GPL-3.0', null, 'Licensed GPL-3.0-or-later.')).toBe('GPL-3.0-or-later');
    expect(licenseOf(null, 'GNU GENERAL PUBLIC LICENSE\n   Version 3, 29 June 2007', '')).toBe('GPL-3.0-only');
    expect(licenseOf(null, 'Permission is hereby granted, free of charge', '')).toBe('MIT');
    expect(licenseOf('GPL-2.0', null, '')).toBeNull();
    expect(licenseOf('WTFPL', null, '')).toBeNull();
  });
});

describe('drafting listings', () => {
  const trio = repo('esc/OP-1', {
    'README.md': '# OP-1\n\nTools.\n',
    'Trio/README.md': '# Trio\n\n| Device | Does |\n|---|---|\n| Bubbler | Granular delay with overlapping grains |\n| Delayed | Delay with a resonant feedback filter |\n\n## Bubbler\n\n![Bubbler](Shots/Bubbler.png)\n\n## Delayed\n\n![Delayed](Shots/Delayed.png)\n',
    'Trio/LICENSE': 'GNU GENERAL PUBLIC LICENSE\n Version 3, 29 June 2007',
    'Trio/bubbler.opmod': 'x',
    'Trio/delayed.opmod': 'x',
    'Trio/Source/Bubbler/manifest.txt': 'id esc.bubbler\nname Bubbler\nauthor ESC_CODE\ncore 2.31\nadds effect bubbler\n',
    'Trio/Source/Delayed/manifest.txt': 'id esc.delay\nname Delayed\nauthor ESC_CODE\ncore 2.31\nadds effect delayed\n',
  });

  it('drafts each plugin in a shared folder from its manifest, table row and image', async () => {
    const r = await draftListings('esc/OP-1', null, trio, new Map());
    expect(r.drafts.map((d) => d.id)).toEqual(['bubbler', 'delayed']);
    const [b] = r.drafts;
    expect(b?.manifest).toMatchObject({
      id: 'bubbler',
      name: 'Bubbler',
      firmware: 'op1/opus',
      kind: 'fx',
      author: 'ESC_CODE',
      license: 'GPL-3.0-only',
      link: 'https://github.com/esc/OP-1/tree/main/Trio',
      requires: 'opus 2.31',
      description: 'Granular delay with overlapping grains.',
    });
    expect(b?.manifest).not.toHaveProperty('version');
    expect(b?.screenshot).toBe('https://raw.githubusercontent.com/esc/OP-1/main/Trio/Shots/Bubbler.png');
    expect(b?.manual).toContain('co-op lists it for reference and hosts no copy.');
  });

  it('leaves plugins already listed from the repo alone, and renames one whose id is taken by another', async () => {
    const existing = new Map<string, Record<string, unknown>>([
      ['bubbler', { source: 'https://github.com/esc/OP-1' }],
      ['delayed', { source: 'https://github.com/someone/else' }],
    ]);
    const r = await draftListings('esc/OP-1', null, trio, existing);
    expect(r.skipped).toEqual(['bubbler']);
    expect(r.drafts.map((d) => d.id)).toEqual(['delayed_esc']);
  });

  it('pins a committed .knl for install over USB, from its header', async () => {
    const r = await draftListings('a/ens', null, repo('a/ens', { 'README.md': '# Ensemble\n\nA chorus for hundekraft 3.2.6 that makes one voice many.\n', 'build/ensemble.knl': ensemble }, { license: 'MIT' }), new Map());
    expect(r.drafts[0]?.manifest).toMatchObject({
      id: 'ensemble',
      firmware: 'op1/hundekraft',
      kind: 'fx',
      license: 'MIT',
      requires: 'hundekraft 3.2.6',
      fileUrl: `https://raw.githubusercontent.com/a/ens/${'f'.repeat(40)}/build/ensemble.knl`,
      sizeBytes: ensemble.length,
    });
    expect(r.drafts[0]?.manifest.sha256).toMatch(/^[0-9a-f]{64}$/);
  });

  it('joins one plugin’s builds for two firmwares into one listing', async () => {
    const r = await draftListings('a/ens', null, repo('a/ens', { 'README.md': '# Ensemble\n\nA chorus that makes one voice many, for kennel and opus.\n', 'dl/ensemble.knl': ensemble, 'dl/ensemble.opmod': 'x' }), new Map());
    expect(r.drafts).toHaveLength(1);
    const builds = r.drafts[0]?.manifest.builds as { firmware: string }[];
    expect(builds.map((b) => b.firmware)).toEqual(['op1/hundekraft', 'op1/opus']);
    expect(r.drafts[0]?.manifest).not.toHaveProperty('firmware');
  });

  it('drafts a repo with no built file only when its firmware is clear, with the release as its version', async () => {
    const bare = repo('a/tool', { 'README.md': '# Tool\n\nA sequencer for opus that plays chords in time.\n' }, { release: 'v2.1.0' });
    const r = await draftListings('a/tool', null, bare, new Map());
    expect(r.drafts[0]?.manifest).toMatchObject({ id: 'tool', name: 'Tool', firmware: 'op1/opus', kind: 'feature', version: '2.1.0', link: 'https://github.com/a/tool/releases/latest' });

    const vague = repo('a/vague', { 'README.md': '# Vague\n\nA thing for the OP-1 that does stuff nicely.\n' });
    const none = await draftListings('a/vague', null, vague, new Map());
    expect(none.drafts).toHaveLength(0);
    expect(none.problems[0]).toMatch(/doesn’t say which firmware/);
    expect((await draftListings('a/vague', 'hundekraft', vague, new Map())).drafts[0]?.manifest.firmware).toBe('op1/hundekraft');
  });

  it('says so for a repo it cannot read', async () => {
    const r = await draftListings('a/missing', null, repo('a/other', {}), new Map());
    expect(r.problems[0]).toMatch(/isn't a public repo/);
  });
});
