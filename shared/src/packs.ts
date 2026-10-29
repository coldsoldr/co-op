/**
 * The community pack format: the pack.json manifest, the generated index.json, and pure
 * validation of both the manifest and the files beside it. Nothing here touches the file system
 * or the patch codec; callers pass in a file listing and a parser, so this module stays free of
 * web code and runs the same in the CLI, the dev server and the browser.
 */

export const PACK_FORMAT = 1;
export const INDEX_FORMAT = 1;

export const PACK_LICENSES = ['CC0-1.0', 'CC-BY-4.0', 'CC-BY-SA-4.0', 'CC-BY-NC-4.0', 'CC-BY-NC-SA-4.0'] as const;
export type PackLicense = (typeof PACK_LICENSES)[number];

/** The folders a pack's patches live in; each maps to the patch kind of the same name. */
export const PACK_KINDS = ['synth', 'drum'] as const;
export type PackKind = (typeof PACK_KINDS)[number];

export const MAX_SLUG = 40;
export const MAX_NAME = 60;
export const MAX_AUTHOR = 60;
export const MAX_DESCRIPTION = 500;
export const MAX_TAGS = 8;
export const MAX_NOTES = 200;
export const MAX_PATCHES = 100;
export const MAX_FILE_BYTES = 2 * 1024 * 1024;
export const MAX_PACK_BYTES = 20 * 1024 * 1024;
/** pack.json and README.md are text; anything bigger is a mistake. */
export const MAX_TEXT_BYTES = 64 * 1024;
/** The longest patch file name, matching assertPatchFileName in web's operations.ts. */
export const MAX_FILE_NAME = 64;

/** Files allowed in a pack folder besides the listed patches. */
export const MANIFEST_FILE = 'pack.json';
export const README_FILE = 'README.md';

/** A pack slug: lower-case letters and digits in words joined by single hyphens. */
export const SLUG = /^[a-z0-9]+(-[a-z0-9]+)*$/;
const TAG = /^[a-z0-9-]{1,20}$/;

export interface PackPatchEntry {
  /** 'synth/<name>.aif' or 'drum/<name>.aif', relative to the pack folder. */
  file: string;
  notes?: string;
}

export interface PackManifest {
  format: 1;
  name: string;
  slug: string;
  author: string;
  description: string;
  license: PackLicense;
  /** Profile ids such as 'op1/stock'. */
  firmware: string[];
  tags: string[];
  patches: PackPatchEntry[];
}

export interface PackIndexPatch {
  file: string;
  kind: PackKind;
  /** The name stored in the patch's op-1 metadata. */
  name: string;
  /** The engine (`type` in the metadata): a synth engine, or 'drum', 'dbox' or 'gin' for drum kits. */
  engine: string;
  sizeBytes: number;
  /** Lower-case hex SHA-256 of the file. */
  sha256: string;
  /** The entry's notes from pack.json, when it has any. */
  notes?: string;
}

export interface PackIndexEntry {
  slug: string;
  name: string;
  author: string;
  description: string;
  license: PackLicense;
  firmware: string[];
  tags: string[];
  /** Total size of the pack's patch files. */
  sizeBytes: number;
  /** True when the pack has a README.md, published at packs/<slug>/README.md. */
  hasReadme: boolean;
  patches: PackIndexPatch[];
}

export interface PackIndex {
  format: 1;
  /** ISO 8601 time the index was generated. */
  generatedAt: string;
  /** Sorted by slug. */
  packs: PackIndexEntry[];
}

export interface PackIssue {
  /** The pack folder name. */
  pack: string;
  /** The file inside the pack the issue is about, if any. */
  file?: string;
  message: string;
}

