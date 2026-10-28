import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

/** The bytes of a committed fixture, by path relative to test/fixtures (e.g. 'real/drum.aif'). */
export function fixtureBytes(name: string): Uint8Array {
  return new Uint8Array(readFileSync(fileURLToPath(new URL(`./fixtures/${name}`, import.meta.url))));
}
