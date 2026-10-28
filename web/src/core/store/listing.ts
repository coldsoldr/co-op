export interface DirEntry {
  name: string;
  kind: 'file' | 'dir';
}

/**
 * Read-only view of a directory tree. Paths are '/'-separated and relative to
 * the root; '' is the root itself. Entries come back sorted by name.
 */
export interface DirectoryListing {
  list(path: string): Promise<DirEntry[]>;
}
