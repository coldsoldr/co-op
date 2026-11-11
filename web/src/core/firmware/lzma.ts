/**
 * LZMA "alone" coding for OP-1 packages, from LZMA-JS (the `lzma` package, MIT, Nathan Rugg). LZMA-JS is a
 * script that defines a global; it is run here from its source text with its own `this`, and with
 * `onmessage` shadowed so it never takes over the worker it runs in.
 *
 * Its preset 7 is exactly what TE's own op1_246.op1 uses: lc 3, lp 0, pb 2, an 8 MiB dictionary, an end
 * marker. LZMA-JS writes the length in the header; TE's says "unknown" (all ones) and ends on the marker,
 * so the header is rewritten to match. The stream itself does not depend on that field.
 */

export interface Lzma {
  compress: (data: Uint8Array, onProgress?: (fraction: number) => void) => Promise<Uint8Array>;
  decompress: (data: Uint8Array) => Promise<Uint8Array>;
}

interface LzmaJs {
  compress: (data: Uint8Array, mode: number, done: (result: number[] | null, error?: unknown) => void, progress: (p: number) => void) => void;
  decompress: (data: Uint8Array, done: (result: number[] | string | null, error?: unknown) => void) => void;
}

/** TE's settings, as LZMA-JS's preset 7 has them. */
const PRESET = 7;

/** An Lzma from LZMA-JS's source (`lzma/src/lzma_worker.js`). */
export function lzmaFromSource(source: string): Lzma {
  const scope: { LZMA?: LzmaJs } = {};
  new Function('onmessage', source).call(scope, undefined);
  const lib = scope.LZMA;
  if (!lib) throw new Error('LZMA-JS did not load');
  return {
    compress: (data, onProgress) =>
      new Promise((resolve, reject) => {
        lib.compress(
          data,
          PRESET,
          (result, error) => {
            if (error || !result) return reject(error instanceof Error ? error : new Error('compression failed'));
            const out = Uint8Array.from(result, (b) => b & 0xff);
            out.fill(0xff, 5, 13);
            resolve(out);
          },
          (p) => onProgress?.(p),
        );
      }),
    decompress: (data) =>
      new Promise((resolve, reject) => {
        lib.decompress(data, (result, error) => {
          if (error || result === null) return reject(error instanceof Error ? error : new Error('decompression failed'));
          // LZMA-JS hands back text when the bytes read as UTF-8; a tar never does (its headers hold NULs).
          if (typeof result === 'string') return reject(new Error('the firmware decoded as text, not an archive'));
          resolve(Uint8Array.from(result, (b) => b & 0xff));
        });
      }),
  };
}
