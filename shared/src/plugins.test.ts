import { describe, it, expect } from 'vitest';
import { crc32 } from './crc32';
import {
  MAX_KNL_BYTES,
  MAX_OPMOD_BYTES,
  MAX_SCREENSHOT_BYTES,
  PLUGIN_LICENSES,
  checkCoop,
  checkKnl,
  checkManual,
  checkOp1p,
  checkOpmod,
  checkScreenshot,
  isHosted,
  isInstallable,
  manifestBuilds,
  parsePluginManifest,
  pluginBuilds,
  pluginFor,
  pluginIndexEntry,
  pluginIndexEntryOfBuilds,
  validatePluginFiles,
  type PluginIndexEntry,
  type PluginIssue,
  type PluginManifest,
} from './plugins';

const ascii = (text: string): Uint8Array => Uint8Array.from(text, (c) => c.charCodeAt(0));
const setU16 = (b: Uint8Array, at: number, v: number): void => {
  b[at] = v & 0xff;
  b[at + 1] = (v >>> 8) & 0xff;
};
const setU32 = (b: Uint8Array, at: number, v: number): void => {
  setU16(b, at, v & 0xffff);
  setU16(b, at + 2, v >>> 16);
};
const getU32 = (b: Uint8Array, at: number): number =>
  ((b[at] as number) | ((b[at + 1] as number) << 8) | ((b[at + 2] as number) << 16) | ((b[at + 3] as number) << 24)) >>> 0;
const pad4 = (n: number): number => (n + 3) & ~3;

/** Writes the CRC of a .knl whose header starts at `h`: over the whole file with its 4 bytes as zero. */
function withKnlCrc(bytes: Uint8Array, h: number): Uint8Array {
  setU32(bytes, h + 12, 0);
  setU32(bytes, h + 12, crc32(bytes));
  return bytes;
}

interface KnlOptions {
  svg?: string;
  kind?: number;
  order?: number;
  name?: string;
  /** Extension table entries as [tag, text]; each is written NUL-terminated. */
  ext?: [number, string][];
  /** Bytes of plugin body after the header. */
  body?: number;
}

/** A small but valid .knl: optional SVG prefix, a 144-byte header, a body and an extension table. */
function knl(options: KnlOptions = {}): Uint8Array {
  const { svg, kind = 2, order = 0, name = 'tape drive', ext, body = 16 } = options;
  const h = svg === undefined ? 0 : pad4(svg.length + 1);
  const entries = (ext ?? []).map(([tag, text]) => ({ tag, data: ascii(`${text}\0`) }));
  const extSize = ext === undefined ? 0 : entries.reduce((sum, e) => sum + 4 + pad4(e.data.length), 0) + 4;
  const extAt = h + 144 + body;
  const bytes = new Uint8Array(extAt + extSize);
  if (svg !== undefined) bytes.set(ascii(svg), 0);
  bytes.set(ascii('KNPL'), h);
  setU16(bytes, h + 4, 1);
  setU16(bytes, h + 6, 144);
  setU32(bytes, h + 8, bytes.length);
  setU16(bytes, h + 16, 1);
  bytes[h + 18] = kind;
  bytes[h + 19] = order;
  bytes.set(ascii(name), h + 20);
  setU32(bytes, h + 132, svg === undefined ? 0 : svg.length + 1);
  setU32(bytes, h + 136, 0xffffffff);
  if (ext !== undefined) {
    setU32(bytes, h + 140, extAt);
    let at = extAt;
    for (const { tag, data } of entries) {
      setU16(bytes, at, tag);
      setU16(bytes, at + 2, data.length);
      bytes.set(data, at + 4);
      at += 4 + pad4(data.length);
    }
  }
  return withKnlCrc(bytes, h);
}

/** Changes a valid .knl and writes its CRC again, so only the change is wrong. */
function knlWith(change: (b: Uint8Array, h: number) => void, options: KnlOptions = {}): Uint8Array {
  const bytes = knl(options);
  const h = options.svg === undefined ? 0 : pad4(options.svg.length + 1);
  change(bytes, h);
  return withKnlCrc(bytes, h);
}

interface Op1pOptions {
  svg?: string;
  kind?: number;
  name?: string;
  binary?: number;
}

/** A valid .op1p: optional SVG prefix, the binary, and the 16-byte trailer. */
function op1p(options: Op1pOptions = {}): Uint8Array {
  const { svg, kind = 1, name = 'tape drive', binary = 76 } = options;
  const at = svg === undefined ? 0 : pad4(svg.length + 1);
  const bytes = new Uint8Array(at + binary + 16);
  if (svg !== undefined) bytes.set(ascii(svg), 0);
  setU32(bytes, at, 0x5031504f);
  setU32(bytes, at + 4, 1);
  setU32(bytes, at + 8, 1);
  setU32(bytes, at + 12, kind);
  bytes.set(ascii(name), at + 16);
  bytes[at + binary - 1] = 0x55;
  const t = at + binary;
  setU32(bytes, t, 0x5431504f);
  setU32(bytes, t + 4, at);
  setU32(bytes, t + 8, binary);
  setU32(bytes, t + 12, crc32(bytes.subarray(at, at + binary)));
  return bytes;
}

/** Changes a valid .op1p's binary and writes the trailer CRC again. */
function op1pWith(change: (b: Uint8Array, at: number) => void, options: Op1pOptions = {}): Uint8Array {
  const bytes = op1p(options);
  const t = bytes.length - 16;
  const at = getU32(bytes, t + 4);
  change(bytes, at);
  setU32(bytes, t + 12, crc32(bytes.subarray(at, at + getU32(bytes, t + 8))));
  return bytes;
}

const PNG_SIGNATURE = [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a];

function png(width: number, height: number, extra = 0): Uint8Array {
  const bytes = new Uint8Array(33 + extra);
  bytes.set(PNG_SIGNATURE, 0);
  bytes.set([0, 0, 0, 13], 8);
  bytes.set(ascii('IHDR'), 12);
  bytes.set([width >>> 24, (width >>> 16) & 0xff, (width >>> 8) & 0xff, width & 0xff], 16);
  bytes.set([height >>> 24, (height >>> 16) & 0xff, (height >>> 8) & 0xff, height & 0xff], 20);
  bytes.set([8, 6, 0, 0, 0], 24);
  return bytes;
}

const valid = (): Record<string, unknown> => ({
  format: 1,
  id: 'tape_drive',
  name: 'Tape drive',
  firmware: 'op1/hundekraft',
  kind: 'fx',
  author: 'coldsoldr',
  version: '1.0',
  description: 'Tape saturation for the OP-1.',
  license: 'GPL-3.0-or-later',
  file: 'tape_drive.knl',
  screenshot: 'screenshot.png',
  manual: 'manual.md',
  source: 'https://github.com/coldsoldr/te-op1-cfw',
  requires: 'hundekraft 3.0.3',
  credits: 'Thanks to everyone.',
  tags: ['saturation', 'tape'],
});

