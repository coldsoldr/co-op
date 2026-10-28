// File System Access API members that Chromium ships but TypeScript's DOM lib does not declare.

interface FileSystemHandlePermissionDescriptor {
  mode?: 'read' | 'readwrite';
}

interface FileSystemHandle {
  queryPermission(descriptor?: FileSystemHandlePermissionDescriptor): Promise<PermissionState>;
  requestPermission(descriptor?: FileSystemHandlePermissionDescriptor): Promise<PermissionState>;
}

interface FileSystemFileHandle {
  /** Chromium 110+; absent elsewhere, so callers must feature-detect. */
  move?(destination: FileSystemDirectoryHandle, name: string): Promise<void>;
}

interface DirectoryPickerOptions {
  id?: string;
  mode?: 'read' | 'readwrite';
  startIn?: string;
}

interface Window {
  /** Optional so feature detection type-checks; only Chromium-based browsers have it. */
  showDirectoryPicker?(options?: DirectoryPickerOptions): Promise<FileSystemDirectoryHandle>;
}
