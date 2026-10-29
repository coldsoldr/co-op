import { describe, expect, it } from 'vitest';
import type { SoundTraits } from '../core/codec/sound-traits';
import type { Compat } from '../core/profiles/types';
import { rejudgeRows, type LibraryRow } from './rows';

const row = (path: string, compat: Compat | null, traits: SoundTraits | null, read = true): LibraryRow => ({
  path,
  fileName: path,
  folder: 'user',
  kind: 'synth',
  sizeBytes: 1,
  error: null,
  compat,
  kindMismatch: false,
  info: read ? { name: path, kind: 'synth', hasMeta: traits !== null, canonical: true, traits } : null,
});

const traits = { kind: 'synth', engine: 'rings' } as SoundTraits;

describe('rejudgeRows', () => {
  it('judges read rows again from their traits and keeps unchanged rows as they were', () => {
    const changed = row('a.aif', { level: 'warn', reason: 'needs plugin "rings"' }, traits);
    const same = row('b.aif', { level: 'ok' }, { ...traits, engine: 'cluster' });
    const unread = row('c.aif', null, null, false);
    const judge = (t: SoundTraits | null): Compat => (t?.engine === 'rings' ? { level: 'ok' } : { level: 'ok' });
    const out = rejudgeRows([changed, same, unread], judge);
    expect(out[0]).toEqual({ ...changed, compat: { level: 'ok' } });
    expect(out[1]).toBe(same);
    expect(out[2]).toBe(unread);
  });

  it('passes null traits for a read file without metadata', () => {
    const bare = row('d.aif', { level: 'ok' }, null);
    const seen: (SoundTraits | null)[] = [];
    rejudgeRows([bare], (t) => {
      seen.push(t);
      return { level: 'unsupported', reason: 'file has no op-1 metadata' };
    });
    expect(seen).toEqual([null]);
  });
});