const opusValid = (): Record<string, unknown> => ({
  format: 1,
  id: 'opus.chorus-2',
  name: 'Chorus 2',
  firmware: 'op1/opus',
  kind: 'fx',
  author: 'someone',
  version: 'v2',
  description: 'A chorus.',
  license: 'LicenseRef-Freeware',
  file: '10_chorus.opmod',
  screenshot: 'screen.png',
  manual: 'manual.md',
  savedName: 'opus.chorus',
});

const parse = (overrides: Record<string, unknown>, folder = 'tape_drive') =>
  parsePluginManifest({ ...valid(), ...overrides }, folder);

const messages = (issues: readonly PluginIssue[]): string => issues.map((i) => i.message).join('\n');

function rejects(overrides: Record<string, unknown>, pattern: RegExp, folder?: string): PluginIssue[] {
  const { manifest, issues } = parse(overrides, folder);
  expect(manifest).toBeNull();
  expect(messages(issues)).toMatch(pattern);
  for (const issue of issues) expect(issue.plugin).toBe(folder ?? 'tape_drive');
  return issues;
}

describe('parsePluginManifest', () => {
  it('accepts a valid hundekraft manifest', () => {
    const { manifest, issues } = parse({});
    expect(issues).toEqual([]);
    expect(manifest).toEqual(valid());
  });

  it('accepts a valid opus manifest', () => {
    const { manifest, issues } = parsePluginManifest(opusValid(), 'opus.chorus-2');
    expect(issues).toEqual([]);
    expect(manifest).toEqual({ ...opusValid(), tags: [] });
  });

  it('leaves out optional fields that are absent and treats missing tags as empty', () => {
    const json = valid();
    for (const key of ['source', 'requires', 'credits', 'tags']) delete json[key];
    const { manifest, issues } = parsePluginManifest(json, 'tape_drive');
    expect(issues).toEqual([]);
    expect(manifest?.tags).toEqual([]);
    expect(Object.keys(manifest ?? {})).not.toContain('source');
    expect(Object.keys(manifest ?? {})).not.toContain('requires');
    expect(Object.keys(manifest ?? {})).not.toContain('credits');
  });

  it('rejects something that is not an object', () => {
    for (const json of [null, [], 'plugin', 3]) {
      const { manifest, issues } = parsePluginManifest(json, 'x');
      expect(manifest).toBeNull();
      expect(messages(issues)).toMatch(/must be a JSON object/);
    }
  });

  it('rejects unknown fields and a wrong format', () => {
    rejects({ extra: 1 }, /unknown field "extra"/);
    rejects({ format: 2 }, /format must be 1 \(found 2\)/);
    rejects({ format: undefined }, /format must be 1 \(it is missing\)/);
  });

  it('applies the hundekraft id rule', () => {
    rejects({ id: 'Tape', file: 'Tape.knl' }, /id "Tape" must be 1 to 15/, 'Tape');
    rejects({ id: 'a_very_long_name_x', file: 'a_very_long_name_x.knl' }, /must be 1 to 15/, 'a_very_long_name_x');
    rejects({ id: 'tape.drive', file: 'tape.drive.knl' }, /must be 1 to 15/, 'tape.drive');
    expect(parse({ id: 'drum-2', file: 'drum-2.op1p' }, 'drum-2').issues).toEqual([]);
  });

  it('applies the opus id rule', () => {
    const opus = (id: string) => parsePluginManifest({ ...opusValid(), id }, id);
    expect(opus('a.b_c-d').issues).toEqual([]);
    expect(opus('x'.repeat(40)).issues).toEqual([]);
    expect(messages(opus('x'.repeat(41)).issues)).toMatch(/must be 1 to 40/);
    expect(messages(opus('Big').issues)).toMatch(/must be 1 to 40/);
    expect(messages(opus('.hidden').issues)).toMatch(/must not start with a dot/);
  });

  it('refuses an id Windows cannot use as a name', () => {
    rejects({ id: 'con', file: 'con.knl' }, /reserved by Windows/, 'con');
  });

  it('requires the folder name to equal the id', () => {
    rejects({}, /id "tape_drive" must equal the folder name "other"/, 'other');
  });

  it('checks text fields and their lengths', () => {
    rejects({ name: '' }, /name must be 1 to 60/);
    rejects({ name: 3 }, /name must be a string/);
    rejects({ author: 'x'.repeat(61) }, /author must be 1 to 60/);
    rejects({ version: 'x'.repeat(21) }, /version must be 1 to 20/);
    rejects({ description: 'x'.repeat(501) }, /description must be 1 to 500/);
    rejects({ requires: 'x'.repeat(61) }, /requires must be 1 to 60/);
    rejects({ credits: 'x'.repeat(201) }, /credits must be 1 to 200/);
    rejects({ credits: 5 }, /credits must be a string/);
  });

  it('checks firmware, kind and license against their lists', () => {
    rejects({ firmware: 'op1/stock' }, /firmware "op1\/stock" must be one of op1\/hundekraft, op1\/opus/);
    rejects({ kind: 'drum' }, /kind "drum" must be one of synth, fx, feature/);
    rejects({ license: 'CC0-1.0' }, /license "CC0-1.0" must be one of GPL-3.0-or-later/);
    for (const license of PLUGIN_LICENSES) expect(parse({ license }).issues).toEqual([]);
  });

  it('names the plugin file after the id for hundekraft', () => {
    rejects({ file: 'drive.knl' }, /file must be "tape_drive.knl" or "tape_drive.op1p"/);
    rejects({ file: 'tape_drive.opmod' }, /file must be "tape_drive.knl" or "tape_drive.op1p"/);
    expect(parse({ file: 'tape_drive.op1p' }).issues).toEqual([]);
  });

  it('refuses an .op1p feature: op1r plugins are synths or effects', () => {
    rejects({ file: 'tape_drive.op1p', kind: 'feature' }, /an \.op1p plugin is a synth or an fx/);
  });

  it('requires an .opmod file for opus', () => {
    const opus = (file: string) => parsePluginManifest({ ...opusValid(), file }, 'opus.chorus-2');
    expect(messages(opus('opus.chorus-2.knl').issues)).toMatch(/file must be a name ending in \.opmod/);
    expect(messages(opus('.opmod').issues)).toMatch(/file must be a name ending in \.opmod/);
    expect(messages(opus('a/b.opmod').issues)).toMatch(/file must be a name ending in \.opmod/);
    expect(opus('Chorus_2.opmod').issues).toEqual([]);
  });

  it('checks the screenshot and manual names', () => {
    rejects({ screenshot: 'screen.jpg' }, /screenshot must be a \.png file name/);
    rejects({ screenshot: 'img/screen.png' }, /screenshot must be a \.png file name/);
    rejects({ screenshot: 'manual.md' }, /screenshot must be a \.png file name/);
    rejects({ manual: 'README.md' }, /manual must be "manual.md"/);
  });

  it('accepts only an https source link', () => {
    rejects({ source: 'http://example.com' }, /source must be an https:\/\/ link/);
    rejects({ source: 'javascript:alert(1)' }, /source must be an https:\/\/ link/);
    rejects({ source: 'not a url' }, /source must be an https:\/\/ link/);
    rejects({ source: `https://example.com/${'x'.repeat(200)}` }, /source must be at most 200/);
  });

  it('applies the packs tag rule', () => {
    rejects({ tags: 'tape' }, /tags must be a list/);
    rejects({ tags: ['Tape'] }, /tag "Tape" must be 1 to 20/);
    rejects({ tags: ['a', 'a'] }, /duplicate tag "a"/);
    rejects({ tags: ['a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i'] }, /at most 8 tags/);
  });

  it('allows savedName only on opus, as opus.<id>', () => {
    rejects({ savedName: 'opus.tape' }, /savedName is only for op1\/opus plugins/);
    const opus = (savedName: unknown) => parsePluginManifest({ ...opusValid(), savedName }, 'opus.chorus-2');
    expect(messages(opus('chorus').issues)).toMatch(/savedName "chorus" must be opus\.<id>/);
    expect(messages(opus(1).issues)).toMatch(/savedName 1 must be opus\.<id>/);
  });

  it('reports every problem, not just the first', () => {
    const issues = rejects({ name: '', author: '', kind: 'x' }, /name must be/);
    expect(issues.length).toBe(3);
  });
});

