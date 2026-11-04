import { messageOf } from '../util';

export interface BulkResult {
  /** What each operation returned: the new path after a move or copy, the old one after a delete. */
  done: string[];
  failed: { path: string; message: string }[];
}

/** Runs `op` on each path in turn. A failure is recorded and does not stop the rest. */
export async function forEachPath(
  paths: readonly string[],
  op: (path: string) => Promise<string>,
  onProgress?: (done: number, total: number) => void,
): Promise<BulkResult> {
  const result: BulkResult = { done: [], failed: [] };
  let done = 0;
  for (const path of paths) {
    try {
      result.done.push(await op(path));
    } catch (error) {
      result.failed.push({ path, message: messageOf(error) });
    }
    onProgress?.(++done, paths.length);
  }
  return result;
}
