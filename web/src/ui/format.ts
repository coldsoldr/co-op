import { dirname } from '../core/store/store';
import type { LibraryRow } from './rows';
import type { SoundTraits } from '../core/codec/sound-traits';

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

/** The patch root a row lives under, for example 'synth' for 'synth/user/a.aif'. */
export function rootOf(row: Pick<LibraryRow, 'path'>): string {
  return dirname(dirname(row.path));
}

/** The folder a row lives in, for example 'synth/user'. */
export function folderOf(row: Pick<LibraryRow, 'path'>): string {
  return dirname(row.path);
}

export interface RowGroup {
  /** Folder path, for example 'synth/user'. */
  folder: string;
  rows: LibraryRow[];
}

/** Rows grouped by folder, keeping the order in which folders first appear. */
export function groupByFolder(rows: readonly LibraryRow[]): RowGroup[] {
  const groups = new Map<string, LibraryRow[]>();
  for (const row of rows) {
    const folder = folderOf(row);
    groups.set(folder, [...(groups.get(folder) ?? []), row]);
  }
  return [...groups].map(([folder, list]) => ({ folder, rows: list }));
}

export interface CompatView {
  level: 'ok' | 'warn' | 'unsupported' | 'unreadable' | 'loading';
  /** Short badge text. */
  label: string;
  /** Why, when there is a reason; shown in the title and on hover or focus. */
  detail: string | null;
}

/** How a row's compatibility is shown. */
export function compatView(row: LibraryRow): CompatView {
  if (row.error !== null) return { level: 'unreadable', label: 'unreadable', detail: row.error };
  if (!row.compat) return { level: 'loading', label: 'reading', detail: null };
  const detail = row.compat.reason ?? null;
  return { level: row.compat.level, label: row.compat.level, detail };
}

/** The warning for a patch whose metadata kind disagrees with its folder, or null. */
export function kindMismatchText(row: LibraryRow): string | null {
  if (!row.kindMismatch || !row.info) return null;
  return `in ${row.kind} folder, but metadata says ${row.info.kind}`;
}

/** Label and value pairs explaining a patch's compatibility badge; missing traits are left out. */
export function traitDetails(traits: SoundTraits): Array<[string, string]> {
  const hex = (value: number): string => `0x${value.toString(16)}`;
  const lines: Array<[string, string]> = [['Engine', traits.engine || 'none']];
  if (traits.fx) lines.push(['Effect', traits.fx]);
  if (traits.lfo) lines.push(['LFO', traits.lfo]);
  if (traits.engineSelector !== null) lines.push(['Engine selector', hex(traits.engineSelector)]);
  if (traits.opusMark !== null) lines.push(['opus engine mark', hex(traits.opusMark)]);
  if (traits.fxSelector !== null) lines.push(['Effect selector', hex(traits.fxSelector)]);
  if (traits.hasGin) lines.push(['Gin kit', 'yes']);
  if (traits.hasOpus) lines.push(['Plugin', traits.opusPlugin ?? 'unknown (opus sound)']);
  if (traits.opusEffects.length > 0) lines.push(['opus effects', traits.opusEffects.join(', ')]);
  return lines;
}