describe('checkKnl', () => {
  it('reads a valid file', () => {
    expect(checkKnl(knl())).toEqual({ problems: [], info: { kind: 'fx', savedName: 'tape drive' } });
  });

  it('reads a file with a screen SVG in front', () => {
    const svg = '<svg viewBox="0 0 320 240"><path d="M1.5 2.25"/></svg>';
    expect(checkKnl(knl({ svg, kind: 1, name: 'rings' }))).toEqual({ problems: [], info: { kind: 'synth', savedName: 'rings' } });
  });

  it('reads author and version from the extension table', () => {
    // Claims are NUL-separated and end in an empty name; unknown tags are skipped.
    const bytes = knl({ kind: 3, name: 'grids', ext: [[1, 'tempo\0'], [2, 'coldsoldr'], [3, '1.2'], [9, 'future']] });
    expect(checkKnl(bytes)).toEqual({ problems: [], info: { kind: 'feature', savedName: 'grids', author: 'coldsoldr', version: '1.2' } });
  });

  it('refuses the SVG decimal boot hazard', () => {
    const svg = '<svg><path d="M1.123456 2"/></svg>';
    expect(checkKnl(knl({ svg })).problems.join('\n')).toMatch(/more than four decimals \("1\.123456"\).*hangs/);
    expect(checkKnl(knl({ svg: '<svg><path d="M1.1234 2"/></svg>' })).problems).toEqual([]);
  });

  it('refuses an SVG without its NUL, or with padding that is not zero', () => {
    expect(checkKnl(ascii('<svg></svg>')).problems).toEqual(['starts with a screen SVG that never ends in a NUL']);
    const bytes = knlWith((b) => (b[7] = 7), { svg: '<svg/>' });
    expect(checkKnl(bytes).problems).toContain('the padding after the screen SVG is not zero');
  });

  it('checks the header fields', () => {
    const problems = (change: (b: Uint8Array, h: number) => void) => checkKnl(knlWith(change)).problems;
    expect(problems((b) => b.set(ascii('KNPX'), 0))).toEqual(['no KNPL header (not a kennel plugin)']);
    expect(problems((b) => setU16(b, 4, 2))).toEqual(['format 2 is not 1']);
    expect(problems((b) => setU16(b, 6, 140))).toEqual(['header size 140 is under 144 bytes']);
    expect(problems((b) => setU16(b, 6, 4000))).toEqual(['header size 4000 runs past the end of the file']);
    expect(problems((b) => setU32(b, 8, 99))).toEqual([`the header says 99 bytes but the file is ${knl().length}`]);
    expect(problems((b) => setU16(b, 16, 2))).toEqual(['plugin API 2 is not 1']);
    expect(problems((b) => (b[18] = 0))).toEqual(['kind 0 is not 1 (synth), 2 (fx) or 3 (feature)']);
    expect(problems((b) => (b[18] = 4))).toEqual(['kind 4 is not 1 (synth), 2 (fx) or 3 (feature)']);
    expect(problems((b) => (b[19] = 32))).toEqual(['legacy order 32 is over 31']);
    expect(problems((b) => (b[19] = 31))).toEqual([]);
  });

  it('checks the svg size field against the prefix', () => {
    expect(checkKnl(knlWith((b) => setU32(b, 132, 5))).problems).toEqual(['the header says the screen SVG is 5 bytes but the file has none']);
    const svg = '<svg></svg>';
    expect(checkKnl(knlWith((b, h) => setU32(b, h + 132, 3), { svg })).problems).toEqual([
      `the header says the screen SVG is 3 bytes but the file starts with ${svg.length + 1}`,
    ]);
  });

  it('checks the reserved SVG fields', () => {
    const reserved = ['the reserved SVG fields (header bytes 128 and 136) are not 0 and 0xFFFFFFFF'];
    expect(checkKnl(knlWith((b) => setU32(b, 128, 4))).problems).toEqual(reserved);
    expect(checkKnl(knlWith((b) => setU32(b, 136, 0))).problems).toEqual(reserved);
  });

  it('accepts an SVG size that takes in the zero padding', () => {
    expect(checkKnl(knlWith((b) => setU32(b, 132, 8), { svg: '<svg/>' })).problems).toEqual([]);
  });

  it('checks the CRC with its own four bytes as zero', () => {
    const bytes = knl();
    bytes[150] = 0xaa;
    expect(checkKnl(bytes).problems).toEqual([expect.stringMatching(/^CRC-32 is [0-9a-f]{8} but the header says [0-9a-f]{8}$/)]);
  });

  it('checks the file length', () => {
    const odd = knl({ body: 14 });
    expect(checkKnl(odd).problems).toEqual([`${odd.length} bytes is not a multiple of 4`]);
    const big = knl({ body: MAX_KNL_BYTES });
    expect(checkKnl(big).problems).toEqual([expect.stringMatching(/over the 512 KB \(524288 bytes\) limit/)]);
    expect(checkKnl(new Uint8Array(100)).problems).toEqual(['100 bytes is too short for the 144-byte header']);
    expect(checkKnl(new Uint8Array(0)).problems).toEqual(['0 bytes is too short for the 144-byte header']);
  });

  it('checks the saved name', () => {
    const problems = (change: (b: Uint8Array) => void) => checkKnl(knlWith(change)).problems;
    expect(problems((b) => b.fill(0, 20, 36))).toEqual(['the saved name is empty']);
    expect(problems((b) => b.fill(0x61, 20, 36))).toEqual(['the saved name does not end in a NUL within 16 bytes']);
    expect(problems((b) => (b[21] = 0x09))).toEqual(['the saved name must be printable ASCII']);
    expect(problems((b) => (b[21] = 0xe9))).toEqual(['the saved name must be printable ASCII']);
  });

  it('checks the extension table bounds and entries', () => {
    const ext: [number, string][] = [[2, 'me']];
    const problems = (change: (b: Uint8Array) => void) => checkKnl(knlWith(change, { ext })).problems;
    const at = 144 + 16;
    expect(problems((b) => setU32(b, 140, 2))).toEqual(['the extension table offset 2 is outside the file or not 4-aligned']);
    expect(problems((b) => setU32(b, 140, 4_000_000))).toEqual(['the extension table offset 4000000 is outside the file or not 4-aligned']);
    expect(problems((b) => setU32(b, 140, at + 1))).toEqual([`the extension table offset ${at + 1} is outside the file or not 4-aligned`]);
    expect(problems((b) => setU16(b, at + 2, 400))).toEqual(['an extension table entry runs past the end of the file']);
    expect(problems((b) => (b[at + 6] = 0x41))).toEqual(['extension entry 2 is not NUL-terminated ASCII']);
    expect(problems((b) => (b[at + 4] = 0xc3))).toEqual(['extension entry 2 is not NUL-terminated ASCII']);
    expect(checkKnl(knl({ ext: [[1, 'tempo']] })).problems).toEqual(['the claims list in the extension table does not end in an empty name']);
    // The terminator replaced by an empty entry: the walk must stop at the end of the file, not read past it.
    expect(problems((b) => setU16(b, at + 8, 9))).toEqual(['the extension table runs past the end of the file']);
  });
});

