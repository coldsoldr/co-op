// File System Access API members that Chromium ships but TypeScript's DOM lib does not declare.
// Local type names carry a CoOp prefix so a future lib.dom addition cannot clash with them.
// window.showDirectoryPicker and FileSystemFileHandle.move are deliberately not declared here;
// fs-access-store.ts reaches them through typed casts instead.

interface CoOpPermissionDescriptor {
  mode?: 'read' | 'readwrite';
}

interface FileSystemHandle {
  queryPermission(descriptor?: CoOpPermissionDescriptor): Promise<PermissionState>;
  requestPermission(descriptor?: CoOpPermissionDescriptor): Promise<PermissionState>;
}
