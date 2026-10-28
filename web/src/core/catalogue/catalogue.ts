import { INDEX_FORMAT, patchKindOf, type PackIndex, type PackIndexEntry, type PackKind } from '@co-op/shared/packs';
import { assertPatchFileName, ensureFolder, freeName } from '../library/operations';
import { getProfile } from '../profiles/registry';
import { writeAtomic } from '../store/atomic';
import { basename, dirname, joinPath, splitPath, type DeviceStore } from '../store/store';

/** The catalogue could not be loaded, a download failed its check, or an install was refused. */
export class CatalogueError extends Error {
  override name = 'CatalogueError';
}

const withSlash = (baseUrl: string): string => (baseUrl.endsWith('/') ? baseUrl : `${baseUrl}/`);
const reason = (error: unknown): string => (error instanceof Error ? error.message : String(error));

async function get(url: string, what: string, fetchImpl: typeof fetch): Promise<Response> {
  let response: Response;
  try {
    response = await fetchImpl(url);
  } catch (error) {
    throw new CatalogueError(`could not ${what}: ${reason(error)}`, { cause: error });
  }
  if (!response.ok) throw new CatalogueError(`could not ${what}: HTTP ${response.status}`);
  return response;
}

/**
 * Loads `<baseUrl>packs/index.json` (baseUrl is the app's base, for example import.meta.env.BASE_URL)
 * and checks that it is an index this version understands.
 */
export async function fetchIndex(baseUrl: string, fetchImpl: typeof fetch = fetch): Promise<PackIndex> {
  const response = await get(`${withSlash(baseUrl)}packs/index.json`, 'load the pack index', fetchImpl);
  let json: unknown;
  try {
    json = JSON.parse(await response.text());
  } catch (error) {
    throw new CatalogueError(`the pack index is not valid JSON: ${reason(error)}`, { cause: error });
  }
  const index = json as Partial<PackIndex> | null;
  if (typeof index !== 'object' || index === null || index.format !== INDEX_FORMAT || !Array.isArray(index.packs)) {
    throw new CatalogueError(`unsupported pack index (expected format ${INDEX_FORMAT})`);
  }
  return index as PackIndex;
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

/**
 * Downloads one patch file of a pack and checks it against the SHA-256 from the index, so a stale
 * cache or a broken download never reaches the device.
 */
export async function fetchPatchBytes(
  baseUrl: string,
  slug: string,
  file: string,
  sha256: string,
  fetchImpl: typeof fetch = fetch,
): Promise<Uint8Array> {
  const path = [slug, ...splitPath(file)].map(encodeURIComponent).join('/');
  const label = `${slug}/${file}`;
  const response = await get(`${withSlash(baseUrl)}packs/${path}`, `download ${label}`, fetchImpl);
  const buffer = await response.arrayBuffer();
  if ((await sha256Hex(buffer)) !== sha256.toLowerCase()) {
    throw new CatalogueError(`${label} does not match the pack index; reload the page and try again`);
  }
  return new Uint8Array(buffer);
}

export interface PatchFile {
  /** The path inside the pack, for example 'synth/cluster.aif'. */
  file: string;
  bytes: Uint8Array;
}

/**
 * Installs pack files onto a device store and returns the paths written, in order. Each file goes
 * into `targetFolderFor(kind)` (by default the profile's root for that kind plus the slug, such as
 * 'synth/test-tones'), created if missing and matched to an existing folder ignoring case. Bytes
 * are written exactly as given with writeAtomic, under a name that is free ignoring case, so an
 * existing patch is never replaced. Every file is checked before anything is written. A
 * read-only source must be wrapped in a StagedStore.
 */
export async function installPatches(
  store: DeviceStore,
  profileId: string,
  slug: string,
  files: readonly PatchFile[],
  targetFolderFor?: (kind: PackKind) => string,
): Promise<{ installed: string[] }> {
  const profile = getProfile(profileId);
  if (!profile) throw new CatalogueError(`unknown device profile "${profileId}"`);
  const defaultFolder = (kind: PackKind): string => {
    const root = profile.layout.patchRoots.find((r) => r.kind === kind);
    if (!root) throw new CatalogueError(`${profile.label} has no folder for ${kind} patches`);
    return joinPath(root.path, slug);
  };
  const folderFor = targetFolderFor ?? defaultFolder;

  const plan = files.map(({ file, bytes }) => {
    const kind = patchKindOf(file);
    if (kind === null || splitPath(file).length !== 2) throw new CatalogueError(`${file} is not in synth/ or drum/`);
    assertPatchFileName(basename(file));
    const folder = joinPath(folderFor(kind));
    if (folder === '') throw new CatalogueError(`no target folder for ${kind} patches`);
    return { name: basename(file), bytes, folder };
  });

  const installed: string[] = [];
  for (const { name, bytes, folder } of plan) {
    const target = await ensureFolder(store, dirname(folder), basename(folder));
    const path = joinPath(target, await freeName(store, target, name));
    await writeAtomic(store, path, bytes);
    installed.push(path);
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