describe('checkOp1p', () => {
  it('reads a valid file', () => {
    expect(checkOp1p(op1p())).toEqual({ problems: [], info: { kind: 'fx', savedName: 'tape drive' } });
    expect(checkOp1p(op1p({ kind: 0, name: 'saw' })).info).toEqual({ kind: 'synth', savedName: 'saw' });
  });

  it('reads a file with an SVG prefix and refuses the decimal hazard in it', () => {
    expect(checkOp1p(op1p({ svg: '<svg/>' })).problems).toEqual([]);
    expect(checkOp1p(op1p({ svg: '<svg x="0.000001"/>' })).problems.join('\n')).toMatch(/more than four decimals \("0\.000001"\)/);
  });

  it('checks the trailer', () => {
    expect(checkOp1p(new Uint8Array(8)).problems).toEqual(['8 bytes is too short for the 16-byte op1r trailer']);
    const trailer = (change: (b: Uint8Array, t: number) => void) => {
      const bytes = op1p();
      change(bytes, bytes.length - 16);
      return checkOp1p(bytes).problems;
    };
    expect(trailer((b, t) => setU32(b, t, 0))).toEqual(['no op1r trailer at the end of the file (not an .op1p plugin)']);
    expect(trailer((b, t) => setU32(b, t + 4, 2))).toEqual(['the binary offset 2 is not 4-aligned']);
    expect(trailer((b, t) => setU32(b, t + 8, 40))).toEqual(['the binary is 40 bytes, under 76']);
    expect(trailer((b, t) => setU32(b, t + 8, 0xfffffff0))).toEqual(['the binary runs past the trailer']);
    expect(trailer((b, t) => setU32(b, t + 4, 0xfffffff0))).toEqual(['the binary runs past the trailer']);
    expect(trailer((b, t) => setU32(b, t + 12, 1))).toEqual([expect.stringMatching(/^the binary's CRC-32 is [0-9a-f]{8} but the trailer says 00000001$/)]);
  });

  it('checks the binary header', () => {
    const problems = (change: (b: Uint8Array, at: number) => void) => checkOp1p(op1pWith(change)).problems;
    expect(problems((b, at) => setU32(b, at, 0))).toEqual(['the binary does not start with the op1r plugin magic']);
    expect(problems((b, at) => setU32(b, at + 4, 2))).toEqual(['binary format 2 is not 1']);
    expect(problems((b, at) => setU32(b, at + 8, 3))).toEqual(['plugin API 3 is not 1']);
    expect(problems((b, at) => setU32(b, at + 12, 2))).toEqual(['kind 2 is not 0 (synth) or 1 (fx)']);
    expect(problems((b, at) => b.fill(0, at + 16, at + 32))).toEqual(['the saved name is empty']);
    expect(problems((b, at) => (b[at + 16] = 0x41))).toEqual(['the saved name must be lower-case letters, digits, spaces, _ or -']);
    expect(problems((b, at) => b.fill(0x61, at + 16, at + 32))).toEqual(['the saved name does not end in a NUL within 16 bytes']);
  });

  it('limits the binary to 512 KB', () => {
    expect(checkOp1p(op1p({ binary: MAX_KNL_BYTES + 4 })).problems).toEqual([expect.stringMatching(/^the binary is 524292 bytes, over the 512 KB/)]);
  });
});

describe('checkOpmod', () => {
  it('needs the OPMD magic and at most 2 MB', () => {
    expect(checkOpmod(ascii('OPMD\0\0\0\0'))).toEqual([]);
    expect(checkOpmod(ascii('OPMX'))).toEqual(['does not start with OPMD (not an opus plugin)']);
    expect(checkOpmod(ascii('OP'))).toEqual(['does not start with OPMD (not an opus plugin)']);
    const big = new Uint8Array(MAX_OPMOD_BYTES + 1);
    big.set(ascii('OPMD'));
    expect(checkOpmod(big)).toEqual([expect.stringMatching(/over the 2 MB \(2097152 bytes\) limit/)]);
  });
});

describe('checkScreenshot', () => {
  it('accepts a PNG between 64 and 2048 pixels each way', () => {
    expect(checkScreenshot(png(320, 240))).toEqual([]);
    expect(checkScreenshot(png(64, 2048))).toEqual([]);
  });

  it('refuses other files and sizes', () => {
    expect(checkScreenshot(ascii('GIF89a'))).toEqual(['not a PNG image']);
    const noIhdr = png(320, 240);
    noIhdr.set(ascii('IDAT'), 12);
    expect(checkScreenshot(noIhdr)).toEqual(['not a PNG image']);
    expect(checkScreenshot(png(63, 240))).toEqual(['is 63 x 240 pixels; width and height must each be 64 to 2048']);
    expect(checkScreenshot(png(320, 2049))).toEqual(['is 320 x 2049 pixels; width and height must each be 64 to 2048']);
    expect(checkScreenshot(png(320, 240, MAX_SCREENSHOT_BYTES))).toEqual([expect.stringMatching(/over the 1 MB \(1048576 bytes\) limit/)]);
  });
});

describe('checkManual', () => {
  it('accepts UTF-8 text', () => {
    expect(checkManual(Uint8Array.from([0x23, 0x20, 0xc3, 0xa9, 0xe2, 0x82, 0xac, 0xf0, 0x9f, 0x8e, 0xb9]))).toEqual([]);
  });

  it('refuses an empty or blank manual', () => {
    expect(checkManual(new Uint8Array(0))).toEqual(['is empty']);
    expect(checkManual(ascii(' \n\t\r\n'))).toEqual(['is empty']);
  });

  it('refuses bytes that are not UTF-8', () => {
    for (const bad of [[0xe9], [0xc3], [0xc0, 0x80], [0xed, 0xa0, 0x80], [0xf4, 0x90, 0x80, 0x80], [0x80], [0xe2, 0x82]]) {
      expect(checkManual(Uint8Array.from([0x61, ...bad]))).toEqual(['is not valid UTF-8']);
    }
  });

  it('limits the manual to 64 KB', () => {
    expect(checkManual(new Uint8Array(64 * 1024 + 1).fill(0x61))).toEqual([expect.stringMatching(/over the 64 KB \(65536 bytes\) limit/)]);
  });
});

describe('validatePluginFiles', () => {
  const manifest = (overrides: Record<string, unknown> = {}, folder = 'tape_drive'): PluginManifest => {
    const { manifest: m, issues } = parsePluginManifest({ ...valid(), ...overrides }, folder);
    expect(issues).toEqual([]);
    return m as PluginManifest;
  };

  const folder = (files: Record<string, Uint8Array>) => ({
    files: Object.entries(files).map(([path, bytes]) => ({ path, size: bytes.length })),
    read: async (path: string): Promise<Uint8Array> => {
      const bytes = files[path];
      if (!bytes) throw new Error(`no ${path}`);
      return bytes;
    },
  });

  const good = (plugin: Uint8Array = knl(), file = 'tape_drive.knl'): Record<string, Uint8Array> => ({
    'plugin.json': ascii('{}'),
    [file]: plugin,
    'manual.md': ascii('# Tape drive\n'),
    'screenshot.png': png(320, 240),
  });

  async function check(m: PluginManifest, files: Record<string, Uint8Array>) {
    const { files: listing, read } = folder(files);
    return validatePluginFiles(m, listing, read);
  }

  it('accepts a complete hundekraft plugin and returns what its file says', async () => {
    const result = await check(manifest(), good());
    const info = { kind: 'fx', savedName: 'tape drive' };
    expect(result).toEqual({ issues: [], warnings: [], info, infos: [info] });
  });

  it('ignores OS clutter', async () => {
    const files = { ...good(), '.DS_Store': ascii('x'), 'Thumbs.db': ascii('x') };
    expect((await check(manifest(), files)).issues).toEqual([]);
  });

  it('reports missing and extra files', async () => {
    const files = good();
    delete files['manual.md'];
    files['notes.txt'] = ascii('hi');
    files['src/main.c'] = ascii('int x;');
    const { issues } = await check(manifest(), files);
    expect(issues).toEqual([
      { plugin: 'tape_drive', file: 'manual.md', message: 'missing' },
      { plugin: 'tape_drive', file: 'notes.txt', message: 'not part of a plugin folder; it holds plugin.json, tape_drive.knl, manual.md and screenshot.png only' },
      { plugin: 'tape_drive', file: 'src/main.c', message: 'not part of a plugin folder; it holds plugin.json, tape_drive.knl, manual.md and screenshot.png only' },
    ]);
  });

  it('points out a spelling that differs only in case', async () => {
    const files = good();
    files['Manual.md'] = files['manual.md'] as Uint8Array;
    delete files['manual.md'];
    expect((await check(manifest(), files)).issues).toEqual([
      { plugin: 'tape_drive', file: 'manual.md', message: 'missing; the folder has Manual.md, which differs only in case' },
    ]);
  });

  it('checks sizes before reading', async () => {
    const files = good();
    const { files: listing, read } = folder(files);
    const sized = listing.map((f) => (f.path === 'tape_drive.knl' ? { ...f, size: MAX_KNL_BYTES + 4 } : f.path === 'plugin.json' ? { ...f, size: 70000 } : f));
    const { issues } = await validatePluginFiles(manifest(), sized, async (path) => {
      if (path === 'tape_drive.knl') throw new Error('should not be read');
      return read(path);
    });
    expect(issues).toEqual([
      { plugin: 'tape_drive', file: 'plugin.json', message: 'is 70000 bytes, over the 64 KB (65536 bytes) limit' },
      { plugin: 'tape_drive', file: 'tape_drive.knl', message: 'is 524292 bytes, over the 512 KB (524288 bytes) limit' },
    ]);
  });

  it('passes on problems in the plugin file, the manual and the screenshot', async () => {
    const files = { ...good(knlWith((b) => setU16(b, 16, 2))), 'manual.md': new Uint8Array(0), 'screenshot.png': png(10, 10) };
    expect((await check(manifest(), files)).issues).toEqual([
      { plugin: 'tape_drive', file: 'tape_drive.knl', message: 'plugin API 2 is not 1' },
      { plugin: 'tape_drive', file: 'manual.md', message: 'is empty' },
      { plugin: 'tape_drive', file: 'screenshot.png', message: 'is 10 x 10 pixels; width and height must each be 64 to 2048' },
    ]);
  });

  it('requires the id to follow from the saved name', async () => {
    const { issues } = await check(manifest({ id: 'tapedrive', file: 'tapedrive.knl' }, 'tapedrive'), good(knl(), 'tapedrive.knl'));
    expect(issues).toEqual([
      {
        plugin: 'tapedrive',
        file: 'tapedrive.knl',
        message: 'the saved name "tape drive" makes the id "tape_drive"; the id, folder and file must use it',
      },
    ]);
  });

  it('maps drum+ to the id drumplus', async () => {
    const m = manifest({ id: 'drumplus', file: 'drumplus.knl', kind: 'synth' }, 'drumplus');
    expect((await check(m, good(knl({ kind: 1, name: 'drum+' }), 'drumplus.knl'))).issues).toEqual([]);
  });

  it('refuses a saved name that cannot make an id', async () => {
    const m = manifest({ id: 'tape', file: 'tape.knl' }, 'tape');
    const { issues } = await check(m, good(knl({ name: 'Tape!' }), 'tape.knl'));
    expect(messages(issues)).toMatch(/the saved name "Tape!" makes the id "Tape!", which is not 1 to 15/);
  });

  it('requires plugin.json kind to match the header', async () => {
    const { issues } = await check(manifest({ kind: 'synth' }), good());
    expect(issues).toEqual([{ plugin: 'tape_drive', file: 'tape_drive.knl', message: "the file's kind is fx but plugin.json says synth" }]);
  });

  it('warns, without failing, when the extension table disagrees with plugin.json', async () => {
    const result = await check(manifest(), good(knl({ ext: [[2, 'someone'], [3, '0.9']] })));
    expect(result.issues).toEqual([]);
    expect(result.warnings).toEqual([
      { plugin: 'tape_drive', file: 'tape_drive.knl', message: 'the file says author "someone" but plugin.json says "coldsoldr"' },
      { plugin: 'tape_drive', file: 'tape_drive.knl', message: 'the file says version "0.9" but plugin.json says "1.0"' },
    ]);
    expect(result.info).toEqual({ kind: 'fx', savedName: 'tape drive', author: 'someone', version: '0.9' });
  });

  it('checks an .op1p plugin the same way', async () => {
    const m = manifest({ file: 'tape_drive.op1p' });
    expect((await check(m, good(op1p(), 'tape_drive.op1p'))).issues).toEqual([]);
    const wrong = await check(m, good(op1p({ kind: 0 }), 'tape_drive.op1p'));
    expect(messages(wrong.issues)).toBe("the file's kind is synth but plugin.json says fx");
  });

  it('checks an opus plugin', async () => {
    const { manifest: m } = parsePluginManifest(opusValid(), 'opus.chorus-2');
    const files = { 'plugin.json': ascii('{}'), '10_chorus.opmod': ascii('OPMD1234'), 'manual.md': ascii('x'), 'screen.png': png(64, 64) };
    expect(await check(m as PluginManifest, files)).toEqual({ issues: [], warnings: [], info: {}, infos: [{}] });
    const bad = await check(m as PluginManifest, { ...files, '10_chorus.opmod': ascii('MZ') });
    expect(messages(bad.issues)).toBe('does not start with OPMD (not an opus plugin)');
  });
});

describe('pluginIndexEntry', () => {
  it('adds the file facts and paths relative to the plugins folder', () => {
    const { manifest } = parsePluginManifest(valid(), 'tape_drive');
    const entry = pluginIndexEntry(manifest as PluginManifest, { sizeBytes: 100, sha256: 'ab' }, { kind: 'fx', savedName: 'tape drive', author: 'x', version: '2' });
    const { format: _format, ...fields } = valid();
    expect(entry).toEqual({
      ...fields,
      screenshot: 'tape_drive/screenshot.png',
      manual: 'tape_drive/manual.md',
      sizeBytes: 100,
      sha256: 'ab',
      savedName: 'tape drive',
      headerAuthor: 'x',
      headerVersion: '2',
    });
  });

  it('takes savedName from plugin.json for opus and leaves out what is absent', () => {
    const { manifest } = parsePluginManifest(opusValid(), 'opus.chorus-2');
    const entry = pluginIndexEntry(manifest as PluginManifest, { sizeBytes: 8, sha256: 'cd' }, {});
    expect(entry.savedName).toBe('opus.chorus');
    expect(Object.keys(entry)).not.toContain('headerAuthor');
    const { savedName: _drop, ...noSavedName } = manifest as PluginManifest;
    const without = pluginIndexEntry(noSavedName, { sizeBytes: 8, sha256: 'cd' }, {});
    expect(Object.keys(without)).not.toContain('savedName');
  });
});

describe('coop plugins', () => {
  /** A valid .coop: kennel's layout with the magic COOP (firmware/docs/plugins.md). */
  const coop = (options: KnlOptions = {}): Uint8Array =>
    knlWith((b, h) => b.set(ascii('COOP'), h), { kind: 1, name: 'saw', ...options });

  it('reads a coop synth engine and a coop effect', () => {
    expect(checkCoop(coop())).toEqual({ problems: [], info: { kind: 'synth', savedName: 'saw' } });
    expect(checkCoop(coop({ kind: 2, name: 'stereo delay' })).info).toEqual({ kind: 'fx', savedName: 'stereo delay' });
  });

  it('refuses a kennel file, and a coop feature (coop has none)', () => {
    expect(checkCoop(knl()).problems).toEqual(['no COOP header (not a coop plugin)']);
    expect(checkCoop(coop({ kind: 3 })).problems).toEqual(['kind 3 is not 1 (synth) or 2 (fx)']);
  });

  it('accepts a coop manifest whose file is <id>.coop', () => {
    const { issues } = parse({ firmware: 'op1/coop', id: 'saw', file: 'saw.coop', kind: 'synth', requires: 'coop e2' }, 'saw');
    expect(issues).toEqual([]);
  });

  it('refuses a coop manifest with another file name or a feature kind', () => {
    rejects({ firmware: 'op1/coop', id: 'saw', file: 'saw.knl', kind: 'synth' }, /file must be "saw.coop"/, 'saw');
    rejects({ firmware: 'op1/coop', id: 'saw', file: 'saw.coop', kind: 'feature' }, /synth or an fx/, 'saw');
    rejects({ firmware: 'op1/coop', id: 'my-saw', file: 'my-saw.coop', kind: 'synth' }, /lower-case letters, digits or _/, 'my-saw');
  });
});

describe('plugins with a file per firmware', () => {
  const coopFile = (options: KnlOptions = {}): Uint8Array =>
    knlWith((b, h) => b.set(ascii('COOP'), h), { kind: 1, name: 'rings', ...options });
  /** A rings manifest with a hundekraft and a coop build. */
  const ringsJson = (overrides: Record<string, unknown> = {}): Record<string, unknown> => {
    const json: Record<string, unknown> = {
      ...valid(),
      id: 'rings',
      name: 'rings',
      kind: 'synth',
      builds: [
        { firmware: 'op1/hundekraft', file: 'rings.knl', version: '0.1', requires: 'hundekraft 3.0.3' },
        { firmware: 'op1/coop', file: 'rings.coop', version: '0.5', requires: 'coop e5', source: 'https://example.com/coop' },
      ],
      ...overrides,
    };
    for (const key of ['firmware', 'file', 'version', 'requires']) if (!(key in overrides)) delete json[key];
    return json;
  };
  const parseRings = (overrides: Record<string, unknown> = {}) => parsePluginManifest(ringsJson(overrides), 'rings');

  it('parses the builds, with the shared source filled in, and the first at the top', () => {
    const { manifest, issues } = parseRings();
    expect(issues).toEqual([]);
    const source = 'https://github.com/coldsoldr/te-op1-cfw';
    expect(manifest?.builds).toEqual([
      { firmware: 'op1/hundekraft', file: 'rings.knl', version: '0.1', requires: 'hundekraft 3.0.3', source },
      { firmware: 'op1/coop', file: 'rings.coop', version: '0.5', requires: 'coop e5', source: 'https://example.com/coop' },
    ]);
    expect(manifest).toMatchObject({ firmware: 'op1/hundekraft', file: 'rings.knl', version: '0.1', requires: 'hundekraft 3.0.3' });
    const [, coop] = manifestBuilds(manifest as PluginManifest);
    expect(coop).toMatchObject({ id: 'rings', firmware: 'op1/coop', file: 'rings.coop', version: '0.5', requires: 'coop e5' });
    expect(coop).not.toHaveProperty('builds');
  });

  it('refuses build fields at the top beside builds', () => {
    const { issues } = parseRings({ firmware: 'op1/coop', version: '1' });
    expect(messages(issues)).toMatch(/firmware goes in each of builds/);
    expect(messages(issues)).toMatch(/version goes in each of builds/);
  });

  it('checks each build against its firmware', () => {
    const bad = (builds: unknown, pattern: RegExp): void => {
      const { manifest, issues } = parseRings({ builds });
      expect(manifest).toBeNull();
      expect(messages(issues)).toMatch(pattern);
    };
    bad([], /builds must be a list of 1 to 3/);
    bad('x', /builds must be a list/);
    bad([{ firmware: 'op1/coop', file: 'rings.knl', version: '1' }], /builds\[1\]: file must be "rings.coop"/);
    bad([{ firmware: 'op1/coop', file: 'rings.coop' }], /builds\[1\]: version must be a string/);
    bad([{ firmware: 'op1/coop', file: 'rings.coop', version: '1', extra: 1 }], /builds\[1\]: unknown field "extra"/);
    bad(
      [{ firmware: 'op1/coop', file: 'rings.coop', version: '1' }, { firmware: 'op1/coop', file: 'rings.coop', version: '2' }],
      /builds\[2\]: a second build for op1\/coop/,
    );
    bad([{ firmware: 'op1/coop', file: 'rings.coop', version: '1', savedName: 'opus.x' }], /builds\[1\]: savedName is only for op1\/opus/);
    bad(['x'], /builds\[1\]: a build must be a JSON object/);
  });

  it('checks every build file and indexes them all', async () => {
    const { manifest } = parseRings();
    const m = manifest as PluginManifest;
    const files: Record<string, Uint8Array> = {
      'plugin.json': ascii('{}'),
      'rings.knl': knl({ kind: 1, name: 'rings' }),
      'rings.coop': coopFile({ ext: [[3, '0.4']] }),
      'manual.md': ascii('# rings\n'),
      'screenshot.png': png(320, 240),
    };
    const listing = Object.entries(files).map(([path, bytes]) => ({ path, size: bytes.length }));
    const read = async (path: string): Promise<Uint8Array> => files[path] as Uint8Array;
    const result = await validatePluginFiles(m, listing, read);
    expect(result.issues).toEqual([]);
    expect(result.warnings).toEqual([
      { plugin: 'rings', file: 'rings.coop', message: 'the file says version "0.4" but plugin.json says "0.5"' },
    ]);
    expect(result.infos).toEqual([
      { kind: 'synth', savedName: 'rings' },
      { kind: 'synth', savedName: 'rings', version: '0.4' },
    ]);

    const missing = await validatePluginFiles(m, listing.filter((f) => f.path !== 'rings.coop'), read);
    expect(missing.issues).toEqual([{ plugin: 'rings', file: 'rings.coop', message: 'missing' }]);
    expect(missing.infos).toBeNull();

    const entry = pluginIndexEntryOfBuilds(m, [
      { sizeBytes: 10, sha256: 'aa', info: { kind: 'synth', savedName: 'rings' } },
      { sizeBytes: 20, sha256: 'bb', info: { kind: 'synth', savedName: 'rings', version: '0.4' } },
    ]);
    expect(entry).toMatchObject({ id: 'rings', firmware: 'op1/hundekraft', file: 'rings.knl', sizeBytes: 10, savedName: 'rings' });
    expect(entry.builds).toEqual([
      { firmware: 'op1/hundekraft', file: 'rings.knl', version: '0.1', requires: 'hundekraft 3.0.3', source: 'https://github.com/coldsoldr/te-op1-cfw', sizeBytes: 10, sha256: 'aa', savedName: 'rings' },
      { firmware: 'op1/coop', file: 'rings.coop', version: '0.5', requires: 'coop e5', source: 'https://example.com/coop', sizeBytes: 20, sha256: 'bb', savedName: 'rings', headerVersion: '0.4' },
    ]);

    expect(pluginBuilds(entry).map((b) => b.firmware)).toEqual(['op1/hundekraft', 'op1/coop']);
    expect(pluginFor(entry, 'op1/hundekraft')).toMatchObject({ file: 'rings.knl', version: '0.1' });
    const coop = pluginFor(entry, 'op1/coop');
    expect(coop).toMatchObject({ id: 'rings', name: 'rings', file: 'rings.coop', version: '0.5', requires: 'coop e5', sizeBytes: 20, sha256: 'bb', headerVersion: '0.4' });
    expect(coop?.builds).toEqual(entry.builds);
    expect(pluginFor(entry, 'op1/opus')).toBeNull();
  });

  it('leaves a single-firmware entry as it is', () => {
    const { manifest } = parsePluginManifest(valid(), 'tape_drive');
    const entry = pluginIndexEntryOfBuilds(manifest as PluginManifest, [{ sizeBytes: 1, sha256: 'ab', info: { kind: 'fx', savedName: 'tape drive' } }]);
    expect(entry).not.toHaveProperty('builds');
    expect(pluginFor(entry, 'op1/hundekraft')).toBe(entry);
    expect(pluginFor(entry, 'op1/coop')).toBeNull();
    expect(pluginBuilds(entry)).toEqual([
      { firmware: 'op1/hundekraft', file: 'tape_drive.knl', version: '1.0', requires: 'hundekraft 3.0.3', source: 'https://github.com/coldsoldr/te-op1-cfw', sizeBytes: 1, sha256: 'ab', savedName: 'tape drive' },
    ]);
  });
});

describe('listed plugins (a link, no file)', () => {
  const listed = (overrides: Record<string, unknown> = {}): Record<string, unknown> => {
    const m: Record<string, unknown> = {
      ...valid(),
      id: 'moai',
      name: 'Moai',
      kind: 'synth',
      author: 'fungiboletus',
      link: 'https://hundekraft.github.io/op1-workshop/',
      ...overrides,
    };
    delete m.file;
    delete m.version;
    delete m.license;
    for (const [k, v] of Object.entries(overrides)) m[k] = v;
    for (const k of Object.keys(m)) if (m[k] === undefined) delete m[k];
    return m;
  };

  it('accepts a link in place of a file, with no version or licence', () => {
    const { manifest, issues } = parsePluginManifest(listed(), 'moai');
    expect(issues).toEqual([]);
    expect(manifest).toMatchObject({ id: 'moai', link: 'https://hundekraft.github.io/op1-workshop/' });
    expect(manifest).not.toHaveProperty('file');
    expect(manifest).not.toHaveProperty('license');
    expect(manifest).not.toHaveProperty('version');
  });

  it('keeps a licence and version when given', () => {
    const { manifest } = parsePluginManifest(listed({ license: 'GPL-3.0-or-later', version: '0.2' }), 'moai');
    expect(manifest).toMatchObject({ license: 'GPL-3.0-or-later', version: '0.2' });
  });

  it('refuses a link with a file, or a link that is not https', () => {
    expect(messages(parsePluginManifest(listed({ file: 'moai.knl' }), 'moai').issues)).toMatch(/listed build \(with a link\) has no file/);
    expect(messages(parsePluginManifest(listed({ link: 'http://example.com/' }), 'moai').issues)).toMatch(/link must be an https:\/\/ link/);
  });

  it('still needs a licence while any build hosts a file', () => {
    const m = listed({ link: undefined, builds: [
      { firmware: 'op1/hundekraft', link: 'https://hundekraft.github.io/op1-workshop/' },
      { firmware: 'op1/coop', file: 'moai.coop', version: '0.1' },
    ] });
    delete m.firmware;
    delete m.requires;
    delete m.link;
    expect(messages(parsePluginManifest(m, 'moai').issues)).toMatch(/license undefined must be one of/);
  });

  it('needs only plugin.json, the manual and the screenshot in its folder', async () => {
    const { manifest } = parsePluginManifest(listed(), 'moai');
    const files: Record<string, Uint8Array> = {
      'plugin.json': ascii('{}'),
      'manual.md': ascii('# Moai\n'),
      'screenshot.png': png(320, 240),
    };
    const listing = Object.entries(files).map(([path, bytes]) => ({ path, size: bytes.length }));
    const result = await validatePluginFiles(manifest as PluginManifest, listing, async (path) => files[path] as Uint8Array);
    expect(result).toEqual({ issues: [], warnings: [], info: {}, infos: [{}] });

    const extra = [...listing, { path: 'moai.knl', size: 4 }];
    const refused = await validatePluginFiles(manifest as PluginManifest, extra, async (path) => files[path] ?? new Uint8Array(4));
    expect(messages(refused.issues)).toMatch(/not part of a plugin folder/);
  });

  it('indexes it with the link and no size or hash, and is not hosted', () => {
    const { manifest } = parsePluginManifest(listed(), 'moai');
    const entry = pluginIndexEntryOfBuilds(manifest as PluginManifest, [null]);
    expect(entry).toMatchObject({ id: 'moai', link: 'https://hundekraft.github.io/op1-workshop/', manual: 'moai/manual.md' });
    expect(entry).not.toHaveProperty('sizeBytes');
    expect(entry).not.toHaveProperty('sha256');
    expect(isHosted(entry)).toBe(false);
  });
});

describe('listed plugins with a fileUrl (installed from the author over USB)', () => {
  const commit = '0123456789abcdef0123456789abcdef01234567';
  const url = `https://raw.githubusercontent.com/someone/Project_X/${commit}/build/moai.knl`;
  const hash = 'ab'.repeat(32);
  const remote = (overrides: Record<string, unknown> = {}): Record<string, unknown> => {
    const m: Record<string, unknown> = {
      ...valid(),
      id: 'moai',
      name: 'Moai',
      kind: 'synth',
      author: 'someone',
      link: 'https://github.com/someone/Project_X',
      fileUrl: url,
      sha256: hash,
      sizeBytes: 51992,
      version: '0.5',
    };
    delete m.file;
    for (const [k, v] of Object.entries(overrides)) m[k] = v;
    for (const k of Object.keys(m)) if (m[k] === undefined) delete m[k];
    return m;
  };

  it('accepts a commit-pinned .knl with its hash and size, and indexes it as installable but not hosted', () => {
    const { manifest, issues } = parsePluginManifest(remote(), 'moai');
    expect(issues).toEqual([]);
    const entry = pluginIndexEntryOfBuilds(manifest as PluginManifest, [null]);
    expect(entry).toMatchObject({ fileUrl: url, file: 'moai.knl', sha256: hash, sizeBytes: 51992, savedName: 'moai', version: '0.5' });
    expect(isHosted(entry)).toBe(false);
    expect(isInstallable(entry)).toBe(true);
  });

  it('keeps a fileUrl per build, and only the hundekraft build is installable', () => {
    const m = remote({ builds: [
      { firmware: 'op1/hundekraft', link: 'https://github.com/someone/Project_X', fileUrl: url, sha256: hash, sizeBytes: 100, version: '0.5' },
      { firmware: 'op1/opus', link: 'https://github.com/someone/Project_X' },
    ] });
    for (const k of ['firmware', 'link', 'fileUrl', 'sha256', 'sizeBytes', 'version', 'requires']) delete m[k];
    const { manifest, issues } = parsePluginManifest(m, 'moai');
    expect(issues).toEqual([]);
    const entry = pluginIndexEntryOfBuilds(manifest as PluginManifest, [null, null]);
    expect(isInstallable(pluginFor(entry, 'op1/hundekraft') as PluginIndexEntry)).toBe(true);
    expect(isInstallable(pluginFor(entry, 'op1/opus') as PluginIndexEntry)).toBe(false);
  });

  it('refuses a branch link, another host, another name, or a missing hash or size', () => {
    const issue = (o: Record<string, unknown>): string => messages(parsePluginManifest(remote(o), 'moai').issues);
    expect(issue({ fileUrl: 'https://raw.githubusercontent.com/someone/Project_X/main/build/moai.knl' })).toMatch(/pinned to a commit/);
    expect(issue({ fileUrl: `https://example.com/${commit}/moai.knl` })).toMatch(/pinned to a commit/);
    expect(issue({ fileUrl: url.replace('moai.knl', 'other.knl') })).toMatch(/must end in \/moai\.knl/);
    expect(issue({ sha256: undefined })).toMatch(/sha256 must be/);
    expect(issue({ sizeBytes: 0 })).toMatch(/sizeBytes must be/);
    expect(issue({ version: undefined })).toMatch(/version must be a string/);
  });

  it('refuses a fileUrl on a hosted build or on a non-hundekraft build', () => {
    expect(messages(parsePluginManifest(remote({ link: undefined, file: 'moai.knl' }), 'moai').issues)).toMatch(/are for a listed build/);
    expect(messages(parsePluginManifest(remote({ firmware: 'op1/opus' }), 'moai').issues)).toMatch(/only for op1\/hundekraft/);
  });
});
