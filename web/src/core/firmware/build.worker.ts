/// <reference lib="webworker" />
/**
 * Builds the coop package off the page's thread: packing 26 MB with LZMA in JavaScript takes a while.
 * In: { stock: ArrayBuffer, deltaUrl: string }. Out: progress messages, then done (the package's bytes,
 * transferred) or error.
 */
import lzmaSource from 'lzma/src/lzma_worker.js?raw';
import { buildCoopPackage, type BuildProgress, type BuildResult } from './build';
import { lzmaFromSource } from './lzma';

export type WorkerMessage =
  | ({ type: 'progress' } & BuildProgress)
  | { type: 'done'; result: BuildResult }
  | { type: 'error'; message: string };

declare const self: DedicatedWorkerGlobalScope;
const post = (message: WorkerMessage, transfer: Transferable[] = []): void => self.postMessage(message, transfer);

self.addEventListener('message', (event: MessageEvent<{ stock: ArrayBuffer; deltaUrl: string }>) => {
  void (async () => {
    try {
      const response = await fetch(event.data.deltaUrl);
      if (!response.ok) throw new Error(`could not load the coop delta (HTTP ${response.status}); check your connection and try again`);
      const delta = new Uint8Array(await response.arrayBuffer());
      const result = await buildCoopPackage(new Uint8Array(event.data.stock), {
        lzma: lzmaFromSource(lzmaSource),
        delta,
        onProgress: (progress) => post({ type: 'progress', ...progress }),
      });
      post({ type: 'done', result }, [result.bytes.buffer]);
    } catch (e) {
      post({ type: 'error', message: e instanceof Error ? e.message : String(e) });
    }
  })();
});