// eslint-disable-next-line no-control-regex
const FAT_ILLEGAL = /[\u0000-\u001f\u007f:*?"<>|/\\]/;
const PATCH_EXTENSIONS = ['.aif', '.aiff'];

const WINDOWS_RESERVED = /^(con|prn|aux|nul|com[1-9]|lpt[1-9])$/i;

/**
 * True for names Windows cannot create (CON, PRN, AUX, NUL, COM1-9, LPT1-9), in any case, with or
 * without an extension: everything before the first dot counts. The same rule as in
 * web/src/core/library/operations.ts.
 */
export function isWindowsReservedName(name: string): boolean {
  return WINDOWS_RESERVED.test(name.split('.')[0] as string);
}

/** Clutter that Finder and Explorer leave in folders; ignored in a pack and never published. */
const IGNORED_FILES = new Set(['.ds_store', 'thumbs.db', 'desktop.ini']);

/** True for `.DS_Store`, `Thumbs.db` or `desktop.ini` (any case) at any depth of a pack folder. */
export function isIgnoredFile(path: string): boolean {
  return IGNORED_FILES.has((path.split('/').at(-1) ?? '').toLowerCase());
}

/**
 * Why `name` cannot be a patch file name on the OP-1's FAT drive, or null when it can. The same
 * rules as assertPatchFileName in web/src/core/library/operations.ts (a test there checks they
 * agree): not empty, no path separator, none of `:*?"<>|`, no control character, no leading '.',
 * no trailing dot or space, at most 64 characters, and ending in .aif or .aiff (any case) after
 * at least one character.
 */
export function patchFileNameProblem(name: string): string | null {
  if (name === '') return 'file name is empty';
  if (name.length > MAX_FILE_NAME) return `file name "${name}" is longer than ${MAX_FILE_NAME} characters`;
  if (isWindowsReservedName(name)) return `file name "${name}" is reserved by Windows`;
  if (FAT_ILLEGAL.test(name)) {
    return `file name ${JSON.stringify(name)} contains a character the OP-1 drive cannot hold (/ \\ : * ? " < > | or a control character)`;
  }
  if (name.startsWith('.')) return `file name "${name}" starts with "."`;
  if (name.endsWith('.') || name.endsWith(' ')) return `file name "${name}" ends with a dot or a space`;
  const lower = name.toLowerCase();
  if (!PATCH_EXTENSIONS.some((ext) => lower.endsWith(ext) && lower.length > ext.length)) {
    return `file name "${name}" must end in ${PATCH_EXTENSIONS.join(' or ')}`;
  }
  return null;
}

/** The kind folder of a manifest path ('synth/a.aif' -> 'synth'), or null when it is not one. */
export function patchKindOf(file: string): PackKind | null {
  const folder = file.slice(0, Math.max(file.indexOf('/'), 0));
  return (PACK_KINDS as readonly string[]).includes(folder) ? (folder as PackKind) : null;
}

const MANIFEST_FIELDS = new Set(['format', 'name', 'slug', 'author', 'description', 'license', 'firmware', 'tags', 'patches']);
const ENTRY_FIELDS = new Set(['file', 'notes']);

const isObject = (value: unknown): value is Record<string, unknown> =>
  typeof value === 'object' && value !== null && !Array.isArray(value);

/**
 * Checks a parsed pack.json against the pack rules. `folderName` is the pack's folder, which the
 * slug must equal; `knownFirmware` lists the profile ids a pack may name. Every problem is
 * reported, not just the first; the manifest is returned only when there are none.
 */
export function parseManifest(
  json: unknown,
  folderName: string,
  knownFirmware: readonly string[],
): { manifest: PackManifest | null; issues: PackIssue[] } {
  const issues: PackIssue[] = [];
  const fail = (message: string, file: string = MANIFEST_FILE): void => {
    issues.push({ pack: folderName, file, message });
  };
  if (!isObject(json)) {
    fail('pack.json must be a JSON object');
    return { manifest: null, issues };
  }

  for (const key of Object.keys(json)) if (!MANIFEST_FIELDS.has(key)) fail(`unknown field "${key}"`);
  if (json.format !== PACK_FORMAT) {
    const found = json.format === undefined ? 'it is missing' : `found ${JSON.stringify(json.format)}`;
    fail(`format must be ${PACK_FORMAT} (${found})`);
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

  const slug = json.slug;
  if (typeof slug !== 'string') {
    fail('slug must be a string');
  } else {
    if (!SLUG.test(slug)) fail(`slug "${slug}" must be lower-case letters and digits in words joined by single hyphens`);
    if (slug.length > MAX_SLUG) fail(`slug "${slug}" is longer than ${MAX_SLUG} characters`);
    if (slug !== folderName) fail(`slug "${slug}" must equal the folder name "${folderName}"`);
  }
  const name = text('name', MAX_NAME);
  const author = text('author', MAX_AUTHOR);
  const description = text('description', MAX_DESCRIPTION);

  const license = json.license;
  if (typeof license !== 'string' || !(PACK_LICENSES as readonly string[]).includes(license)) {
    fail(`license ${JSON.stringify(license)} must be one of ${PACK_LICENSES.join(', ')}`);
  }

  const list = (field: string): unknown[] | null => {
    const value = json[field];
    if (!Array.isArray(value)) {
      fail(`${field} must be a list`);
      return null;
    }
    return value;
  };

  const firmware = list('firmware');
  if (firmware) {
    if (firmware.length === 0) fail('firmware must list at least one profile id');
    const seen = new Set<unknown>();
    for (const id of firmware) {
      if (typeof id !== 'string' || !knownFirmware.includes(id)) {
        fail(`unknown firmware ${JSON.stringify(id)}; known: ${knownFirmware.join(', ')}`);
      } else if (seen.has(id)) {
        fail(`duplicate firmware "${id}"`);
      }
      seen.add(id);
    }
  }

  let tags: unknown[] | null = [];
  if (json.tags !== undefined) {
    tags = list('tags');
    if (tags) {
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

  const patches = list('patches');
  const entries: PackPatchEntry[] = [];
  if (patches) {
    if (patches.length === 0) fail('patches must list at least one patch');
    if (patches.length > MAX_PATCHES) fail(`at most ${MAX_PATCHES} patches are allowed`);
    const seen = new Set<string>();
    for (const entry of patches) {
      if (!isObject(entry)) {
        fail('each patch must be an object with a "file"');
        continue;
      }
      const file = entry.file;
      if (typeof file !== 'string') {
        fail('file must be a string');
        continue;
      }
      for (const key of Object.keys(entry)) if (!ENTRY_FIELDS.has(key)) fail(`unknown field "${key}"`, file);
      if (entry.notes !== undefined) {
        if (typeof entry.notes !== 'string') fail('notes must be a string', file);
        else if (entry.notes.length > MAX_NOTES) fail(`notes must be at most ${MAX_NOTES} characters`, file);
      }
      const rest = file.slice(file.indexOf('/') + 1);
      if (patchKindOf(file) === null && patchKindOf(file.toLowerCase()) !== null) {
        fail('folder names are lower-case: use synth/ or drum/', file);
      } else if (patchKindOf(file) === null || rest === '' || rest.includes('/')) {
        fail('must be synth/<name> or drum/<name>', file);
      } else {
        const problem = patchFileNameProblem(rest);
        if (problem) fail(problem, file);
      }
      const key = file.toLowerCase();
      if (seen.has(key)) fail('listed more than once (names are compared ignoring case)', file);
      seen.add(key);
      entries.push(entry.notes === undefined ? { file } : { file, notes: entry.notes as string });
    }
  }

  if (issues.length > 0) return { manifest: null, issues };
  return {
    manifest: {
      format: PACK_FORMAT,
      name,
      slug: slug as string,
      author,
      description,
      license: license as PackLicense,
      firmware: firmware as string[],
      tags: tags as string[],
      patches: entries,
    },
    issues,
  };
}

/** What validatePackFiles needs to know about one parsed patch file. */
export interface ParsedPackPatch {
  kind: 'synth' | 'drum' | 'unknown';
  name: string;
  engine: string;
}

/** '2097153 bytes, over the 2 MB (2097152 bytes) limit': exact bytes, so nothing rounds under. */
function overLimit(size: number, limit: number): string {
  const unit = limit >= 1024 * 1024 ? `${limit / (1024 * 1024)} MB` : `${limit / 1024} KB`;
  return `${size} bytes, over the ${unit} (${limit} bytes) limit`;
}

/**
 * Checks the files in a pack folder against its (already valid) manifest. `files` lists every
 * file in the folder with its size, by path relative to the folder; `parse` reads one of them as
 * a patch and throws when it cannot. Every listed file must exist with exactly that spelling, no
 * other file may be present except pack.json, README.md and ignored clutter (isIgnoredFile), each patch must be at most 2 MB and
 * the patches together at most 20 MB, and each must parse as the kind its folder names.
 */
export async function validatePackFiles(
  manifest: PackManifest,
  files: readonly { path: string; size: number }[],
  parse: (path: string) => Promise<ParsedPackPatch>,
): Promise<PackIssue[]> {
  const issues: PackIssue[] = [];
  const fail = (message: string, file?: string): void => {
    issues.push(file === undefined ? { pack: manifest.slug, message } : { pack: manifest.slug, file, message });
  };
  files = files.filter((f) => !isIgnoredFile(f.path));
  const sizes = new Map(files.map((f) => [f.path, f.size]));
  const listed = new Set(manifest.patches.map((p) => p.file));

  let total = 0;
  for (const { file } of manifest.patches) {
    const size = sizes.get(file);
    if (size === undefined) {
      const near = files.find((f) => f.path.toLowerCase() === file.toLowerCase());
      fail(near ? `listed in pack.json but the file is ${near.path}; the spelling differs only in case` : 'listed in pack.json but missing', file);
      continue;
    }
    total += size;
    if (size > MAX_FILE_BYTES) {
      fail(`is ${overLimit(size, MAX_FILE_BYTES)}`, file);
      continue;
    }
    let parsed: ParsedPackPatch;
    try {
      parsed = await parse(file);
    } catch (error) {
      fail(`not a readable patch: ${error instanceof Error ? error.message : String(error)}`, file);
      continue;
    }
    const folder = patchKindOf(file);
    if (parsed.kind !== folder) {
      const what = parsed.kind === 'unknown' ? 'has no op-1 synth or drum metadata' : `is a ${parsed.kind} patch`;
      fail(`${what} but sits in ${folder}/`, file);
    }
  }

  for (const { path, size } of files) {
    if (path === MANIFEST_FILE || path === README_FILE) {
      if (size > MAX_TEXT_BYTES) fail(`is ${overLimit(size, MAX_TEXT_BYTES)}`, path);
      continue;
    }
    if (listed.has(path)) continue;
    // A case-only misspelling of a listed file that is itself missing was reported above; when the
    // listed spelling exists too, this one is an extra file.
    const variantOf = manifest.patches.find((p) => p.file.toLowerCase() === path.toLowerCase());
    if (variantOf && !sizes.has(variantOf.file)) continue;
    fail('not listed in pack.json; list it under "patches" or remove it', path);
  }

  if (total > MAX_PACK_BYTES) fail(`patches total ${overLimit(total, MAX_PACK_BYTES)}`);
  return issues;
}
