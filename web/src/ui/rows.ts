import type { PatchKind } from '../core/codec/op1-meta';
import type { LibraryEntry } from '../core/library/library';

/** What the table needs from a parsed patch; the bytes are not kept. */
export interface PatchInfo {
  /** The metadata name, '' when there is none. */
  name: string;
  /** The metadata kind. */
  kind: PatchKind;
  hasMeta: boolean;
  /** False when the metadata is laid out differently from how co-op would write it. */
  canonical: boolean;
}

/** A library entry as the UI holds it: the parsed patch is reduced to PatchInfo. */
export interface LibraryRow extends Omit<LibraryEntry, 'patch'> {
  /** Null until the patch is read, and for unreadable files. */
  info: PatchInfo | null;
}

export function toRow(entry: LibraryEntry): LibraryRow {
  const { patch, ...rest } = entry;
  return {
    ...rest,
    info: patch
      ? { name: patch.name, kind: patch.kind, hasMeta: patch.meta !== null, canonical: patch.op1Canonical }
      : null,
  };
}

/** True once a row's patch has been read, successfully or not. */
export const isSettled = (row: LibraryRow): boolean => row.info !== null || row.error !== null;

/**
 * Merges a fresh listing with the rows already shown. A row is kept (with its parsed details) when
 * its path is listed again with the same size, it was read before, and it is not in `touched`;
 * every other listed entry becomes an unread row and is returned in `toLoad`. Rows are in listing
 * order.
 */
export function mergeRows(
  listed: readonly LibraryEntry[],
  previous: ReadonlyMap<string, LibraryRow>,
  touched: ReadonlySet<string>,
): { rows: LibraryRow[]; toLoad: LibraryEntry[] } {
  const rows: LibraryRow[] = [];
  const toLoad: LibraryEntry[] = [];
  for (const entry of listed) {
    const before = previous.get(entry.path);
    if (before && !touched.has(entry.path) && before.sizeBytes === entry.sizeBytes && isSettled(before)) {
      rows.push(before);
    } else {
      rows.push(toRow(entry));
      toLoad.push(entry);
    }
  }
  return { rows, toLoad };
}

/** The name to show: the metadata name, falling back to the file name. */
export function displayName(row: LibraryRow): string {
  return row.info?.name || row.fileName;
}

/** Rows whose metadata name or file name contains `query`, ignoring case; all rows for ''. */
export function filterRows(rows: readonly LibraryRow[], query: string): LibraryRow[] {
  const needle = query.trim().toLowerCase();
  if (needle === '') return [...rows];
  return rows.filter(
    (row) => row.fileName.toLowerCase().includes(needle) || (row.info?.name ?? '').toLowerCase().includes(needle),
  );
}

export type SortKey = 'name' | 'file' | 'kind' | 'size' | 'compat';
export type SortDirection = 'ascending' | 'descending';

const COMPAT_RANK: Record<string, number> = { ok: 0, warn: 1, unsupported: 2, unreadable: 3, loading: 4 };

function compatRank(row: LibraryRow): number {
  if (row.error !== null) return COMPAT_RANK.unreadable as number;
  return COMPAT_RANK[row.compat?.level ?? 'loading'] as number;
}

const text = (a: string, b: string): number => a.localeCompare(b, undefined, { sensitivity: 'base', numeric: true });

const COMPARE: Record<SortKey, (a: LibraryRow, b: LibraryRow) => number> = {
  name: (a, b) => text(displayName(a), displayName(b)),
  file: (a, b) => text(a.fileName, b.fileName),
  kind: (a, b) => text(a.kind, b.kind),
  size: (a, b) => a.sizeBytes - b.sizeBytes,
  compat: (a, b) => compatRank(a) - compatRank(b),
};

/** A sorted copy; ties fall back to the path so the order is stable and total. */
export function sortRows(rows: readonly LibraryRow[], key: SortKey, direction: SortDirection): LibraryRow[] {
  const sign = direction === 'ascending' ? 1 : -1;
  return [...rows].sort((a, b) => sign * COMPARE[key](a, b) || (a.path < b.path ? -1 : a.path > b.path ? 1 : 0));
}
