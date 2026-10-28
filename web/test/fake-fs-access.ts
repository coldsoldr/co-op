/**
 * Just enough of FileSystemDirectoryHandle and FileSystemFileHandle for FsAccessStore, held in
 * memory and failing the way Chromium does: DOMException 'NotFoundError' for missing entries and
 * 'TypeMismatchError' when a file is asked for as a folder or the other way round.
 */

const domError = (name: string, message: string): DOMException => new DOMException(message, name);

export interface FakeOptions {
  /** Give file handles a `move` method, as Chromium 110+ does. Defaults to true. */
  withMove?: boolean;
}

export class FakeFileHandle {
  readonly kind = 'file';

  constructor(
    public name: string,
    public data: Uint8Array,
    private parent: FakeDirectoryHandle,
    withMove: boolean,
  ) {
    if (!withMove) Object.defineProperty(this, 'move', { value: undefined });
  }

  async getFile(): Promise<File> {
    return new File([this.data.slice()], this.name);
  }

  async createWritable(): Promise<{ write(data: Uint8Array): Promise<void>; close(): Promise<void> }> {
    let pending = new Uint8Array(0);
    return {
      write: async (data: Uint8Array) => {
        pending = data.slice();
      },
      close: async () => {
        this.data = pending;
      },
    };
  }

  async move(destination: FakeDirectoryHandle, name: string): Promise<void> {
    if (this.parent.children.get(this.name) !== this) throw domError('NotFoundError', 'source gone');
    this.parent.children.delete(this.name);
    destination.children.set(name, this);
    this.parent = destination;
    this.name = name;
  }
}

export class FakeDirectoryHandle {
  readonly kind = 'directory';
  readonly children = new Map<string, FakeDirectoryHandle | FakeFileHandle>();

  constructor(
    readonly name: string,
    readonly withMove: boolean,
  ) {}

  async getDirectoryHandle(name: string, options: { create?: boolean } = {}): Promise<FakeDirectoryHandle> {
    const child = this.children.get(name);
    if (child instanceof FakeDirectoryHandle) return child;
    if (child) throw domError('TypeMismatchError', `${name} is a file`);
    if (!options.create) throw domError('NotFoundError', `${name} not found`);
    const created = new FakeDirectoryHandle(name, this.withMove);
    this.children.set(name, created);
    return created;
  }

  async getFileHandle(name: string, options: { create?: boolean } = {}): Promise<FakeFileHandle> {
    const child = this.children.get(name);
    if (child instanceof FakeFileHandle) return child;
    if (child) throw domError('TypeMismatchError', `${name} is a folder`);
    if (!options.create) throw domError('NotFoundError', `${name} not found`);
    const created = new FakeFileHandle(name, new Uint8Array(0), this, this.withMove);
    this.children.set(name, created);
    return created;
  }

  async removeEntry(name: string): Promise<void> {
    const child = this.children.get(name);
    if (!child) throw domError('NotFoundError', `${name} not found`);
    if (child instanceof FakeDirectoryHandle && child.children.size > 0) {
      throw domError('InvalidModificationError', `${name} is not empty`);
    }
    this.children.delete(name);
  }

  async *entries(): AsyncGenerator<[string, FakeDirectoryHandle | FakeFileHandle]> {
    // Chromium yields in no particular order; reverse insertion order makes sorting observable.
    for (const entry of [...this.children.entries()].reverse()) yield entry;
  }
}

/** Builds a fake root holding the given files, typed as the real handle for the store. */
export function fakeDirectory(
  files: Record<string, Uint8Array>,
  name = 'OP-1',
  options: FakeOptions = {},
): { root: FakeDirectoryHandle; handle: FileSystemDirectoryHandle } {
  const root = new FakeDirectoryHandle(name, options.withMove ?? true);
  for (const [path, bytes] of Object.entries(files)) {
    const parts = path.split('/').filter(Boolean);
    const fileName = parts.pop();
    if (fileName === undefined) continue;
    let dir = root;
    for (const part of parts) {
      let next = dir.children.get(part);
      if (!next) {
        next = new FakeDirectoryHandle(part, root.withMove);
        dir.children.set(part, next);
      }
      if (!(next instanceof FakeDirectoryHandle)) throw new Error(`fake: ${part} is a file`);
      dir = next;
    }
    dir.children.set(fileName, new FakeFileHandle(fileName, bytes.slice(), dir, root.withMove));
  }
  return { root, handle: root as unknown as FileSystemDirectoryHandle };
}
