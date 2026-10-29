import type { DirectoryListing } from './listing';

export interface StoreCapabilities {
  /** False for read-only sources such as a dropped folder. */
  write: boolean;
}

export class StoreError extends Error {
  override name = 'StoreError';
}

/** A device drive. Paths are '/'-separated, relative to the drive root, never starting with '/'. */
export interface DeviceStore extends DirectoryListing {
  readonly capabilities: StoreCapabilities;
  /** Human label for the source, e.g. the picked folder name. */
  readonly label: string;
  /** Throws StoreError if the path is missing or is a folder. */
  read(path: string): Promise<Uint8Array>;
  /**
   * The first `n` bytes of a file (fewer when the file is shorter), without reading the rest.
   * Optional: callers fall back to read. Throws StoreError if the path is missing or is a folder.
   */
  readHead?(path: string, n: number): Promise<Uint8Array>;
  /** Size in bytes without reading the file. Throws StoreError if missing. */
  size(path: string): Promise<number>;
  /** True when the path names an existing file or folder. */
  exists(path: string): Promise<boolean>;
  /** Creates parent folders as needed; overwrites. Throws StoreError when capabilities.write is false. */
  write(path: string, bytes: Uint8Array): Promise<void>;
  /** Moves a file, creating the target's parent folders; throws if `to` exists. */
  move(from: string, to: string): Promise<void>;
  /** Removes a file (not folders). */
  remove(path: string): Promise<void>;
  /** Creates a folder and its parents; no-op if present. */
  mkdir(path: string): Promise<void>;
}

/** The non-empty segments of a path. */
export function splitPath(path: string): string[] {
  return path.split('/').filter(Boolean);
}

/** Joins parts with '/', collapsing duplicate separators and stripping leading and trailing ones. */
export function joinPath(...parts: string[]): string {
  return parts.flatMap(splitPath).join('/');
}

/** The parent folder of a path; '' for a top-level name. */
export function dirname(path: string): string {
  return splitPath(path).slice(0, -1).join('/');
}

/** The last segment of a path. */
export function basename(path: string): string {
  return splitPath(path).at(-1) ?? '';
}
