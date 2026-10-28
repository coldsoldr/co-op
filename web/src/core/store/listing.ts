export interface DirEntry {
  name: string;
  kind: 'file' | 'dir';
}

/**
 * Read-only view of a directory tree. Paths are '/'-separated and relative to
 * the root; '' is the root itself.
 *
 * `list` of a path that does not exist, or that names a file rather than a
 * folder, resolves to `[]`; implementations must catch their backend's
 * not-found errors rather than reject. Entries come back sorted by code-point
 * order of `name` (plain `<` / `>` comparison, not locale-aware), so callers
 * get the same order on every platform.
 */
export interface DirectoryListing {
  list(path: string): Promise<DirEntry[]>;
}
