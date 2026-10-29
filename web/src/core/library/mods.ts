import { joinPath, type DeviceStore } from '../store/store';
import { CO_OP_PREFIX } from '../store/atomic';
import { MODS_FOLDER, OPUS_PLUGIN_EXTENSION } from '../profiles/op1-opus';
import { byCodePoint, messageOf } from '../util';

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
  const folder = (await store.list('')).find((e) => e.kind === 'dir' && e.name.toLowerCase() === MODS_FOLDER);
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
