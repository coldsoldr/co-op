/**
 * The community plugin format: the plugin.json manifest, the generated index.json, and pure checks
 * of the manifest and of the plugin, manual and screenshot bytes beside it. The file rules come
 * from docs/research/2026-10-29-hundekraft-usb-and-plugins.md. Nothing here touches the file
 * system; callers pass in a file listing and a reader, so this module runs the same in the CLI,
 * the dev server and the browser (which checks a plugin again before installing it).
 */
import { crc32 } from './crc32';
import { TAG, isIgnoredFile, isWindowsReservedName, overLimit } from './packs';

export const PLUGIN_FORMAT = 1;
export const PLUGIN_INDEX_FORMAT = 1;

export const PLUGIN_LICENSES = [
  'GPL-3.0-or-later',
  'GPL-3.0-only',
  'GPL-2.0-or-later',
  'MIT',
  'BSD-3-Clause',
  'Apache-2.0',
  'MPL-2.0',
  // Closed plugins that are free to use.
  'LicenseRef-Freeware',
] as const;
export type PluginLicense = (typeof PLUGIN_LICENSES)[number];

/** The profile ids a plugin can be for. */
export const PLUGIN_FIRMWARE = ['op1/hundekraft', 'op1/opus'] as const;
export type PluginFirmware = (typeof PLUGIN_FIRMWARE)[number];

export const PLUGIN_KINDS = ['synth', 'fx', 'feature'] as const;
export type PluginKind = (typeof PLUGIN_KINDS)[number];

export const MAX_NAME = 60;
export const MAX_AUTHOR = 60;
export const MAX_VERSION = 20;
export const MAX_DESCRIPTION = 500;
export const MAX_REQUIRES = 60;
export const MAX_CREDITS = 200;
export const MAX_SOURCE = 200;
export const MAX_TAGS = 8;

/** The kennel loader's limit for a .knl file. */
export const MAX_KNL_BYTES = 512 * 1024;
/** The op1r limit for the binary inside an .op1p. */
export const MAX_OP1P_BINARY_BYTES = 512 * 1024;
/** A whole .op1p (SVG, binary and trailer); our own limit, with room for the SVG. */
export const MAX_OP1P_BYTES = 1024 * 1024;
export const MAX_OPMOD_BYTES = 2 * 1024 * 1024;
export const MAX_SCREENSHOT_BYTES = 1024 * 1024;
/** plugin.json and manual.md are text; anything bigger is a mistake. */
export const MAX_TEXT_BYTES = 64 * 1024;
export const MIN_SCREENSHOT_SIDE = 64;
export const MAX_SCREENSHOT_SIDE = 2048;

export const MANIFEST_FILE = 'plugin.json';
export const MANUAL_FILE = 'manual.md';

