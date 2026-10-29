/**
 * The inventory text a hundekraft 3.x OP-1 serves over LIST: what its last boot found and decided.
 * Tab separated; a header line whose fields are found by their first word, then one line per
 * plugin file or built-in:
 *
 *     kennel-inventory 1<TAB>core <start hex8> <len hex8> <crc hex8><TAB>presets <count> cap <cap>
 *     kind  name  source  path  version  author  size  crc  state  claims  reason
 */

export type InventoryKind = 'synth' | 'fx' | 'feature';
export type InventorySource = 'builtin' | 'knl' | 'op1p';
/** `skipped`: the last boot did not reach the UI, so this boot started without plugin files. */
export type InventoryState = 'loaded' | 'failed' | 'skipped' | 'off';
export type ClaimState = 'held' | 'lost' | 'not held';

export interface InventoryClaim {
  name: string;
  state: ClaimState;
}

export interface InventoryRecord {
  /** null when the file did not parse (`-`) or the kind is unknown. */
  kind: InventoryKind | null;
  /** The plugin id, or a built-in's name. */
  name: string;
  /** null when unknown. */
  source: InventorySource | null;
  /** `plugins/<file>` or `content/<file>`; empty for built-ins. */
  path: string;
  /** From the plugin file; empty when it has none. */
  version: string;
  author: string;
  /** File size in bytes; 0 for built-ins. */
  size: number;
  /** The CRC-32 the file carries; 0 for built-ins. */
  crc: number;
  /** null when unknown. */
  state: InventoryState | null;
  claims: InventoryClaim[];
  /** Why it failed or is off, or a note such as "replaces the built-in"; empty otherwise. */
  reason: string;
}

export interface Inventory {
  /** Where the running core sits and its CRC; null when the header has no core field. */
  core: { start: number; len: number; crc: number } | null;
  /** The user synth preset count and the cap of the save gate; null on older cores. */
  presets: { count: number; cap: number } | null;
  records: InventoryRecord[];
}

const MAGIC = 'kennel-inventory 1';

const KINDS: readonly string[] = ['synth', 'fx', 'feature'];
const SOURCES: readonly string[] = ['builtin', 'knl', 'op1p'];
const STATES: readonly string[] = ['loaded', 'failed', 'skipped', 'off'];
const CLAIM_STATES: Record<string, ClaimState> = { '+': 'held', '!': 'lost', '~': 'not held' };

const oneOf = <T extends string>(values: readonly string[], value: string): T | null =>
  values.includes(value) ? (value as T) : null;

const HEX8 = /^[0-9a-fA-F]{1,8}$/;
const DECIMAL = /^\d+$/;

function parseCore(field: string | undefined): Inventory['core'] {
  const parts = field?.split(' ') ?? [];
  if (parts.length !== 4 || !parts.slice(1).every((p) => HEX8.test(p))) return null;
  const [start, len, crc] = parts.slice(1).map((p) => parseInt(p, 16)) as [number, number, number];
  return { start, len, crc };
}

function parsePresets(field: string | undefined): Inventory['presets'] {
  const parts = field?.split(' ') ?? [];
  if (parts.length !== 4 || parts[2] !== 'cap' || !DECIMAL.test(parts[1] ?? '') || !DECIMAL.test(parts[3] ?? '')) return null;
  return { count: Number(parts[1]), cap: Number(parts[3]) };
}

function parseRecord(line: string): InventoryRecord {
  const [kind = '', name = '', source = '', path = '', version = '', author = '', size = '', crc = '', state = '', claims = '', reason = ''] =
    line.split('\t');
  return {
    kind: oneOf<InventoryKind>(KINDS, kind),
    name,
    source: oneOf<InventorySource>(SOURCES, source),
    path,
    version,
    author,
    size: DECIMAL.test(size) ? Number(size) : 0,
    crc: HEX8.test(crc) ? parseInt(crc, 16) : 0,
    state: oneOf<InventoryState>(STATES, state),
    claims: claims
      .split(',')
      .filter((c) => c !== '')
      .map((c) => ({ name: c.slice(1), state: CLAIM_STATES[c[0] ?? ''] ?? 'not held' })),
    reason,
  };
}

/** Parses the LIST text. Throws when the first line is not a format this code knows. */
export function parseInventory(text: string): Inventory {
  const [head = '', ...lines] = text.split(/\r?\n/);
  const fields = head.split('\t');
  if (fields[0] !== MAGIC) throw new Error('the OP-1 sent a plugin inventory in a format co-op does not know');
  // Fields are found by their first word; unknown ones are for newer pages.
  const field = (word: string): string | undefined => fields.find((f) => f.split(' ')[0] === word);
  return {
    core: parseCore(field('core')),
    presets: parsePresets(field('presets')),
    records: lines.filter((line) => line.trim() !== '').map(parseRecord),
  };
}

/** The name sounds save for a plugin id: underscores as spaces; `drumplus` saves as "drum+". */
export const savedNameOf = (id: string): string => (id === 'drumplus' ? 'drum+' : id.replaceAll('_', ' '));
