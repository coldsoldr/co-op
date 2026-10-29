import { checkOpmod } from '@co-op/shared/plugins';
import { isWindowsReservedName } from '@co-op/shared/packs';
import { joinPath, type DeviceStore } from '../store/store';
import { CO_OP_PREFIX, writeAtomic } from '../store/atomic';
import { MODS_FOLDER, OPUS_PLUGIN_EXTENSION } from '../profiles/op1-opus';
import { byCodePoint, messageOf } from '../util';
import { OperationError } from './operations';

/** One opus plugin file in mods/. */
export interface OpusPlugin {
  path: string;
  name: string;
  sizeBytes: number;
  /** True when the file starts with the opus plugin magic 'OPMD'. */
  valid: boolean;
  /** Why the file could not be read, or null. */
  error: string | null;
}

export interface OpusPluginListing {
  plugins: OpusPlugin[];
  /** co-op temp or set-aside files anywhere under mods/ that recovery could not clear. */
  leftovers: string[];
}

const MAGIC = [0x4f, 0x50, 0x4d, 0x44]; // 'OPMD'

/** The drive's mods folder entry, matched ignoring case, if there is one. */
async function modsFolder(store: DeviceStore) {
  return (await store.list('')).find((e) => e.kind === 'dir' && e.name.toLowerCase() === MODS_FOLDER);
}

async function head(store: DeviceStore, path: string, n: number): Promise<Uint8Array> {
  return store.readHead ? store.readHead(path, n) : (await store.read(path)).subarray(0, n);
}

/** Every `.co-op-*` file below `folder`, at any depth, skipping other dot folders. */
async function leftoversUnder(store: DeviceStore, folder: string, out: string[]): Promise<void> {
  for (const entry of await store.list(folder)) {
    const path = joinPath(folder, entry.name);
    if (entry.kind === 'file' && entry.name.startsWith(CO_OP_PREFIX)) out.push(path);
    else if (entry.kind === 'dir' && !entry.name.startsWith('.')) await leftoversUnder(store, path, out);
  }
}

/**
 * The .opmod files directly in the drive's mods/ folder (any case), in code-point file-name order,
 * which is how opus loads them (whether FAT makes that order case-insensitive is unverified). Only
 * the first four bytes are read, for the magic. A file that cannot be read is listed with its
 * error; dot files are skipped, and co-op leftovers under mods/ are returned apart. Read-only.
 */
export async function listOpusPlugins(store: DeviceStore): Promise<OpusPluginListing> {
  const folder = await modsFolder(store);
  if (!folder) return { plugins: [], leftovers: [] };
  const names = (await store.list(folder.name))
    .filter((e) => e.kind === 'file' && !e.name.startsWith('.') && e.name.toLowerCase().endsWith(OPUS_PLUGIN_EXTENSION))
    .map((e) => e.name)
    .sort(byCodePoint);
  const plugins: OpusPlugin[] = [];
  for (const name of names) {
    const path = joinPath(folder.name, name);
    let sizeBytes = 0;
    try {
      sizeBytes = await store.size(path);
      const bytes = await head(store, path, MAGIC.length);
      const valid = MAGIC.every((byte, i) => bytes[i] === byte);
      plugins.push({ path, name, sizeBytes, valid, error: null });
    } catch (error) {
      plugins.push({ path, name, sizeBytes, valid: false, error: messageOf(error) });
    }
  }
  const leftovers: string[] = [];
  await leftoversUnder(store, folder.name, leftovers);
  return { plugins, leftovers: leftovers.sort(byCodePoint) };
}

/** A plugin file of the same name, in any case, is already in mods/. */
export class PluginExistsError extends OperationError {
  override name = 'PluginExistsError';
  /** The existing file, as the drive spells it. */
  readonly path: string;
  constructor(path: string) {
    super(`${path} already exists`);
    this.path = path;
  }
}

/** An .opmod file name, as the plugin rules allow it in plugin.json. */
const OPMOD_FILE = /^[A-Za-z0-9_][A-Za-z0-9_.-]{0,57}\.opmod$/;

/** The path of the entry in mods/ named `file` ignoring case (FAT does), or null. */
export async function findOpusPluginFile(store: DeviceStore, file: string): Promise<string | null> {
  const folder = await modsFolder(store);
  if (!folder) return null;
  const entry = (await store.list(folder.name)).find((e) => e.name.toLowerCase() === file.toLowerCase());
  return entry ? joinPath(folder.name, entry.name) : null;
}

/**
 * Copies an opus plugin into mods/ (the folder as the drive spells it, created when missing) with
 * writeAtomic, and returns its path. The bytes must start with the OPMD magic and the name must be
 * a plain .opmod file name. A file of the same name in any case is replaced only with `overwrite`,
 * keeping the drive's spelling; otherwise PluginExistsError. On a staged (dropped) source the
 * write is collected with the other changes. opus loads mods/ in file-name order.
 */
export async function copyOpusPlugin(
  store: DeviceStore,
  file: string,
  bytes: Uint8Array,
  { overwrite = false }: { overwrite?: boolean } = {},
): Promise<string> {
  if (!OPMOD_FILE.test(file) || isWindowsReservedName(file)) throw new OperationError(`"${file}" is not an opus plugin file name`);
  const problems = checkOpmod(bytes);
  if (problems.length > 0) throw new OperationError(`${file} ${problems.join('; ')}`);
  const folder = (await modsFolder(store))?.name ?? MODS_FOLDER;
  const entry = (await store.list(folder)).find((e) => e.name.toLowerCase() === file.toLowerCase());
  const target = joinPath(folder, entry?.name ?? file);
  if (entry?.kind === 'dir') throw new OperationError(`${target} is a folder`);
  if (entry && !overwrite) throw new PluginExistsError(target);
  await writeAtomic(store, target, bytes);
  return target;
}
