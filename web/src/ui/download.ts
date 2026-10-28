/** Saves a blob as a download with the given file name. */
export function saveBlob(blob: Blob, fileName: string): void {
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a');
  link.href = url;
  link.download = fileName;
  link.rel = 'noopener';
  document.body.append(link);
  link.click();
  link.remove();
  // Revoking at once can cancel the download in some browsers; give it a moment.
  setTimeout(() => URL.revokeObjectURL(url), 60_000);
}

/** A zip blob from bytes. */
export function zipBlob(bytes: Uint8Array): Blob {
  return new Blob([bytes as Uint8Array<ArrayBuffer>], { type: 'application/zip' });
}
