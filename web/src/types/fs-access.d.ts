// File System Access API members that Chromium ships but TypeScript's DOM lib does not declare.
// Local type names carry a CoOp prefix so a future lib.dom addition cannot clash with them.
// window.showDirectoryPicker is deliberately not declared here; fs-access-store.ts reaches it
// through a typed accessor instead.

interface CoOpPermissionDescriptor {
  mode?: 'read' | 'readwrite';
}

interface FileSystemHandle {
  queryPermission(descriptor?: CoOpPermissionDescriptor): Promise<PermissionState>;
  requestPermission(descriptor?: CoOpPermissionDescriptor): Promise<PermissionState>;
}

interface FileSystemFileHandle {
  /** Chromium 110+; absent elsewhere, so callers must feature-detect. */
  move?(destination: FileSystemDirectoryHandle, name: string): Promise<void>;
}
