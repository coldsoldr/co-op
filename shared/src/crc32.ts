/**
 * zlib's CRC-32 (poly 0xEDB88320, init and final XOR 0xFFFFFFFF), as the kennel update protocol
 * and the .knl and .op1p plugin files use it. Shared so the plugin validator (CLI and browser) and
 * the USB MIDI client compute the same value.
 */

const CRC_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    table[n] = c >>> 0;
  }
  return table;
})();

/** zlib's CRC-32. Pass the previous result as `crc` to continue over more bytes. */
export function crc32(bytes: Uint8Array, crc = 0): number {
  let c = ~crc >>> 0;
  for (const b of bytes) c = (CRC_TABLE[(c ^ b) & 0xff] as number) ^ (c >>> 8);
  return ~c >>> 0;
}
