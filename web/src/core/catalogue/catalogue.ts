import { INDEX_FORMAT, PACK_KINDS, SLUG, patchKindOf, type PackIndex, type PackIndexEntry, type PackKind } from '@co-op/shared/packs';
import { assertPatchFileName, ensureFolder, freeName } from '../library/operations';
import { getProfile } from '../profiles/registry';
import { writeAtomic } from '../store/atomic';
import { basename, dirname, joinPath, splitPath, type DeviceStore } from '../store/store';

/** The catalogue could not be loaded, a download failed its check, or an install was refused. */
export class CatalogueError extends Error {
  override name = 'CatalogueError';
}

/** An install that failed part-way; `installed` lists the paths written before the failure. */
export class InstallError extends CatalogueError {
  override name = 'InstallError';
  constructor(
    message: string,
    readonly installed: string[],
    options?: ErrorOptions,
  ) {
    super(message, options);
  }
}

const withSlash = (baseUrl: string): string => (baseUrl.endsWith('/') ? baseUrl : `${baseUrl}/`);
const reason = (error: unknown): string => (error instanceof Error ? error.message : String(error));

async function get(url: string, what: string, fetchImpl: typeof fetch, init?: RequestInit): Promise<Response> {
  let response: Response;
  try {
    response = await (init ? fetchImpl(url, init) : fetchImpl(url));
  } catch (error) {
    throw new CatalogueError(`could not ${what}: ${reason(error)}`, { cause: error });
  }
  if (!response.ok) throw new CatalogueError(`could not ${what}: HTTP ${response.status}`);
  return response;
}

const isRecord = (value: unknown): value is Record<string, unknown> =>
  typeof value === 'object' && value !== null && !Array.isArray(value);
const SHA256 = /^[0-9a-f]{64}$/i;
const isStringList = (value: unknown): boolean => Array.isArray(value) && value.every((v) => typeof v === 'string');

/** A cheap shape check, so the UI never trips over a field of the wrong type. */
function isIndexEntry(value: unknown): value is PackIndexEntry {
  if (!isRecord(value)) return false;
  const strings = ['slug', 'name', 'author', 'description', 'license'].every((k) => typeof value[k] === 'string');
  if (!strings || typeof value.sizeBytes !== 'number' || !isStringList(value.firmware) || !isStringList(value.tags)) {
    return false;
  }
  return (
    Array.isArray(value.patches) &&
    value.patches.every(
      (p) =>
        isRecord(p) &&
        ['file', 'name', 'engine', 'sha256'].every((k) => typeof p[k] === 'string') &&
        SHA256.test(p.sha256 as string) &&
        (p.notes === undefined || typeof p.notes === 'string') &&
        (PACK_KINDS as readonly unknown[]).includes(p.kind) &&
        typeof p.sizeBytes === 'number',
    )
  );
}

/**
 * Loads `<baseUrl>packs/index.json` (baseUrl is the app's base, for example import.meta.env.BASE_URL)
 * and checks that it is an index this version understands. The index changes with every deploy,
 * so it is always revalidated with the server.
 */
export async function fetchIndex(baseUrl: string, fetchImpl: typeof fetch = fetch): Promise<PackIndex> {
  const response = await get(`${withSlash(baseUrl)}packs/index.json`, 'load the pack index', fetchImpl, { cache: 'no-cache' });
  let json: unknown;
  try {
    json = JSON.parse(await response.text());
  } catch (error) {
    throw new CatalogueError(`the pack index is not valid JSON: ${reason(error)}`, { cause: error });
  }
  if (!isRecord(json) || json.format !== INDEX_FORMAT || !Array.isArray(json.packs)) {
    throw new CatalogueError(`unsupported pack index (expected format ${INDEX_FORMAT})`);
  }
  json.packs.forEach((entry: unknown, i: number) => {
    if (!isIndexEntry(entry)) throw new CatalogueError(`pack index entry ${i + 1} is malformed`);
  });
  return json as unknown as PackIndex;
}

export interface PackFilter {
  /** Every whitespace-separated word must appear (ignoring case) in the name, author, description, a tag or a patch name. */
  text?: string;
  /** Packs with at least one patch of this kind. */
  kind?: PackKind;
  /** Packs that list this profile id. */
  firmware?: string;
  license?: string;
  tag?: string;
}

/** The packs that match every given criterion, in index order. */
export function filterPacks(index: PackIndex, filter: PackFilter): PackIndexEntry[] {
  const words = (filter.text ?? '').toLowerCase().split(/\s+/).filter(Boolean);
  return index.packs.filter((pack) => {
    if (filter.kind !== undefined && !pack.patches.some((p) => p.kind === filter.kind)) return false;
    if (filter.firmware !== undefined && !pack.firmware.includes(filter.firmware)) return false;
    if (filter.license !== undefined && pack.license !== filter.license) return false;
    if (filter.tag !== undefined && !pack.tags.includes(filter.tag)) return false;
    if (words.length === 0) return true;
    const haystack = [pack.name, pack.author, pack.description, ...pack.tags, ...pack.patches.map((p) => p.name)]
      .join('\n')
      .toLowerCase();
    return words.every((word) => haystack.includes(word));
  });
}

