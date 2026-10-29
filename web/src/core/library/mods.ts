import { joinPath, type DeviceStore } from '../store/store';
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

const MAGIC = [0x4f, 0x50, 0x4d, 0x44]; // 'OPMD'

/**
 * The .opmod files directly in the drive's mods/ folder (any case), in file-name order, which is
 * the order opus loads them. Nothing is parsed beyond the four-byte magic. A file that cannot be
 * read is listed with its error; dot files are skipped. Read-only.
 */
export async function listOpusPlugins(store: DeviceStore): Promise<OpusPlugin[]> {
  const folder = (await store.list('')).find((e) => e.kind === 'dir' && e.name.toLowerCase() === MODS_FOLDER);
  if (!folder) return [];
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
      const bytes = await store.read(path);
      const valid = MAGIC.every((byte, i) => bytes[i] === byte);
      plugins.push({ path, name, sizeBytes, valid, error: null });
    } catch (error) {
      plugins.push({ path, name, sizeBytes, valid: false, error: messageOf(error) });
    }
  }
  return plugins;
}