/** A hundekraft plugin id: the saved name with spaces as `_`. */
export const HUNDEKRAFT_ID = /^[a-z0-9_-]{1,15}$/;
export const OPUS_ID = /^[a-z0-9_.-]{1,40}$/;
/** The name opus sounds store for a plugin: its plugin id. */
const OPUS_SAVED_NAME = /^opus\.[a-z0-9_.-]{1,40}$/;
const OPMOD_NAME = /^[A-Za-z0-9_][A-Za-z0-9_.-]{0,57}\.opmod$/;
const SCREENSHOT_NAME = /^[A-Za-z0-9_][A-Za-z0-9_.-]{0,59}\.png$/;
const HTTPS_LINK = /^https:\/\/[^\s/?#]+\.[^\s/?#]+([/?#]\S*)?$/;

export interface PluginManifest {
  format: 1;
  /** Equals the folder name; for hundekraft also the file name and the saved name with spaces as `_`. */
  id: string;
  name: string;
  firmware: PluginFirmware;
  kind: PluginKind;
  author: string;
  version: string;
  description: string;
  license: PluginLicense;
  /** `<id>.knl` or `<id>.op1p` for hundekraft, `<name>.opmod` for opus. */
  file: string;
  /** A .png file name in the plugin folder. */
  screenshot: string;
  manual: typeof MANUAL_FILE;
  /** An https:// link to the source. */
  source?: string;
  /** Free text such as "hundekraft 3.0.3". */
  requires?: string;
  credits?: string;
  tags: string[];
  /** opus only: the `opus.<id>` plugin id that sounds made with it store. */
  savedName?: string;
}

export interface PluginIndexEntry extends Omit<PluginManifest, 'format' | 'savedName' | 'screenshot' | 'manual'> {
  /** `<id>/<screenshot>`, relative to the plugins folder. */
  screenshot: string;
  /** `<id>/manual.md`, relative to the plugins folder. The plugin file is at `<id>/<file>`. */
  manual: string;
  sizeBytes: number;
  /** Lower-case hex SHA-256 of the plugin file. */
  sha256: string;
  /** The name sounds store: from the file header for hundekraft, from plugin.json for opus. */
  savedName?: string;
  /** Author and version from a .knl extension table, when it has them. */
  headerAuthor?: string;
  headerVersion?: string;
}

export interface PluginIndex {
  format: 1;
  /** ISO 8601 time the index was generated. */
  generatedAt: string;
  /** Sorted by id. */
  plugins: PluginIndexEntry[];
}

export interface PluginIssue {
  /** The plugin folder name. */
  plugin: string;
  /** The file inside the folder the issue is about, if any. */
  file?: string;
  message: string;
}

const MANIFEST_FIELDS = new Set([
  'format', 'id', 'name', 'firmware', 'kind', 'author', 'version', 'description', 'license',
  'file', 'screenshot', 'manual', 'source', 'requires', 'credits', 'tags', 'savedName',
]);

const isObject = (value: unknown): value is Record<string, unknown> =>
  typeof value === 'object' && value !== null && !Array.isArray(value);

const oneOf = <T extends string>(list: readonly T[], value: unknown): value is T =>
  typeof value === 'string' && (list as readonly string[]).includes(value);

/**
 * Checks a parsed plugin.json against the plugin rules. `folderName` is the plugin's folder, which
 * the id must equal. Every problem is reported, not just the first; the manifest is returned only
 * when there are none.
 */
export function parsePluginManifest(json: unknown, folderName: string): { manifest: PluginManifest | null; issues: PluginIssue[] } {
  const issues: PluginIssue[] = [];
  const fail = (message: string): void => {
    issues.push({ plugin: folderName, file: MANIFEST_FILE, message });
  };
  if (!isObject(json)) {
    fail('plugin.json must be a JSON object');
    return { manifest: null, issues };
  }

  for (const key of Object.keys(json)) if (!MANIFEST_FIELDS.has(key)) fail(`unknown field "${key}"`);
  if (json.format !== PLUGIN_FORMAT) {
    const found = json.format === undefined ? 'it is missing' : `found ${JSON.stringify(json.format)}`;
    fail(`format must be ${PLUGIN_FORMAT} (${found})`);
  }

  const text = (field: string, max: number): string => {
    const value = json[field];
    if (typeof value !== 'string') {
      fail(`${field} must be a string`);
      return '';
    }
    if (value.trim() === '' || value.length > max) fail(`${field} must be 1 to ${max} characters`);
    return value;
  };
  const optionalText = (field: string, max: number): string | undefined =>
    json[field] === undefined ? undefined : text(field, max);

  const firmware = json.firmware;
  if (!oneOf(PLUGIN_FIRMWARE, firmware)) fail(`firmware ${JSON.stringify(firmware)} must be one of ${PLUGIN_FIRMWARE.join(', ')}`);
  const opus = firmware === 'op1/opus';
  const kind = json.kind;
  if (!oneOf(PLUGIN_KINDS, kind)) fail(`kind ${JSON.stringify(kind)} must be one of ${PLUGIN_KINDS.join(', ')}`);

  const id = json.id;
  if (typeof id !== 'string') {
    fail('id must be a string');
  } else {
    if (id.startsWith('.')) fail(`id "${id}" must not start with a dot`);
    else if (opus && !OPUS_ID.test(id)) fail(`id "${id}" must be 1 to 40 lower-case letters, digits, _, . or -`);
    else if (!opus && !HUNDEKRAFT_ID.test(id)) fail(`id "${id}" must be 1 to 15 lower-case letters, digits, _ or -`);
    if (isWindowsReservedName(id)) fail(`id "${id}" is reserved by Windows`);
    if (id !== folderName) fail(`id "${id}" must equal the folder name "${folderName}"`);
  }

  const name = text('name', MAX_NAME);
  const author = text('author', MAX_AUTHOR);
  const version = text('version', MAX_VERSION);
  const description = text('description', MAX_DESCRIPTION);

  const license = json.license;
  if (!oneOf(PLUGIN_LICENSES, license)) fail(`license ${JSON.stringify(license)} must be one of ${PLUGIN_LICENSES.join(', ')}`);

  const file = json.file;
  if (typeof file !== 'string') {
    fail('file must be a string');
  } else if (firmware === 'op1/hundekraft' && typeof id === 'string') {
    if (file !== `${id}.knl` && file !== `${id}.op1p`) fail(`file must be "${id}.knl" or "${id}.op1p"`);
    else if (file.endsWith('.op1p') && kind === 'feature') fail('an .op1p plugin is a synth or an fx, not a feature');
  } else if (opus && (!OPMOD_NAME.test(file) || isWindowsReservedName(file))) {
    fail('file must be a name ending in .opmod: letters, digits, _, . or -, at most 64 characters');
  }

  const screenshot = json.screenshot;
  if (typeof screenshot !== 'string' || !SCREENSHOT_NAME.test(screenshot) || isWindowsReservedName(screenshot)) {
    fail('screenshot must be a .png file name in the plugin folder: letters, digits, _, . or -, at most 64 characters');
  }
  if (json.manual !== MANUAL_FILE) fail(`manual must be "${MANUAL_FILE}"`);

  const source = json.source;
  if (source !== undefined) {
    if (typeof source !== 'string' || !HTTPS_LINK.test(source)) fail('source must be an https:// link');
    else if (source.length > MAX_SOURCE) fail(`source must be at most ${MAX_SOURCE} characters`);
  }
  const requires = optionalText('requires', MAX_REQUIRES);
  const credits = optionalText('credits', MAX_CREDITS);

  let tags: unknown[] = [];
  if (json.tags !== undefined) {
    if (!Array.isArray(json.tags)) {
      fail('tags must be a list');
    } else {
      tags = json.tags;
      if (tags.length > MAX_TAGS) fail(`at most ${MAX_TAGS} tags are allowed`);
      const seen = new Set<unknown>();
      for (const tag of tags) {
        if (typeof tag !== 'string' || !TAG.test(tag)) {
          fail(`tag ${JSON.stringify(tag)} must be 1 to 20 lower-case letters, digits or hyphens`);
        } else if (seen.has(tag)) {
          fail(`duplicate tag "${tag}"`);
        }
        seen.add(tag);
      }
    }
  }

  const savedName = json.savedName;
  if (savedName !== undefined) {
    if (!opus) fail('savedName is only for op1/opus plugins; hundekraft takes it from the plugin file');
    else if (typeof savedName !== 'string' || !OPUS_SAVED_NAME.test(savedName)) {
      fail(`savedName ${JSON.stringify(savedName)} must be opus.<id>: "opus." and 1 to 40 lower-case letters, digits, _, . or -`);
    }
  }

  if (issues.length > 0) return { manifest: null, issues };
  return {
    manifest: {
      format: PLUGIN_FORMAT,
      id: id as string,
      name,
      firmware: firmware as PluginFirmware,
      kind: kind as PluginKind,
      author,
      version,
      description,
      license: license as PluginLicense,
      file: file as string,
      screenshot: screenshot as string,
      manual: MANUAL_FILE,
      ...(source === undefined ? {} : { source: source as string }),
      ...(requires === undefined ? {} : { requires }),
      ...(credits === undefined ? {} : { credits }),
      tags: tags as string[],
      ...(savedName === undefined ? {} : { savedName: savedName as string }),
    },
    issues,
  };
}

/** What a plugin file says about itself. Empty for .opmod, whose layout beyond its magic is unknown. */
export interface PluginFileInfo {
  kind?: PluginKind;
  /** The name sounds store for it. */
  savedName?: string;
  author?: string;
  version?: string;
}

export interface PluginFileCheck {
  problems: string[];
  /** Null when there are problems. */
  info: PluginFileInfo | null;
}

const LT = 0x3c;
const u16 = (b: Uint8Array, at: number): number => (b[at] ?? 0) | ((b[at + 1] ?? 0) << 8);
const u32 = (b: Uint8Array, at: number): number => (u16(b, at) | (u16(b, at + 2) << 16)) >>> 0;
const u32be = (b: Uint8Array, at: number): number =>
  (((b[at] ?? 0) << 24) | ((b[at + 1] ?? 0) << 16) | ((b[at + 2] ?? 0) << 8) | (b[at + 3] ?? 0)) >>> 0;
const pad4 = (n: number): number => n + ((4 - (n % 4)) % 4);
const hex = (n: number): string => n.toString(16).padStart(8, '0');

/** Bytes as text, one character per byte; enough for ASCII names and for finding digits in an SVG. */
function latin1(bytes: Uint8Array): string {
  let text = '';
  for (let i = 0; i < bytes.length; i += 8192) text += String.fromCharCode(...bytes.subarray(i, i + 8192));
  return text;
}

/**
 * R246's SVG parser hangs the boot on a number with more than four decimals, so a screen SVG
 * with one is refused.
 */
function svgHazard(svg: string): string | null {
  const found = /\d\.\d{5,}/.exec(svg);
  return found ? `the screen SVG has a number with more than four decimals ("${found[0]}"); the OP-1 hangs at boot on it` : null;
}

/** A saved name in a 16-byte NUL-padded field: the name, or why it is not one. */
function savedNameAt(bytes: Uint8Array, at: number, allowed: RegExp, rule: string): { name: string | null; problem: string | null } {
  const field = bytes.subarray(at, at + 16);
  const nul = field.indexOf(0);
  if (nul < 0) return { name: null, problem: 'the saved name does not end in a NUL within 16 bytes' };
  if (nul === 0) return { name: null, problem: 'the saved name is empty' };
  const name = latin1(field.subarray(0, nul));
  return allowed.test(name) ? { name, problem: null } : { name: null, problem: `the saved name must be ${rule}` };
}

const KNL_KINDS: Record<number, PluginKind> = { 1: 'synth', 2: 'fx', 3: 'feature' };
const KNL_HEADER = 144;
const EXT_AUTHOR = 2;
const EXT_VERSION = 3;
const EXT_CLAIMS = 1;

/**
 * Checks a kennel plugin (.knl, format 1): an optional screen SVG, NUL and zero padding to 4
 * bytes; then the 144-byte header (magic, format, header size, file size, CRC-32 over the file
 * with its own 4 bytes as zero, API, kind, legacy order, saved name, SVG fields); the file length;
 * the SVG boot hazard; and the optional extension table, whose author (tag 2) and version (tag 3)
 * are returned. Every offset read from the file is checked against its length first.
 */
export function checkKnl(bytes: Uint8Array): PluginFileCheck {
  const problems: string[] = [];
  const done = (info: PluginFileInfo | null = null): PluginFileCheck => ({ problems, info: problems.length > 0 ? null : info });
  const length = bytes.length;

  let h = 0;
  let svgEnd = 0;
  if (bytes[0] === LT) {
    const nul = bytes.indexOf(0);
    if (nul < 0) {
      problems.push('starts with a screen SVG that never ends in a NUL');
      return done();
    }
    svgEnd = nul + 1;
    h = pad4(svgEnd);
    if (bytes.subarray(svgEnd, h).some((b) => b !== 0)) problems.push('the padding after the screen SVG is not zero');
    const hazard = svgHazard(latin1(bytes.subarray(0, nul)));
    if (hazard) problems.push(hazard);
  }
  if (length > MAX_KNL_BYTES) problems.push(`is ${overLimit(length, MAX_KNL_BYTES)}`);
  if (length % 4 !== 0) problems.push(`${length} bytes is not a multiple of 4`);
  if (length < h + KNL_HEADER) {
    problems.push(`${length} bytes is too short for the ${KNL_HEADER}-byte header`);
    return done();
  }
  if (latin1(bytes.subarray(h, h + 4)) !== 'KNPL') {
    problems.push('no KNPL header (not a kennel plugin)');
    return done();
  }

  const format = u16(bytes, h + 4);
  if (format !== 1) problems.push(`format ${format} is not 1`);
  const headerSize = u16(bytes, h + 6);
  if (headerSize < KNL_HEADER) problems.push(`header size ${headerSize} is under ${KNL_HEADER} bytes`);
  else if (h + headerSize > length) problems.push(`header size ${headerSize} runs past the end of the file`);
  const fileSize = u32(bytes, h + 8);
  if (fileSize !== length) problems.push(`the header says ${fileSize} bytes but the file is ${length}`);

  const stored = u32(bytes, h + 12);
  let crc = crc32(bytes.subarray(0, h + 12));
  crc = crc32(new Uint8Array(4), crc);
  crc = crc32(bytes.subarray(h + 16), crc);
  if (crc !== stored) problems.push(`CRC-32 is ${hex(crc)} but the header says ${hex(stored)}`);

  const api = u16(bytes, h + 16);
  if (api !== 1) problems.push(`plugin API ${api} is not 1`);
  const kindByte = bytes[h + 18] as number;
  const kind = KNL_KINDS[kindByte];
  if (!kind) problems.push(`kind ${kindByte} is not 1 (synth), 2 (fx) or 3 (feature)`);
  const order = bytes[h + 19] as number;
  if (order > 31) problems.push(`legacy order ${order} is over 31`);
  const saved = savedNameAt(bytes, h + 20, /^[\x20-\x7e]+$/, 'printable ASCII');
  if (saved.problem) problems.push(saved.problem);

  // The SVG size counts the text and its NUL, and may take in the zero padding; 0 without an SVG.
  const svgSize = u32(bytes, h + 132);
  if (svgEnd === 0 && svgSize !== 0) problems.push(`the header says the screen SVG is ${svgSize} bytes but the file has none`);
  if (svgEnd > 0 && (svgSize < svgEnd || svgSize > h)) {
    problems.push(`the header says the screen SVG is ${svgSize} bytes but the file starts with ${svgEnd}`);
  }
  if (u32(bytes, h + 128) !== 0 || u32(bytes, h + 136) !== 0xffffffff) {
    problems.push('the reserved SVG fields (header bytes 128 and 136) are not 0 and 0xFFFFFFFF');
  }

  const info: PluginFileInfo = { kind: kind as PluginKind, savedName: saved.name as string };
  const extAt = u32(bytes, h + 140);
  if (extAt !== 0) {
    if (extAt % 4 !== 0 || extAt < h + KNL_HEADER || extAt >= length) {
      problems.push(`the extension table offset ${extAt} is outside the file or not 4-aligned`);
    } else {
      // Each entry is u16 tag, u16 length and the bytes padded to 4; tag 0 ends the table.
      for (let at = extAt; ; ) {
        if (at + 4 > length) {
          problems.push('the extension table runs past the end of the file');
          break;
        }
        const tag = u16(bytes, at);
        const size = u16(bytes, at + 2);
        if (tag === 0) break;
        if (at + 4 + size > length) {
          problems.push('an extension table entry runs past the end of the file');
          break;
        }
        const data = bytes.subarray(at + 4, at + 4 + size);
        if (tag >= EXT_CLAIMS && tag <= EXT_VERSION) {
          if (size === 0 || data[size - 1] !== 0 || data.some((b) => b >= 0x80)) {
            problems.push(`extension entry ${tag} is not NUL-terminated ASCII`);
            break;
          }
          // Claims are NUL-separated names ending in an empty one.
          if (tag === EXT_CLAIMS && size > 1 && data[size - 2] !== 0) {
            problems.push('the claims list in the extension table does not end in an empty name');
            break;
          }
          const value = latin1(data.subarray(0, data.indexOf(0)));
          if (tag === EXT_AUTHOR) info.author = value;
          if (tag === EXT_VERSION) info.version = value;
        }
        at += 4 + pad4(size);
      }
    }
  }
  return done(info);
}

const OP1P_TRAILER_MAGIC = 0x5431504f;
const OP1P_BINARY_MAGIC = 0x5031504f;
const OP1P_MIN_BINARY = 76;
const OP1P_KINDS: Record<number, PluginKind> = { 0: 'synth', 1: 'fx' };

/**
 * Checks an op1r plugin (.op1p): the 16-byte trailer (magic, 4-aligned binary offset, binary
 * length of at least 76 bytes inside the file, CRC-32 of the binary), then the binary's own header
 * (magic, format 1, API 1, kind 0 synth or 1 fx, saved name), the size limits and the SVG boot
 * hazard on a screen SVG in front.
 */
export function checkOp1p(bytes: Uint8Array): PluginFileCheck {
  const problems: string[] = [];
  const done = (info: PluginFileInfo | null = null): PluginFileCheck => ({ problems, info: problems.length > 0 ? null : info });
  const length = bytes.length;
  if (length < 16) {
    problems.push(`${length} bytes is too short for the 16-byte op1r trailer`);
    return done();
  }
  if (length > MAX_OP1P_BYTES) problems.push(`is ${overLimit(length, MAX_OP1P_BYTES)}`);
  if (bytes[0] === LT) {
    const nul = bytes.indexOf(0);
    const hazard = svgHazard(latin1(bytes.subarray(0, nul < 0 ? length : nul)));
    if (hazard) problems.push(hazard);
  }

  const t = length - 16;
  if (u32(bytes, t) !== OP1P_TRAILER_MAGIC) {
    problems.push('no op1r trailer at the end of the file (not an .op1p plugin)');
    return done();
  }
  const at = u32(bytes, t + 4);
  const size = u32(bytes, t + 8);
  if (at % 4 !== 0) {
    problems.push(`the binary offset ${at} is not 4-aligned`);
    return done();
  }
  if (size < OP1P_MIN_BINARY) {
    problems.push(`the binary is ${size} bytes, under ${OP1P_MIN_BINARY}`);
    return done();
  }
  if (at + size > t) {
    problems.push('the binary runs past the trailer');
    return done();
  }
  const binary = bytes.subarray(at, at + size);
  if (size > MAX_OP1P_BINARY_BYTES) problems.push(`the binary is ${overLimit(size, MAX_OP1P_BINARY_BYTES)}`);
  const stored = u32(bytes, t + 12);
  const crc = crc32(binary);
  if (crc !== stored) problems.push(`the binary's CRC-32 is ${hex(crc)} but the trailer says ${hex(stored)}`);

  if (u32(binary, 0) !== OP1P_BINARY_MAGIC) {
    problems.push('the binary does not start with the op1r plugin magic');
    return done();
  }
  const format = u32(binary, 4);
  if (format !== 1) problems.push(`binary format ${format} is not 1`);
  const api = u32(binary, 8);
  if (api !== 1) problems.push(`plugin API ${api} is not 1`);
  const kindValue = u32(binary, 12);
  const kind = OP1P_KINDS[kindValue];
  if (!kind) problems.push(`kind ${kindValue} is not 0 (synth) or 1 (fx)`);
  const saved = savedNameAt(binary, 16, /^[a-z0-9 _-]+$/, 'lower-case letters, digits, spaces, _ or -');
  if (saved.problem) problems.push(saved.problem);
  return done({ kind: kind as PluginKind, savedName: saved.name as string });
}

/** Checks an opus plugin (.opmod): the OPMD magic and the size limit; nothing more is known. */
export function checkOpmod(bytes: Uint8Array): string[] {
  const problems: string[] = [];
  if (bytes.length > MAX_OPMOD_BYTES) problems.push(`is ${overLimit(bytes.length, MAX_OPMOD_BYTES)}`);
  if (latin1(bytes.subarray(0, 4)) !== 'OPMD') problems.push('does not start with OPMD (not an opus plugin)');
  return problems;
}

const PNG_SIGNATURE = [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a];

/** Checks a screenshot: a PNG (signature, then the IHDR chunk) of 64 to 2048 pixels each way, at most 1 MB. */
export function checkScreenshot(bytes: Uint8Array): string[] {
  const problems: string[] = [];
  if (bytes.length > MAX_SCREENSHOT_BYTES) problems.push(`is ${overLimit(bytes.length, MAX_SCREENSHOT_BYTES)}`);
  const isPng =
    bytes.length >= 24 &&
    PNG_SIGNATURE.every((b, i) => bytes[i] === b) &&
    u32be(bytes, 8) === 13 &&
    latin1(bytes.subarray(12, 16)) === 'IHDR';
  if (!isPng) return [...problems, 'not a PNG image'];
  const width = u32be(bytes, 16);
  const height = u32be(bytes, 20);
  const fits = (side: number): boolean => side >= MIN_SCREENSHOT_SIDE && side <= MAX_SCREENSHOT_SIDE;
  if (!fits(width) || !fits(height)) {
    problems.push(`is ${width} x ${height} pixels; width and height must each be ${MIN_SCREENSHOT_SIDE} to ${MAX_SCREENSHOT_SIDE}`);
  }
  return problems;
}

/** True when `bytes` is well-formed UTF-8: shortest forms only, no surrogates, nothing above U+10FFFF. */
export function isUtf8(bytes: Uint8Array): boolean {
  for (let i = 0; i < bytes.length; ) {
    const lead = bytes[i] as number;
    if (lead < 0x80) {
      i++;
      continue;
    }
    const extra = lead >= 0xc2 && lead <= 0xdf ? 1 : lead >= 0xe0 && lead <= 0xef ? 2 : lead >= 0xf0 && lead <= 0xf4 ? 3 : 0;
    if (extra === 0 || i + extra >= bytes.length) return false;
    let point = lead & (0x3f >> extra);
    for (let k = 1; k <= extra; k++) {
      const next = bytes[i + k] as number;
      if ((next & 0xc0) !== 0x80) return false;
      point = (point << 6) | (next & 0x3f);
    }
    if (extra === 2 && (point < 0x800 || (point >= 0xd800 && point <= 0xdfff))) return false;
    if (extra === 3 && (point < 0x10000 || point > 0x10ffff)) return false;
    i += extra + 1;
  }
  return true;
}

/** Checks manual.md: at most 64 KB of UTF-8 that is not blank. */
export function checkManual(bytes: Uint8Array): string[] {
  if (bytes.length > MAX_TEXT_BYTES) return [`is ${overLimit(bytes.length, MAX_TEXT_BYTES)}`];
  if (!isUtf8(bytes)) return ['is not valid UTF-8'];
  const bom = bytes[0] === 0xef && bytes[1] === 0xbb && bytes[2] === 0xbf ? 3 : 0;
  if (!bytes.subarray(bom).some((b) => b > 0x20)) return ['is empty'];
  return [];
}

/** The id a hundekraft saved name gives: spaces as `_`, and "drum+" as "drumplus". */
export function idFromSavedName(savedName: string): string {
  return savedName === 'drum+' ? 'drumplus' : savedName.replaceAll(' ', '_');
}

/** The size limit for one of a plugin's files. */
function limitFor(manifest: PluginManifest, path: string): number {
  if (path !== manifest.file) return path === manifest.screenshot ? MAX_SCREENSHOT_BYTES : MAX_TEXT_BYTES;
  if (path.endsWith('.knl')) return MAX_KNL_BYTES;
  return path.endsWith('.op1p') ? MAX_OP1P_BYTES : MAX_OPMOD_BYTES;
}

function checkPluginFile(file: string, bytes: Uint8Array): PluginFileCheck {
  if (file.endsWith('.knl')) return checkKnl(bytes);
  if (file.endsWith('.op1p')) return checkOp1p(bytes);
  const problems = checkOpmod(bytes);
  return { problems, info: problems.length > 0 ? null : {} };
}

/**
 * Checks the files in a plugin folder against its (already valid) manifest. `files` lists every
 * file in the folder with its size, by path relative to the folder; `read` returns one file's
 * bytes. The folder must hold plugin.json, the plugin file, manual.md and the screenshot, spelled
 * exactly, and nothing else but ignored clutter (isIgnoredFile). Sizes are checked before a file
 * is read. For hundekraft the file's saved name must give the id and its kind must match
 * plugin.json; an author or version in the file that differs from plugin.json is a warning.
 * `info` is what the plugin file says, or null when there are issues.
 */
export async function validatePluginFiles(
  manifest: PluginManifest,
  files: readonly { path: string; size: number }[],
  read: (path: string) => Promise<Uint8Array>,
): Promise<{ issues: PluginIssue[]; warnings: PluginIssue[]; info: PluginFileInfo | null }> {
  const issues: PluginIssue[] = [];
  const warnings: PluginIssue[] = [];
  const fail = (message: string, file: string): void => {
    issues.push({ plugin: manifest.id, file, message });
  };
  files = files.filter((f) => !isIgnoredFile(f.path));
  const sizes = new Map(files.map((f) => [f.path, f.size]));
  const expected = [MANIFEST_FILE, manifest.file, manifest.manual, manifest.screenshot];

  let info: PluginFileInfo | null = null;
  for (const path of expected) {
    const size = sizes.get(path);
    if (size === undefined) {
      const near = files.find((f) => f.path.toLowerCase() === path.toLowerCase());
      fail(near ? `missing; the folder has ${near.path}, which differs only in case` : 'missing', path);
      continue;
    }
    const limit = limitFor(manifest, path);
    if (size > limit) {
      fail(`is ${overLimit(size, limit)}`, path);
      continue;
    }
    // plugin.json was parsed by the caller.
    if (path === MANIFEST_FILE) continue;
    const bytes = await read(path);
    if (path === manifest.manual) {
      for (const problem of checkManual(bytes)) fail(problem, path);
      continue;
    }
    if (path === manifest.screenshot) {
      for (const problem of checkScreenshot(bytes)) fail(problem, path);
      continue;
    }

    const check = checkPluginFile(path, bytes);
    for (const problem of check.problems) fail(problem, path);
    if (!check.info) continue;
    info = check.info;
    if (manifest.firmware !== 'op1/hundekraft') continue;
    const savedName = check.info.savedName as string;
    const id = idFromSavedName(savedName);
    if (!HUNDEKRAFT_ID.test(id)) {
      fail(`the saved name "${savedName}" makes the id "${id}", which is not 1 to 15 lower-case letters, digits, _ or -`, path);
    } else if (id !== manifest.id) {
      fail(`the saved name "${savedName}" makes the id "${id}"; the id, folder and file must use it`, path);
    }
    if (check.info.kind !== manifest.kind) fail(`the file's kind is ${check.info.kind} but plugin.json says ${manifest.kind}`, path);
    const warn = (field: 'author' | 'version'): void => {
      const value = check.info?.[field];
      if (value !== undefined && value !== manifest[field]) {
        warnings.push({ plugin: manifest.id, file: path, message: `the file says ${field} "${value}" but plugin.json says "${manifest[field]}"` });
      }
    };
    warn('author');
    warn('version');
  }

  const listed = `${expected.slice(0, -1).join(', ')} and ${expected.at(-1)}`;
  for (const { path } of files) {
    if (expected.includes(path)) continue;
    // A case-only misspelling of a missing file was reported above.
    const variantOf = expected.find((e) => e.toLowerCase() === path.toLowerCase());
    if (variantOf && !sizes.has(variantOf)) continue;
    fail(`not part of a plugin folder; it holds ${listed} only`, path);
  }
  return { issues, warnings, info: issues.length > 0 ? null : info };
}

/** The index entry of a valid plugin, given its plugin file's size and hash and what the file says. */
export function pluginIndexEntry(
  manifest: PluginManifest,
  file: { sizeBytes: number; sha256: string },
  info: PluginFileInfo,
): PluginIndexEntry {
  const { format: _format, savedName: ownSavedName, ...fields } = manifest;
  const savedName = manifest.firmware === 'op1/hundekraft' ? info.savedName : ownSavedName;
  return {
    ...fields,
    screenshot: `${manifest.id}/${manifest.screenshot}`,
    manual: `${manifest.id}/${manifest.manual}`,
    sizeBytes: file.sizeBytes,
    sha256: file.sha256,
    ...(savedName === undefined ? {} : { savedName }),
    ...(info.author === undefined ? {} : { headerAuthor: info.author }),
    ...(info.version === undefined ? {} : { headerVersion: info.version }),
  };
}
