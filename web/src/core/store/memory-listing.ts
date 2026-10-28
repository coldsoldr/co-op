import type { DirectoryListing, DirEntry } from './listing';

/** A folder is an object; a file is the string 'file'. */
export type MemoryTree = { [name: string]: MemoryTree | 'file' };

const byCodePoint = (a: DirEntry, b: DirEntry): number =>
  a.name < b.name ? -1 : a.name > b.name ? 1 : 0;

export class MemoryListing implements DirectoryListing {
  constructor(private readonly tree: MemoryTree) {}

  async list(path: string): Promise<DirEntry[]> {
    let node: MemoryTree | 'file' = this.tree;
    for (const part of path.split('/').filter(Boolean)) {
      if (node === 'file' || !Object.hasOwn(node, part)) return [];
      node = node[part] as MemoryTree | 'file';
    }
    if (node === 'file') return [];
    return Object.entries(node)
      .map(([name, value]): DirEntry => ({ name, kind: value === 'file' ? 'file' : 'dir' }))
      .sort(byCodePoint);
  }
}