async function sha256Hex(bytes: ArrayBuffer): Promise<string> {
  const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
  return Array.from(digest, (b) => b.toString(16).padStart(2, '0')).join('');
}

function assertSlug(slug: string): void {
  if (!SLUG.test(slug)) throw new CatalogueError(`${JSON.stringify(slug)} is not a pack slug`);
}

/** The kind folder of a pack file path, which must be exactly synth/<name> or drum/<name>. */
function packFileKind(file: string): PackKind {
  const kind = patchKindOf(file);
  if (kind === null || file.split('/').length !== 2) throw new CatalogueError(`${file} is not in synth/ or drum/`);
  try {
    assertPatchFileName(basename(file));
  } catch (error) {
    throw new CatalogueError(`${file}: ${reason(error)}`, { cause: error });
  }
  return kind;
}

/**
 * Downloads one patch file of a pack and checks it against the SHA-256 from the index, so a stale
 * cache or a broken download never reaches the device. A mismatch is retried once bypassing the
 * HTTP cache, which covers a file cached from before a deploy.
 */
export async function fetchPatchBytes(
  baseUrl: string,
  slug: string,
  file: string,
  sha256: string,
  fetchImpl: typeof fetch = fetch,
): Promise<Uint8Array> {
  assertSlug(slug);
  packFileKind(file);
  if (typeof sha256 !== 'string' || sha256 === '') throw new CatalogueError(`${slug}/${file} has no SHA-256 in the pack index`);
  if (!SHA256.test(sha256)) throw new CatalogueError(`${slug}/${file}: ${JSON.stringify(sha256)} is not a SHA-256`);
  const url = `${withSlash(baseUrl)}packs/${[slug, ...splitPath(file)].map(encodeURIComponent).join('/')}`;
  const label = `${slug}/${file}`;
  for (const init of [undefined, { cache: 'no-cache' } as RequestInit]) {
    const buffer = await (await get(url, `download ${label}`, fetchImpl, init)).arrayBuffer();
    if ((await sha256Hex(buffer)) === sha256.toLowerCase()) return new Uint8Array(buffer);
  }
  throw new CatalogueError(`${label} does not match the pack index; reload the page and try again`);
}

export interface PatchFile {
  /** The path inside the pack, for example 'synth/cluster.aif'. */
  file: string;
  bytes: Uint8Array;
}

/**
 * Installs pack files onto a device store and returns the paths written, in order. Each file goes
 * into `targetFolderFor(kind)` (by default the profile's root for that kind plus the slug, such as
 * 'synth/test-tones'), matched to an existing folder ignoring case. Every file is checked and every
 * target folder is created before the first file is written. Bytes are written exactly as given
 * with writeAtomic, under a name that is free ignoring case, so an existing patch is never
 * replaced. A failure part-way throws an InstallError listing what was installed. A read-only
 * source must be wrapped in a StagedStore.
 */
export async function installPatches(
  store: DeviceStore,
  profileId: string,
  slug: string,
  files: readonly PatchFile[],
  targetFolderFor?: (kind: PackKind) => string,
): Promise<{ installed: string[] }> {
  assertSlug(slug);
  const profile = getProfile(profileId);
  if (!profile) throw new CatalogueError(`unknown device profile "${profileId}"`);
  const defaultFolder = (kind: PackKind): string => {
    const root = profile.layout.patchRoots.find((r) => r.kind === kind);
    if (!root) throw new CatalogueError(`${profile.label} has no folder for ${kind} patches`);
    return joinPath(root.path, slug);
  };
  const folderFor = targetFolderFor ?? defaultFolder;

  const plan = files.map(({ file, bytes }) => {
    const kind = packFileKind(file);
    const folder = joinPath(folderFor(kind));
    if (folder === '') throw new CatalogueError(`no target folder for ${kind} patches`);
    return { name: basename(file), bytes, folder };
  });

  const resolved = new Map<string, string>();
  for (const { folder } of plan) {
    if (!resolved.has(folder)) resolved.set(folder, await ensureFolder(store, dirname(folder), basename(folder)));
  }

  const installed: string[] = [];
  for (const { name, bytes, folder } of plan) {
    const target = resolved.get(folder) as string;
    try {
      const path = joinPath(target, await freeName(store, target, name));
      await writeAtomic(store, path, bytes);
      installed.push(path);
    } catch (error) {
      throw new InstallError(`installed ${installed.length} of ${plan.length} patches, then failed: ${reason(error)}`, installed, {
        cause: error,
      });
    }
  }
  return { installed };
}

/**
 * A warning to show before installing `pack` onto a device running `profileId`, or null when the
 * pack lists that profile.
 */
export function compatibilityWarning(pack: PackIndexEntry, profileId: string): string | null {
  if (pack.firmware.includes(profileId)) return null;
  const label = (id: string): string => getProfile(id)?.label ?? id;
  return `${pack.name} is made for ${pack.firmware.map(label).join(', ')}, not ${label(profileId)}. Its patches may not load or may sound different.`;
}
