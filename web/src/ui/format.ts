import type { LibraryEntry } from '../core/library/library';
import { dirname } from '../core/store/store';

/** The local calendar date as YYYY-MM-DD, for download file names. */
export function dateStamp(date: Date): string {
  const pad = (n: number): string => String(n).padStart(2, '0');
  return `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())}`;
}

/** A byte count for people: '512 B', '12.3 KB', '1.5 MB'. */
export function formatSize(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

/** The patch root an entry lives under, for example 'synth' for 'synth/user/a.aif'. */
export function rootOf(entry: Pick<LibraryEntry, 'path'>): string {
  return dirname(dirname(entry.path));
}

/** The folder an entry lives in, for example 'synth/user'. */
export function folderOf(entry: Pick<LibraryEntry, 'path'>): string {
  return dirname(entry.path);
}

export interface EntryGroup {
  /** Folder path, for example 'synth/user'. */
  folder: string;
  entries: LibraryEntry[];
}

/** Entries grouped by folder, keeping the order in which folders first appear. */
export function groupByFolder(entries: readonly LibraryEntry[]): EntryGroup[] {
  const groups = new Map<string, LibraryEntry[]>();
  for (const entry of entries) {
    const folder = folderOf(entry);
    groups.set(folder, [...(groups.get(folder) ?? []), entry]);
  }
  return [...groups].map(([folder, list]) => ({ folder, entries: list }));
}

/** The name to show: the metadata name, falling back to the file name. */
export function displayName(entry: LibraryEntry): string {
  return entry.patch?.name || entry.fileName;
}

export interface CompatView {
  level: 'ok' | 'warn' | 'unsupported' | 'unreadable' | 'loading';
  /** Short badge text. */
  label: string;
  /** Why, when there is a reason; shown in the title and on hover or focus. */
  detail: string | null;
}

/** How an entry's compatibility is shown. */
export function compatView(entry: LibraryEntry): CompatView {
  if (entry.error !== null) return { level: 'unreadable', label: 'unreadable', detail: entry.error };
  if (!entry.compat) return { level: 'loading', label: 'reading', detail: null };
  const detail = entry.compat.reason ?? null;
  return { level: entry.compat.level, label: entry.compat.level, detail };
}

/** The warning for a patch whose metadata kind disagrees with its folder, or null. */
export function kindMismatchText(entry: LibraryEntry): string | null {
  if (!entry.kindMismatch || !entry.patch) return null;
  return `in ${entry.kind} folder, but metadata says ${entry.patch.kind}`;
}
