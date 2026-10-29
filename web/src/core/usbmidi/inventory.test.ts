import { describe, it, expect } from 'vitest';
import { parseInventory, savedNameOf } from './inventory';

const line = (...fields: string[]): string => fields.join('\t');

const HEADER = line('kennel-inventory 1', 'core 01e00000 0003a000 1a2b3c4d', 'presets 00042 cap 00300');

const RINGS = line(
  'synth',
  'rings',
  'knl',
  'plugins/rings.knl',
  '1.2',
  'coldsoldr',
  '111768',
  'deadbeef',
  'loaded',
  '+engine:rings',
  '',
);

describe('parseInventory', () => {
  it('reads the header and every record field', () => {
    const inventory = parseInventory(`${HEADER}\n${RINGS}\n`);
    expect(inventory.core).toEqual({ start: 0x01e00000, len: 0x3a000, crc: 0x1a2b3c4d });
    expect(inventory.presets).toEqual({ count: 42, cap: 300 });
    expect(inventory.records).toEqual([
      {
        kind: 'synth',
        name: 'rings',
        source: 'knl',
        path: 'plugins/rings.knl',
        version: '1.2',
        author: 'coldsoldr',
        size: 111768,
        crc: 0xdeadbeef,
        state: 'loaded',
        claims: [{ name: 'engine:rings', state: 'held' }],
        reason: '',
      },
    ]);
  });

  it('reads claims, built-ins, reasons and unparsed files', () => {
    const text = [
      HEADER,
      line('feature', 'marbles', 'knl', 'plugins/marbles.knl', '', '', '141508', '00000001', 'off', '!page:t4,~seq', 'lost page:t4 to chord'),
      line('-', 'broken', 'knl', 'plugins/broken.knl', '', '', '12', '0', 'failed', '', 'CRC mismatch'),
      line('fx', 'drive', 'builtin', '', '', '', '0', '0', 'loaded', '', ''),
      line('synth', 'odd', 'mystery', 'content/odd.op1p', '', '', 'x', 'zz', 'weird', '?x', ''),
    ].join('\n');
    const [marbles, broken, drive, odd] = parseInventory(text).records;
    expect(marbles).toMatchObject({
      kind: 'feature',
      state: 'off',
      claims: [
        { name: 'page:t4', state: 'lost' },
        { name: 'seq', state: 'not held' },
      ],
      reason: 'lost page:t4 to chord',
    });
    expect(broken).toMatchObject({ kind: null, state: 'failed', reason: 'CRC mismatch', size: 12 });
    expect(drive).toMatchObject({ kind: 'fx', source: 'builtin', path: '', size: 0, crc: 0, claims: [] });
    expect(odd).toMatchObject({ source: null, state: null, size: 0, crc: 0, claims: [{ name: 'x', state: 'not held' }] });
  });

  it('fills missing trailing fields with nothing', () => {
    const [record] = parseInventory(`kennel-inventory 1\n${line('fx', 'drive', 'builtin')}`).records;
    expect(record).toMatchObject({ name: 'drive', path: '', version: '', state: null, claims: [], reason: '' });
  });

  it('ignores unknown header fields and reads a header without core or presets', () => {
    const inventory = parseInventory(`${line('kennel-inventory 1', 'future 1 2 3')}\n${RINGS}`);
    expect(inventory.core).toBeNull();
    expect(inventory.presets).toBeNull();
    expect(inventory.records).toHaveLength(1);
  });

  it('finds header fields in any order and skips malformed ones', () => {
    const swapped = parseInventory(line('kennel-inventory 1', 'presets 00007 cap 00100', 'core 1 2 3'));
    expect(swapped).toMatchObject({ core: { start: 1, len: 2, crc: 3 }, presets: { count: 7, cap: 100 } });
    const malformed = parseInventory(line('kennel-inventory 1', 'core 1 2', 'presets 5 max 9'));
    expect(malformed).toMatchObject({ core: null, presets: null, records: [] });
  });

  it('reads CRLF line ends and skips blank lines', () => {
    expect(parseInventory(`${HEADER}\r\n\r\n${RINGS}\r\n`).records).toHaveLength(1);
  });

  it('refuses a text that is not a kennel inventory', () => {
    expect(() => parseInventory('')).toThrow(/format co-op does not know/);
    expect(() => parseInventory(`kennel-inventory 2\n${RINGS}`)).toThrow(/format co-op does not know/);
  });
});

describe('savedNameOf', () => {
  it('turns underscores into spaces', () => {
    expect(savedNameOf('tape_echo')).toBe('tape echo');
    expect(savedNameOf('rings')).toBe('rings');
  });

  it('saves drumplus as drum+', () => {
    expect(savedNameOf('drumplus')).toBe('drum+');
  });
});
