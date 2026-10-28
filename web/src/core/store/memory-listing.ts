import type { DirectoryListing, DirEntry } from './listing';

/** A folder is an object; a file is the string 'file'. */
export type MemoryTree = { [name: string]: MemoryTree | 'file' };

export class MemoryListing implements DirectoryListing {
  constructor(private readonly tree: MemoryTree) {}

  async list(path: string): Promise<DirEntry[]> {
    let node: MemoryTree | 'file' = this.tree;
    for (const part of path.split('/').filter(Boolean)) {
      if (node === 'file') return [];
      const next: MemoryTree | 'file' | undefined = node[part];
      if (next === undefined) return [];
      node = next;
    }
    if (node === 'file') return [];
    return Object.entries(node)
      .map(([name, value]): DirEntry => ({ name, kind: value === 'file' ? 'file' : 'dir' }))
      .sort((a, b) => a.name.localeCompare(b.name));
  }
}
