/**
 * Just enough of FileSystemDirectoryHandle and FileSystemFileHandle for FsAccessStore, held in
 * memory and failing the way Chromium does: DOMException 'NotFoundError' for missing entries and
 * 'TypeMismatchError' when a file is asked for as a folder or the other way round.
 */

const domError = (name: string, message: string): DOMException => new DOMException(message, name);

export interface FakeOptions {
  /** Give file handles a `move` method, as Chromium 110+ does. Defaults to true. */
  withMove?: boolean;
  /**
   * FAT behaviour, as on the OP-1 in disk mode: lookups ignore case, names keep the case they were
   * created with, and creating 'b.aif' when 'B.aif' exists returns the existing entry. Defaults to false.
   */
  caseInsensitive?: boolean;
  /** When set, `move` rejects with a DOMException of this name (e.g. 'NotSupportedError'). */
  moveError?: string;
  /** When set, the writable stream's `write` rejects with a DOMException of this name. */
  writeError?: string;
}

type ResolvedOptions = Required<Pick<FakeOptions, 'withMove' | 'caseInsensitive'>> &
  Pick<FakeOptions, 'moveError' | 'writeError'>;

type Child = FakeDirectoryHandle | FakeFileHandle;

export class FakeFileHandle {
  readonly kind = 'file';

  constructor(
    public name: string,
    public data: Uint8Array,
    private parent: FakeDirectoryHandle,
    private readonly options: ResolvedOptions,
  ) {
    if (!options.withMove) Object.defineProperty(this, 'move', { value: undefined });
  }

  async getFile(): Promise<File> {
    return new File([this.data.slice()], this.name);
  }

  /** Like Chromium, writes go to a swap copy that only replaces the file on close. */
  async createWritable(): Promise<{
    write(data: Uint8Array): Promise<void>;
    close(): Promise<void>;
    abort(): Promise<void>;
  }> {
    let pending = new Uint8Array(0);
    let finished = false;
    return {
      write: async (data: Uint8Array) => {
        if (this.options.writeError) throw domError(this.options.writeError, 'write failed');
        pending = data.slice();
      },
      close: async () => {
        if (finished) throw domError('TypeError', 'stream already closed');
        finished = true;
        this.data = pending;
      },
      abort: async () => {
        finished = true;
      },
    };
  }

  async move(destination: FakeDirectoryHandle, name: string): Promise<void> {
    if (this.options.moveError) throw domError(this.options.moveError, 'move failed');
    if (this.parent.lookup(this.name) !== this) throw domError('NotFoundError', 'source gone');
    this.parent.children.delete(this.parent.keyOf(this.name));
    destination.children.set(destination.keyOf(name), this);
    this.parent = destination;
    this.name = name;
  }
}

export class FakeDirectoryHandle {
  readonly kind = 'directory';
  /** Keyed by name, or by lower-cased name in case-insensitive mode. */
  readonly children = new Map<string, Child>();

  constructor(
    readonly name: string,
    readonly options: ResolvedOptions,
  ) {}

  keyOf(name: string): string {
    return this.options.caseInsensitive ? name.toLowerCase() : name;
  }

  lookup(name: string): Child | undefined {
    return this.children.get(this.keyOf(name));
  }

  async getDirectoryHandle(name: string, options: { create?: boolean } = {}): Promise<FakeDirectoryHandle> {
    const child = this.lookup(name);
    if (child instanceof FakeDirectoryHandle) return child;
    if (child) throw domError('TypeMismatchError', `${name} is a file`);
    if (!options.create) throw domError('NotFoundError', `${name} not found`);
    const created = new FakeDirectoryHandle(name, this.options);
    this.children.set(this.keyOf(name), created);
    return created;
  }

  /** Like Chromium, `create: true` makes an empty file straight away. */
  async getFileHandle(name: string, options: { create?: boolean } = {}): Promise<FakeFileHandle> {
    const child = this.lookup(name);
    if (child instanceof FakeFileHandle) return child;
    if (child) throw domError('TypeMismatchError', `${name} is a folder`);
    if (!options.create) throw domError('NotFoundError', `${name} not found`);
    const created = new FakeFileHandle(name, new Uint8Array(0), this, this.options);
    this.children.set(this.keyOf(name), created);
    return created;
  }

  async removeEntry(name: string): Promise<void> {
    const child = this.lookup(name);
    if (!child) throw domError('NotFoundError', `${name} not found`);
    if (child instanceof FakeDirectoryHandle && child.children.size > 0) {
      throw domError('InvalidModificationError', `${name} is not empty`);
    }
    this.children.delete(this.keyOf(name));
  }

  async *entries(): AsyncGenerator<[string, Child]> {
    // Chromium yields in no particular order; reverse insertion order makes sorting observable.
    for (const child of [...this.children.values()].reverse()) yield [child.name, child];
  }
}

/** Builds a fake root holding the given files, typed as the real handle for the store. */
export function fakeDirectory(
  files: Record<string, Uint8Array>,
  name = 'OP-1',
  options: FakeOptions = {},
): { root: FakeDirectoryHandle; handle: FileSystemDirectoryHandle } {
  const resolved: ResolvedOptions = {
    withMove: options.withMove ?? true,
    caseInsensitive: options.caseInsensitive ?? false,
    ...(options.moveError === undefined ? {} : { moveError: options.moveError }),
    ...(options.writeError === undefined ? {} : { writeError: options.writeError }),
  };
  const root = new FakeDirectoryHandle(name, resolved);
  for (const [path, bytes] of Object.entries(files)) {
    const parts = path.split('/').filter(Boolean);
    const fileName = parts.pop();
    if (fileName === undefined) continue;
    let dir = root;
    for (const part of parts) {
      let next = dir.lookup(part);
      if (!next) {
        next = new FakeDirectoryHandle(part, resolved);
        dir.children.set(dir.keyOf(part), next);
      }
      if (!(next instanceof FakeDirectoryHandle)) throw new Error(`fake: ${part} is a file`);
      dir = next;
    }
    if (dir.lookup(fileName)) throw new Error(`fake: ${path} given twice`);
    dir.children.set(dir.keyOf(fileName), new FakeFileHandle(fileName, bytes.slice(), dir, resolved));
  }
  return { root, handle: root as unknown as FileSystemDirectoryHandle };
}
